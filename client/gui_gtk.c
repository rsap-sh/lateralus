/*
 * gui_gtk.c — Native GTK3 frontend for Lateralus voice chat (Linux).
 *
 * Uses GTK3 for a fully native Linux experience:
 *   - System font and theme (follows GNOME / KDE GTK theme)
 *   - HiDPI handled automatically by GTK
 *   - GLib main loop is fully event-driven — near-zero idle CPU
 *   - Settings persisted to ~/.config/voicechat/settings
 */

#include <gtk/gtk.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../common/vc_api.h"

/* ── Engine thread ──────────────────────────────────────────────────────── */

typedef struct { char host[256], room[64], user[64], psk[256]; } EngArgs;
static EngArgs      s_args;
static pthread_t    s_thread;
static volatile int s_running = 0;

static void *eng_fn(void *p)
{
    EngArgs *a = (EngArgs *)p;
    vc_run(a->host, a->room, a->user, a->psk);
    s_running = 0;
    return NULL;
}

/* ── Settings persistence ───────────────────────────────────────────────── */

static char s_cfg[1024];

static void cfg_init(void)
{
    const char *home = getenv("HOME");
    if (home) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/.config/voicechat", home);
        mkdir(dir, 0755);
        snprintf(s_cfg, sizeof s_cfg, "%s/settings", dir);
    } else {
        snprintf(s_cfg, sizeof s_cfg, "/tmp/voicechat-settings");
    }
}

static void cfg_load(GtkEntry *srv, GtkEntry *room, GtkEntry *user)
{
    FILE *f = fopen(s_cfg, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, "server=", 7) == 0) gtk_entry_set_text(srv,  line + 7);
        if (strncmp(line, "room=",   5) == 0) gtk_entry_set_text(room, line + 5);
        if (strncmp(line, "user=",   5) == 0) gtk_entry_set_text(user, line + 5);
    }
    fclose(f);
}

static void cfg_save(const char *srv, const char *room, const char *user)
{
    FILE *f = fopen(s_cfg, "w");
    if (!f) return;
    fprintf(f, "server=%s\nroom=%s\nuser=%s\n", srv, room, user);
    fclose(f);
}

/* ── Widget handles ─────────────────────────────────────────────────────── */

static GtkWidget    *window;
static GtkWidget    *stack;                    /* "connect" | "room" pages */
static GtkWidget    *server_entry, *room_entry, *user_entry, *psk_entry;
static GtkWidget    *connect_btn, *update_btn, *error_label;
static GtkWidget    *room_label, *mute_btn, *disconnect_btn, *share_btn;
static GtkTextBuffer *peer_buf;
static GtkWidget    *screen_draw;              /* GtkDrawingArea for video */
static GtkWidget    *screen_label;             /* "Viewing shared screen" text */
static cairo_surface_t *screen_surface = NULL; /* ARGB32 surface from decoded frames */
static int           screen_surf_w = 0, screen_surf_h = 0;
static uint32_t      screen_last_fid = 0;
static uint8_t      *screen_frame_buf = NULL;
static int           screen_frame_buf_sz = 0;

/* ── Update button helpers ──────────────────────────────────────────────── */

static int s_update_rc = 0;
static gboolean update_idle(gpointer data)
{
    (void)data;
    gtk_widget_set_sensitive(update_btn, TRUE);
    if (s_update_rc == 0) {
        gtk_button_set_label(GTK_BUTTON(update_btn), "✓ Updated — restart to apply");
    } else {
        const char *reason = vc_last_error();
        if (reason && reason[0]) {
            char msg[300];
            snprintf(msg, sizeof(msg), "Update failed: %s", reason);
            gtk_button_set_label(GTK_BUTTON(update_btn), msg);
        } else {
            gtk_button_set_label(GTK_BUTTON(update_btn), "Update failed");
        }
    }
    return G_SOURCE_REMOVE;
}

static void *update_bg(void *p)
{
    (void)p;
    s_update_rc = do_update();
    g_idle_add(update_idle, NULL);
    return NULL;
}

static void on_update_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_widget_set_sensitive(update_btn, FALSE);
    gtk_button_set_label(GTK_BUTTON(update_btn), "Updating…");
    pthread_t t; pthread_create(&t, NULL, update_bg, NULL); pthread_detach(t);
}

static void on_debug_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    vc_set_debug(gtk_toggle_button_get_active(b) ? 1 : 0);
}

/* ── Screen preview rendering ──────────────────────────────────────────── */

static void update_screen_surface(void)
{
    int w = 0, h = 0;
    uint32_t fid = 0;
    if (vc_screen_sharer_id() == 0 && !vc_screen_sharing()) return;

    int need = 3840 * 2160 * 4;
    if (screen_frame_buf_sz < need) {
        free(screen_frame_buf);
        screen_frame_buf = (uint8_t *)malloc((size_t)need);
        screen_frame_buf_sz = need;
    }
    if (!screen_frame_buf) return;
    if (vc_screen_frame_get(screen_frame_buf, screen_frame_buf_sz, &w, &h, &fid) != 0)
        return;
    if (fid == screen_last_fid) return;
    screen_last_fid = fid;

    /* Create/recreate Cairo surface */
    if (w != screen_surf_w || h != screen_surf_h || !screen_surface) {
        if (screen_surface) cairo_surface_destroy(screen_surface);
        screen_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        screen_surf_w = w;
        screen_surf_h = h;
    }

    /* Copy BGRA frame data into Cairo ARGB32 surface (same byte layout) */
    cairo_surface_flush(screen_surface);
    unsigned char *dst = cairo_image_surface_get_data(screen_surface);
    int stride = cairo_image_surface_get_stride(screen_surface);
    for (int y = 0; y < h; y++)
        memcpy(dst + y * stride, screen_frame_buf + y * w * 4, (size_t)w * 4);
    cairo_surface_mark_dirty(screen_surface);
}

static gboolean on_screen_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;
    if (!screen_surface || screen_surf_w <= 0 || screen_surf_h <= 0) return FALSE;

    int alloc_w = gtk_widget_get_allocated_width(widget);
    int alloc_h = gtk_widget_get_allocated_height(widget);
    double aspect = (double)screen_surf_w / (double)screen_surf_h;
    double disp_w = alloc_w;
    double disp_h = disp_w / aspect;
    if (disp_h > alloc_h) {
        disp_h = alloc_h;
        disp_w = disp_h * aspect;
    }
    double off_x = (alloc_w - disp_w) / 2.0;
    double off_y = (alloc_h - disp_h) / 2.0;

    cairo_translate(cr, off_x, off_y);
    cairo_scale(cr, disp_w / screen_surf_w, disp_h / screen_surf_h);
    cairo_set_source_surface(cr, screen_surface, 0, 0);
    cairo_paint(cr);
    return TRUE;
}

/* ── Peer list refresh (100 ms timer) ───────────────────────────────────── */

static gboolean tick(gpointer data)
{
    (void)data;

    /* Engine stopped while in room view → return to connect */
    if (!s_running &&
        strcmp(gtk_stack_get_visible_child_name(GTK_STACK(stack)), "room") == 0) {
        if (s_running) { vc_quit(); pthread_join(s_thread, NULL); s_running = 0; }
        const char *err = vc_last_error();
        gtk_label_set_text(GTK_LABEL(error_label), (err && err[0]) ? err : "");
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "connect");
        return G_SOURCE_CONTINUE;
    }
    if (!s_running) return G_SOURCE_CONTINUE;

    int connected = vc_is_connected();
    int muted     = vc_get_muted();

    gtk_button_set_label(GTK_BUTTON(mute_btn), muted ? "Unmute [M]" : "Mute [M]");
    int sharing = vc_screen_sharing();
    gtk_button_set_label(GTK_BUTTON(share_btn),
                          sharing ? "Stop Share" : "Share Screen");

    if (!connected) {
        char buf[512];
        snprintf(buf, sizeof buf, "Connecting to %s…", s_args.host);
        gtk_label_set_text(GTK_LABEL(room_label), buf);
        gtk_text_buffer_set_text(peer_buf, "Connecting…", -1);
        return G_SOURCE_CONTINUE;
    }

    char rlbl[128];
    snprintf(rlbl, sizeof rlbl, "# %s", vc_room_name());
    gtk_label_set_text(GTK_LABEL(room_label), rlbl);

    GString *txt = g_string_new(NULL);
    const char *me = vc_username();
    g_string_append_printf(txt, muted
        ? "\xe2\x97\x8b %s  (you, muted)\n" : "\xe2\x97\x8f %s  (you)\n", me);

    vc_peer_snapshot_t peers[32];
    int n = vc_snapshot_peers(peers, 32);
    for (int i = 0; i < n; i++) {
        vc_peer_snapshot_t *p = &peers[i];
        const char *path = p->direct_ok    ? "P2P"  :
                           p->direct_known ? "~P2P" : "relay";
        g_string_append_printf(txt, p->speaking
            ? "\xe2\x97\x8f %s  [%s]  jb=%d/%dms"
            : "\xe2\x97\x8b %s  [%s]  jb=%d/%dms",
            p->name, path, p->jb_ms, p->jb_target_ms);
        if (p->sharing_screen)
            g_string_append(txt, "  [screen]");
        g_string_append_c(txt, '\n');
    }
    if (n == 0)
        g_string_append(txt, "\nWaiting for others to join…");

    gtk_text_buffer_set_text(peer_buf, txt->str, -1);
    g_string_free(txt, TRUE);

    /* Update screen share preview */
    update_screen_surface();
    int has_preview = (screen_surface != NULL && screen_surf_w > 0 &&
                       (vc_screen_sharer_id() != 0 || sharing));
    if (has_preview) {
        gtk_widget_show(screen_draw);
        gtk_widget_show(screen_label);
        if (sharing)
            gtk_label_set_text(GTK_LABEL(screen_label), "Sharing screen");
        else
            gtk_label_set_text(GTK_LABEL(screen_label), "Viewing shared screen");
        gtk_widget_queue_draw(screen_draw);
    } else {
        gtk_widget_hide(screen_draw);
        gtk_widget_hide(screen_label);
    }

    return G_SOURCE_CONTINUE;
}

/* ── Signal handlers ────────────────────────────────────────────────────── */

static void on_connect_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *srv  = gtk_entry_get_text(GTK_ENTRY(server_entry));
    const char *room = gtk_entry_get_text(GTK_ENTRY(room_entry));
    const char *user = gtk_entry_get_text(GTK_ENTRY(user_entry));
    const char *psk  = gtk_entry_get_text(GTK_ENTRY(psk_entry));

    if (!srv[0] || !user[0]) {
        gtk_label_set_text(GTK_LABEL(error_label),
                           "Server and Name are required.");
        return;
    }
    if (!room[0]) room = "general";

    cfg_save(srv, room, user);

    snprintf(s_args.host, sizeof s_args.host, "%s", srv);
    snprintf(s_args.room, sizeof s_args.room, "%s", room);
    snprintf(s_args.user, sizeof s_args.user, "%s", user);
    snprintf(s_args.psk,  sizeof s_args.psk,  "%s", psk);

    s_running = 1;
    pthread_create(&s_thread, NULL, eng_fn, &s_args);

    char rlbl[128]; snprintf(rlbl, sizeof rlbl, "# %s", room);
    gtk_label_set_text(GTK_LABEL(room_label), rlbl);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "room");
}

static void on_disconnect_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (s_running) { vc_quit(); pthread_join(s_thread, NULL); s_running = 0; }
    const char *err = vc_last_error();
    gtk_label_set_text(GTK_LABEL(error_label), (err && err[0]) ? err : "");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "connect");
}

static void on_mute_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    vc_set_muted(!vc_get_muted());
}

static void on_share_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (vc_screen_sharing()) vc_screen_share_stop();
    else                     vc_screen_share_start();
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    /* M key toggles mute while in room view */
    if (ev->keyval == GDK_KEY_m || ev->keyval == GDK_KEY_M) {
        if (s_running &&
            strcmp(gtk_stack_get_visible_child_name(GTK_STACK(stack)),
                   "room") == 0) {
            vc_set_muted(!vc_get_muted());
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    if (s_running) { vc_quit(); pthread_join(s_thread, NULL); }
    gtk_main_quit();
    return FALSE;
}

/* ── UI construction ────────────────────────────────────────────────────── */

static GtkWidget *make_row(const char *lbl_text, GtkWidget *field)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl = gtk_label_new(lbl_text);
    gtk_label_set_width_chars(GTK_LABEL(lbl), 7);
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), field, TRUE, TRUE, 0);
    return box;
}

static GtkWidget *build_connect_page(void)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);

    /* Title */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b><big>Lateralus</big></b>");
    gtk_widget_set_margin_bottom(title, 6);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

    /* Version label — faint, small */
#ifndef VC_COMMIT_COUNT
#  define VC_COMMIT_COUNT "0"
#  define VC_COMMIT_HASH  "dev"
#endif
    GtkWidget *ver = gtk_label_new("#" VC_COMMIT_COUNT "  " VC_COMMIT_HASH);
    gtk_label_set_xalign(GTK_LABEL(ver), 0.0);
    gtk_widget_set_opacity(ver, 0.3);
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "label { font-size: 9pt; }", -1, NULL);
        gtk_style_context_add_provider(gtk_widget_get_style_context(ver),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
    gtk_box_pack_start(GTK_BOX(vbox), ver, FALSE, FALSE, 0);

    /* Input fields */
    server_entry = gtk_entry_new();
    room_entry   = gtk_entry_new();
    user_entry   = gtk_entry_new();
    psk_entry    = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(psk_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(server_entry), "hostname or IP");
    gtk_entry_set_placeholder_text(GTK_ENTRY(room_entry),   "general");
    gtk_entry_set_placeholder_text(GTK_ENTRY(psk_entry),    "optional passphrase");

    cfg_load(GTK_ENTRY(server_entry), GTK_ENTRY(room_entry),
             GTK_ENTRY(user_entry));

    /* Default username */
    if (!gtk_entry_get_text(GTK_ENTRY(user_entry))[0]) {
        const char *u = getenv("USER"); if (!u) u = getenv("USERNAME");
        if (u && u[0]) gtk_entry_set_text(GTK_ENTRY(user_entry), u);
    }

    gtk_box_pack_start(GTK_BOX(vbox), make_row("Server", server_entry), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(vbox), make_row("Room",   room_entry),   FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(vbox), make_row("Name",   user_entry),   FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(vbox), make_row("PSK",    psk_entry),    FALSE,FALSE,0);

    /* Connect button */
    connect_btn = gtk_button_new_with_label("Connect");
    gtk_widget_set_margin_top(connect_btn, 6);
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), connect_btn, FALSE, FALSE, 0);

    /* Update button */
    update_btn = gtk_button_new_with_label("Check for Updates");
    g_signal_connect(update_btn, "clicked", G_CALLBACK(on_update_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), update_btn, FALSE, FALSE, 0);

    /* Debug log checkbox */
    GtkWidget *debug_btn = gtk_check_button_new_with_label("Debug logs");
    g_signal_connect(debug_btn, "toggled", G_CALLBACK(on_debug_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), debug_btn, FALSE, FALSE, 0);

    /* Error label */
    error_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(error_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), error_label, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *build_room_page(void)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    room_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(room_label), 0.0);
    gtk_label_set_markup(GTK_LABEL(room_label), "<b># …</b>");
    gtk_box_pack_start(GTK_BOX(vbox), room_label, FALSE, FALSE, 0);

    /* Scrollable monospaced peer list */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_IN);

    GtkWidget *tv = gtk_text_view_new();
    peer_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 4);

    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "textview { font-family: Monospace; font-size: 11pt; }", -1, NULL);
        gtk_style_context_add_provider(gtk_widget_get_style_context(tv),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    /* Screen share preview */
    screen_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(screen_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), screen_label, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(screen_label, TRUE);

    screen_draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(screen_draw, -1, 240);
    g_signal_connect(screen_draw, "draw", G_CALLBACK(on_screen_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), screen_draw, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(screen_draw, TRUE);

    /* Mute / Share Screen / Disconnect buttons */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    mute_btn       = gtk_button_new_with_label("Mute [M]");
    share_btn      = gtk_button_new_with_label("Share Screen");
    disconnect_btn = gtk_button_new_with_label("Disconnect");
    g_signal_connect(mute_btn,       "clicked", G_CALLBACK(on_mute_clicked),       NULL);
    g_signal_connect(share_btn,      "clicked", G_CALLBACK(on_share_clicked),      NULL);
    g_signal_connect(disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), mute_btn,       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), share_btn,      TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), disconnect_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    return vbox;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Handle CLI flags before GUI */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0)
            return do_update();
        if (strcmp(argv[i], "--token") == 0 && i + 1 < argc)
            return save_token(argv[i + 1]);
    }

    cfg_init();
    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Lateralus");
    gtk_window_set_default_size(GTK_WINDOW(window), 340, 420);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);

    stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(stack), build_connect_page(), "connect");
    gtk_stack_add_named(GTK_STACK(stack), build_room_page(),    "room");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "connect");

    gtk_container_add(GTK_CONTAINER(window), stack);
    gtk_widget_show_all(window);

    /* 100 ms peer-list refresh */
    g_timeout_add(100, tick, NULL);

    gtk_main();
    return 0;
}
