#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/stat.h>
#include <string.h>
#include <gtk/gtk.h>
#include <libgnomecanvas/libgnomecanvas.h>

#include "xournal.h"
#include "xo-interface.h"
#include "xo-support.h"
#include "xo-callbacks.h"
#include "xo-misc.h"
#include "xo-file.h"
#include "xo-paint.h"
#include "xo-shapes.h"
#include "xo-print.h"

static gint openAtPageNumber = 1;
static const char **fileArguments = NULL;


static gchar* exportPdfFile;
static gboolean show_gui = TRUE;

static GOptionEntry entries[] =
{
  { "page", 'p', 0, G_OPTION_ARG_INT, &openAtPageNumber, "Jump to Page", "N" },
  { "export-pdf", 'A', 0, G_OPTION_ARG_STRING, &exportPdfFile, "Export document to a PDF file", "FILENAME" },
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &fileArguments, NULL, N_("[FILE]") },
  { NULL }
};


GtkWidget *winMain;
GnomeCanvas *canvas;

struct Journal journal; // the journal
struct BgPdf bgpdf;  // the PDF loader stuff
struct UIData ui;   // the user interface data
struct UndoItem *undo, *redo; // the undo and redo stacks
  GError  *error = NULL;
  GOptionContext *context;

double DEFAULT_ZOOM;

void init_stuff (int argc, char *argv[])
{
  GtkWidget *w;
  GList *dev_list;
  GdkDevice *device;
  GdkScreen *screen;
  int i, j;
  struct Brush *b;
  gboolean can_xinput = FALSE, success = FALSE;
  gchar *tmppath, *tmpfn;
  GError  *error = NULL;
  GOptionContext *context;

  // create some data structures needed to populate the preferences
  ui.default_page.bg = g_new(struct Background, 1);
  ui.xournal_exe_cmd = argv[0];

  // initialize config file names
  tmppath = g_build_filename(g_get_home_dir(), CONFIG_DIR, NULL);
  if(!g_file_test(tmppath, G_FILE_TEST_EXISTS))
    g_mkdir(tmppath, 0700); // safer (MRU data may be confidential)
  ui.mrufile = g_build_filename(tmppath, MRU_FILE, NULL);
  ui.configfile = g_build_filename(tmppath, CONFIG_FILE, NULL);
  g_free(tmppath);

  // initialize preferences
  init_config_default();
  load_config_from_file();
  ui.font_name = g_strdup(ui.default_font_name);
  ui.font_size = ui.default_font_size;
  ui.hiliter_alpha_mask = 0xffffff00 + (guint)(255*ui.hiliter_opacity);
  ui.image_one_to_one_zoom = DEFAULT_ZOOM;

  // we need an empty canvas prior to creating the journal structures
  canvas = GNOME_CANVAS (gnome_canvas_new_aa ());

  // initialize data
  ui.default_page.bg->canvas_item = NULL;
  ui.layerbox_length = 0;
  ui.autosave_defers = 0;
  ui.this_is_autosave = FALSE;

  // parse command line options
  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gtk_get_option_group (TRUE));
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
      printf(_("option parsing failed: %s\n"), error->message);
      exit (1);
  }
  
  undo = NULL; redo = NULL;
  journal.pages = NULL;
  bgpdf.status = STATUS_NOT_INIT;

  new_journal();

  if(exportPdfFile) {
    show_gui = FALSE;
  }

  // load the MRU
  if (show_gui) init_mru();

  // if any autosaves have been restored by spawning child processes, we exit
  // this instance of xournal.  Note: check autosaves only if in gui mode, and 
  // only if we aren't opening a file directly from command line
  if (show_gui && (fileArguments == NULL) && check_and_restore_autosaves())
    exit(1);

  if (fileArguments == NULL) {
    if(exportPdfFile) {
       // If exporting having a file to open is essential
       printf(_("Path to xoj file mssing\n"));
       exit(2);
     }
  } else {
    if (g_path_is_absolute(fileArguments[0])) {  
      tmpfn = g_strdup(fileArguments[0]);
    } else {
      tmppath = g_get_current_dir();
      tmpfn = g_build_filename(tmppath, fileArguments[0], NULL);
      g_free(tmppath);
    }

    success = open_file_or_its_autosave(tmpfn);
    g_free(tmpfn);
    if (!success && !show_gui) { // GUI error is showed later
      printf(_("Error opening file '%s'"), fileArguments[0]);
      exit(2);
    }
  }

  if(exportPdfFile) {
     exit(print_to_pdf(exportPdfFile));
  }

  ui.cur_item_type = ITEM_NONE;
  ui.cur_item = NULL;
  ui.cur_path.coords = NULL;
  ui.cur_path_storage_alloc = 0;
  ui.cur_path.ref_count = 1;
  ui.cur_widths = NULL;
  ui.cur_widths_storage_alloc = 0;

  ui.selection = NULL;
  ui.cursor = NULL;

  ui.cur_brush = &(ui.brushes[0][ui.toolno[0]]);
  for (j=0; j<=NUM_BUTTONS; j++)
    for (i=0; i < NUM_STROKE_TOOLS; i++) {
      b = &(ui.brushes[j][i]);
      b->tool_type = i;
      if (b->color_no>=0) {
        b->color_rgba = predef_colors_rgba[b->color_no];
        if (i == TOOL_HIGHLIGHTER) {
          b->color_rgba &= ui.hiliter_alpha_mask;
        }
      }
      b->thickness = predef_thickness[i][b->thickness_no];
    }
  for (i=0; i<NUM_STROKE_TOOLS; i++)
    g_memmove(ui.default_brushes+i, &(ui.brushes[0][i]), sizeof(struct Brush));

  ui.cur_mapping = 0;
  ui.which_unswitch_button = 0;
  
  reset_recognizer();

  // initialize various interface elements
  
  gtk_window_set_default_size(GTK_WINDOW (winMain), ui.window_default_width, ui.window_default_height);
  if (ui.maximize_at_start) gtk_window_maximize(GTK_WINDOW (winMain));
  update_toolbar_and_menu();
  update_font_button();

  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("journalApplyAllPages")), ui.bg_apply_all_pages);
  if (ui.fullscreen) {
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("viewFullscreen")), TRUE);
    gtk_toggle_tool_button_set_active(
      GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonFullscreen")), TRUE);
    gtk_window_fullscreen(GTK_WINDOW(winMain));
  }
  gtk_button_set_relief(GTK_BUTTON(GET_COMPONENT("buttonColorChooser")), GTK_RELIEF_NONE);

  allow_all_accels();
  add_scroll_bindings();

  // prevent interface items from stealing focus
  // glade doesn't properly handle can_focus, so manually set it
  gtk_combo_box_set_focus_on_click(GTK_COMBO_BOX(GET_COMPONENT("comboLayer")), FALSE);
  g_signal_connect(GET_COMPONENT("spinPageNo"), "activate",
          G_CALLBACK(handle_activate_signal), NULL);
  gtk_container_forall(GTK_CONTAINER(winMain), unset_flags, (gpointer)GTK_CAN_FOCUS);
  GTK_WIDGET_SET_FLAGS(GTK_WIDGET(canvas), GTK_CAN_FOCUS);
  GTK_WIDGET_SET_FLAGS(GTK_WIDGET(GET_COMPONENT("spinPageNo")), GTK_CAN_FOCUS);
  
  GTK_WIDGET_SET_FLAGS(GET_COMPONENT("findBar"), GTK_CAN_FOCUS);

  // install hooks on button/key/activation events to make the spinPageNo lose focus
  gtk_container_forall(GTK_CONTAINER(winMain), install_focus_hooks, NULL);

  // set up and initialize the canvas

  gtk_widget_show (GTK_WIDGET (canvas));
  w = GET_COMPONENT("scrolledwindowMain");
  gtk_container_add (GTK_CONTAINER (w), GTK_WIDGET (canvas));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW (w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_events (GTK_WIDGET (canvas), GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_KEY_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gnome_canvas_set_pixels_per_unit (canvas, ui.zoom);
  gnome_canvas_set_center_scroll_region (canvas, TRUE);
  gtk_layout_get_hadjustment(GTK_LAYOUT (canvas))->step_increment = ui.scrollbar_step_increment;
  gtk_layout_get_vadjustment(GTK_LAYOUT (canvas))->step_increment = ui.scrollbar_step_increment;

  // set up the page size and canvas size
  update_page_stuff();

  g_signal_connect ((gpointer) canvas, "button_press_event",
                    G_CALLBACK (on_canvas_button_press_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "button_release_event",
                    G_CALLBACK (on_canvas_button_release_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "enter_notify_event",
                    G_CALLBACK (on_canvas_enter_notify_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "leave_notify_event",
                    G_CALLBACK (on_canvas_leave_notify_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "expose_event",
                    G_CALLBACK (on_canvas_expose_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "key_press_event",
                    G_CALLBACK (on_canvas_key_press_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "motion_notify_event",
                    G_CALLBACK (on_canvas_motion_notify_event),
                    NULL);
  g_signal_connect ((gpointer) gtk_layout_get_vadjustment(GTK_LAYOUT(canvas)),
                    "value-changed", G_CALLBACK (on_vscroll_changed),
                    NULL);
  g_object_set_data (G_OBJECT (winMain), "canvas", canvas);

  screen = gtk_widget_get_screen(winMain);
  ui.screen_width = gdk_screen_get_width(screen);
  ui.screen_height = gdk_screen_get_height(screen);
  
  can_xinput = FALSE;
  dev_list = gdk_devices_list();
  while (dev_list != NULL) {
    device = (GdkDevice *)dev_list->data;
    if (device != gdk_device_get_core_pointer() && device->num_axes >= 2) {
      /* get around a GDK bug: map the valuator range CORRECTLY to [0,1] */
#ifdef ENABLE_XINPUT_BUGFIX
      gdk_device_set_axis_use(device, 0, GDK_AXIS_IGNORE);
      gdk_device_set_axis_use(device, 1, GDK_AXIS_IGNORE);
#endif
      gdk_device_set_mode(device, GDK_MODE_SCREEN);
      if (g_strrstr(device->name, "raser"))
        gdk_device_set_source(device, GDK_SOURCE_ERASER);
      can_xinput = TRUE;
    }
    dev_list = dev_list->next;
  }
  if (!can_xinput)
    gtk_widget_set_sensitive(GET_COMPONENT("optionsUseXInput"), FALSE);

  ui.use_xinput = ui.allow_xinput && can_xinput;

  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsProgressiveBG")), ui.progressive_bg);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsPrintRuling")), ui.print_ruling);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsAutoloadPdfXoj")), ui.autoload_pdf_xoj);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsAutoExportPdf")), ui.autoexport_pdf);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsTouchAsHandTool")), ui.touch_as_handtool);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsLeftHanded")), ui.left_handed);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsShortenMenus")), ui.shorten_menus);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsAutoSavePrefs")), ui.auto_save_prefs);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsButtonSwitchMapping")), ui.button_switch_mapping);
  
  hide_unimplemented();

  update_undo_redo_enabled();
  update_copy_paste_enabled();
  update_vbox_order(ui.vertical_order[ui.fullscreen?1:0]);
  gtk_widget_grab_focus(GTK_WIDGET(canvas));

  // show everything...
  
  gtk_widget_show (winMain);
  update_cursor();

  /* this will cause extension events to get enabled/disabled, but
     we need the windows to be mapped first */
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsUseXInput")), ui.use_xinput);

  /* fix a bug in GTK+ 2.16 and 2.17: scrollbars shouldn't get extended
     input events from pointer motion when cursor moves into main window */

  if (!gtk_check_version(2, 16, 0)) {
    g_signal_connect (
      GET_COMPONENT("menubar"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      GET_COMPONENT("toolbarMain"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      GET_COMPONENT("toolbarPen"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      GET_COMPONENT("statusbar"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      (gpointer)(gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(w))),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      (gpointer)(gtk_scrolled_window_get_hscrollbar(GTK_SCROLLED_WINDOW(w))),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
  }


  // here is the command line "option" is handled

  context = g_option_context_new ("Xournal");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gtk_get_option_group (TRUE));
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_printerr ("Cannot parse arguments: %s\n", error->message);
    g_error_free (error);
    g_option_context_free (context);
    gtk_exit(0);
  }
  g_option_context_free (context);

  // show error if loading file failed
  
  // if no file was provided, we are done
  if (fileArguments == NULL || fileArguments[0] == NULL) return;
  //  fprintf(stderr, "Fiel name %s\n", fileArguments[0]);
  /*

  set_cursor_busy(TRUE);
  if (g_path_is_absolute(fileArguments[0]))
    tmpfn = g_strdup(fileArguments[0]);
  else {
    tmppath = g_get_current_dir();
    tmpfn = g_build_filename(tmppath, fileArguments[0], NULL);
    g_free(tmppath);
  }
  success = open_journal(tmpfn);
  g_free(tmpfn);
  set_cursor_busy(FALSE);
  */
  if (!success) {
  
   // jump to desired page
   if (openAtPageNumber > journal.npages) {
     openAtPageNumber = journal.npages;
   }
   do_switch_page(openAtPageNumber - 1, TRUE, TRUE);
    w = gtk_message_dialog_new(GTK_WINDOW (winMain), GTK_DIALOG_DESTROY_WITH_PARENT,
       GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, _("Error opening file '%s'"), fileArguments[0]);
    gtk_dialog_run(GTK_DIALOG(w));
    gtk_widget_destroy(w);
  }
  // jump to desired page, if requested...
  if (openAtPageNumber > 1) {
    //fprintf(stderr, "Jumping to page %d out of %d pages \n", openAtPageNumber, journal.npages);
    if (openAtPageNumber > journal.npages)
      // if user wants to jump too far away, then use last page
      openAtPageNumber = journal.npages;
    do_switch_page(openAtPageNumber - 1, TRUE, TRUE);
  }
}


int
main (int argc, char *argv[])
{
  gchar *path, *path1, *path2;
  
#ifdef ENABLE_NLS
  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
#endif
  
  gtk_set_locale ();
  gtk_init (&argc, &argv);

  add_pixmap_directory (PACKAGE_DATA_DIR "/" PACKAGE "/pixmaps");
  path = g_path_get_dirname(argv[0]);
  path1 = g_build_filename(path, "pixmaps", NULL);
  path2 = g_build_filename(path, "..", "pixmaps", NULL);
  add_pixmap_directory (path1);
  add_pixmap_directory (path2);
  add_pixmap_directory (path);
  g_free(path);
  g_free(path1);
  g_free(path2);

  /*
   * The following code was added by Glade to create one of each component
   * (except popup menus), just so that you see something after building
   * the project. Delete any components that you don't want shown initially.
   */
  winMain = create_winMain ();
  
  init_stuff (argc, argv);
  
  gtk_window_set_icon(GTK_WINDOW(winMain), create_pixbuf("xournal.png"));
  
  gtk_main ();
  
  if (bgpdf.status != STATUS_NOT_INIT) shutdown_bgpdf();

  save_mru_list();
  if (ui.auto_save_prefs) save_config_to_file();

  clear_autosave_entry();
  
  return 0;
}

