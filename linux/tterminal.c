// TTerminal for Linux: a GTK window hosting the shared web UI, with bash on a pty
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <webkit2/webkit2.h>

#define ORIGIN "tterminal://app/"
#define READ_CHUNK (64 * 1024)
#define SEND_MAX (256 * 1024)    // largest output message to the page
#define MAX_PENDING (1024 * 1024) // stop reading the pty past this much unsent output

static struct {
    GtkWindow *win;
    WebKitWebView *web;
    int fd;
    pid_t pid;
    guint read_watch, write_watch;
    GByteArray *pending; // pty output waiting for the page
    GByteArray *outq;    // keyboard input waiting for the pty
    gboolean page_ready, in_flight, maximized, eof;
    int cols, rows;
    char *cwd, *size_file;
    guint btn; // last mouse press, used to start native move/resize
    double bx, by;
    guint32 btime;
} T = { .fd = -1, .cols = 115, .rows = 33 };

static void flush(void);

// Shell: TTERMINAL_SHELL, bash on PATH, $SHELL, then /bin/sh
static char *find_shell(void)
{
    const char *env = g_getenv("TTERMINAL_SHELL");
    if (env && *env && g_file_test(env, G_FILE_TEST_IS_EXECUTABLE)) return g_strdup(env);
    char *bash = g_find_program_in_path("bash");
    if (bash) return bash;
    env = g_getenv("SHELL");
    if (env && *env && g_file_test(env, G_FILE_TEST_IS_EXECUTABLE)) return g_strdup(env);
    return g_strdup("/bin/sh");
}

static void load_size(void)
{
    char *s = NULL;
    int c, r;
    if (g_file_get_contents(T.size_file, &s, NULL, NULL) && sscanf(s, "%d,%d", &c, &r) == 2 && c > 0 && r > 0)
        T.cols = c, T.rows = r;
    g_free(s);
}

static void save_size(void)
{
    char *dir = g_path_get_dirname(T.size_file);
    char buf[32];
    g_mkdir_with_parents(dir, 0700);
    snprintf(buf, sizeof buf, "%d,%d", T.cols, T.rows);
    g_file_set_contents(T.size_file, buf, -1, NULL);
    g_free(dir);
}

// Forks the shell before GTK starts, so the child inherits a clean process
static void spawn_shell(const char *shell)
{
    struct winsize ws = { .ws_row = T.rows, .ws_col = T.cols };
    T.pid = forkpty(&T.fd, NULL, NULL, &ws);
    if (T.pid < 0) {
        perror("tterminal: forkpty");
        exit(1);
    }
    if (T.pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        if (chdir(T.cwd) != 0 && chdir("/") != 0) _exit(127);
        setenv("PWD", T.cwd, 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "TTerminal", 1);
        const char *name = strrchr(shell, '/');
        execl(shell, name ? name + 1 : shell, (char *)NULL);
        perror("tterminal: exec");
        _exit(127);
    }
    fcntl(T.fd, F_SETFL, fcntl(T.fd, F_GETFL) | O_NONBLOCK);
    fcntl(T.fd, F_SETFD, FD_CLOEXEC);
}

// ---- page bridge ----

static void on_called(GObject *src, GAsyncResult *res, gpointer is_output)
{
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(src), res, NULL);
    if (v) g_object_unref(v);
    if (is_output) {
        T.in_flight = FALSE;
        flush();
    }
}

// Sends one message to the page; output messages resolve once xterm has parsed them
static void post_take(char *msg, gboolean is_output)
{
    if (!T.page_ready || !T.web) {
        g_free(msg);
        return;
    }
    GVariantDict args;
    g_variant_dict_init(&args, NULL);
    g_variant_dict_insert_value(&args, "d", g_variant_new_take_string(msg));
    // WebKit takes its own ref on the arguments, so sink and drop ours (a floating ref would leak every message)
    GVariant *v = g_variant_ref_sink(g_variant_dict_end(&args));
    webkit_web_view_call_async_javascript_function(T.web, "return window.__tt(d);", -1, v,
                                                   NULL, NULL, NULL, on_called, GINT_TO_POINTER(is_output));
    g_variant_unref(v);
}

static void post(char tag, const char *arg)
{
    post_take(g_strdup_printf("%c%s", tag, arg ? arg : ""), FALSE);
}

// ---- pty I/O ----

static gboolean on_pty_read(int fd, GIOCondition cond, gpointer data);

static void update_read_watch(void)
{
    gboolean want = !T.eof && T.pending->len < MAX_PENDING;
    if (want && !T.read_watch)
        T.read_watch = g_unix_fd_add(T.fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pty_read, NULL);
    else if (!want && T.read_watch) {
        g_source_remove(T.read_watch);
        T.read_watch = 0;
    }
}

// Length of the longest prefix that does not end inside a UTF-8 sequence
static guint utf8_cut(const guint8 *p, guint len)
{
    for (guint i = len, back = 0; i > 0 && back < 4; i--, back++) {
        guint8 b = p[i - 1];
        if ((b & 0xC0) == 0x80) continue;
        guint need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        return (i - 1 + need > len) ? i - 1 : len;
    }
    return len;
}

// Sends buffered output while nothing is in flight; data arriving meanwhile is batched
static void flush(void)
{
    if (T.page_ready && !T.in_flight && T.pending->len > 0) {
        guint cut = utf8_cut(T.pending->data, MIN(T.pending->len, SEND_MAX));
        if (cut > 0) {
            GString *s = g_string_sized_new(cut + 1);
            g_string_append_c(s, 'o');
            const char *p = (const char *)T.pending->data, *end = p + cut;
            while (p < end) { // drop NULs, which would truncate the message
                const char *z = memchr(p, 0, end - p);
                if (!z) z = end;
                g_string_append_len(s, p, z - p);
                p = z + 1;
            }
            g_byte_array_remove_range(T.pending, 0, cut);
            char *text = g_utf8_make_valid(s->str, s->len);
            g_string_free(s, TRUE);
            T.in_flight = TRUE;
            post_take(text, TRUE);
        }
    }
    update_read_watch();
}

static gboolean on_pty_read(int fd, GIOCondition cond, gpointer data)
{
    static guint8 buf[READ_CHUNK];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
        g_byte_array_append(T.pending, buf, n);
        flush();
        return G_SOURCE_CONTINUE;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return G_SOURCE_CONTINUE;
    T.eof = TRUE; // shell is gone; the child watch closes the window
    T.read_watch = 0;
    return G_SOURCE_REMOVE;
}

static gboolean on_pty_writable(int fd, GIOCondition cond, gpointer data)
{
    ssize_t n = write(fd, T.outq->data, T.outq->len);
    if (n > 0) g_byte_array_remove_range(T.outq, 0, n);
    else if (n < 0 && errno != EAGAIN && errno != EINTR) g_byte_array_set_size(T.outq, 0);
    if (T.outq->len > 0) return G_SOURCE_CONTINUE;
    T.write_watch = 0;
    return G_SOURCE_REMOVE;
}

static void pty_write(const void *data, gsize len)
{
    if (T.fd < 0 || len == 0) return;
    if (T.outq->len == 0) {
        ssize_t n = write(T.fd, data, len);
        if (n < 0 && errno != EAGAIN && errno != EINTR) return;
        if (n > 0) data = (const guint8 *)data + n, len -= n;
        if (len == 0) return;
    }
    g_byte_array_append(T.outq, data, len); // large paste: finish when the pty drains
    if (!T.write_watch) T.write_watch = g_unix_fd_add(T.fd, G_IO_OUT, on_pty_writable, NULL);
}

// xterm sends raw bytes as code points 0-255
static void pty_write_binary(const char *s)
{
    GByteArray *b = g_byte_array_new();
    for (; *s; s = g_utf8_next_char(s)) {
        guint8 c = (guint8)g_utf8_get_char(s);
        g_byte_array_append(b, &c, 1);
    }
    pty_write(b->data, b->len);
    g_byte_array_unref(b);
}

static void resize_pty(const char *s)
{
    int c, r;
    if (sscanf(s, "%d,%d", &c, &r) != 2 || c < 1 || r < 1 || (c == T.cols && r == T.rows)) return;
    T.cols = c, T.rows = r;
    struct winsize ws = { .ws_row = r, .ws_col = c };
    ioctl(T.fd, TIOCSWINSZ, &ws);
}

// ---- window actions ----

static void on_clip_text(GtkClipboard *cb, const char *text, gpointer data)
{
    post('p', text);
}

static void set_clip(GdkAtom which, const char *text)
{
    if (*text) gtk_clipboard_set_text(gtk_clipboard_get(which), text, -1);
}

static void open_link(const char *uri)
{
    if (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))
        gtk_show_uri_on_window(T.win, uri, GDK_CURRENT_TIME, NULL);
}

// Opens another window in the shell's current folder
static void new_window(void)
{
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *link = g_strdup_printf("/proc/%d/cwd", T.pid);
    char *cwd = g_file_read_link(link, NULL);
    char *argv[] = { exe, cwd ? cwd : T.cwd, NULL };
    if (exe) g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
    g_free(exe);
    g_free(link);
    g_free(cwd);
}

static void begin_resize(int edge)
{
    static const GdkWindowEdge map[] = {
        [1] = GDK_WINDOW_EDGE_WEST,       [2] = GDK_WINDOW_EDGE_EAST,       [3] = GDK_WINDOW_EDGE_NORTH,
        [4] = GDK_WINDOW_EDGE_NORTH_WEST, [5] = GDK_WINDOW_EDGE_NORTH_EAST, [6] = GDK_WINDOW_EDGE_SOUTH,
        [7] = GDK_WINDOW_EDGE_SOUTH_WEST, [8] = GDK_WINDOW_EDGE_SOUTH_EAST,
    };
    if (edge < 1 || edge > 8 || !T.btn || T.maximized) return;
    gtk_window_begin_resize_drag(T.win, map[edge], T.btn, T.bx, T.by, T.btime);
}

static void on_message(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer data)
{
    JSCValue *v = webkit_javascript_result_get_js_value(r);
    if (!jsc_value_is_string(v)) return;
    char *msg = jsc_value_to_string(v);
    const char *arg = msg + (*msg ? 1 : 0);

    switch (*msg) {
    case 'k':
        resize_pty(arg);
        T.page_ready = TRUE;
        post('f', gtk_window_is_active(T.win) ? "1" : "0");
        post('m', T.maximized ? "1" : "0");
        flush();
        break;
    case 'i': pty_write(arg, strlen(arg)); break;
    case 'b': pty_write_binary(arg); break;
    case 'r': resize_pty(arg); break;
    case 'c': set_clip(GDK_SELECTION_CLIPBOARD, arg); break;
    case 'y': set_clip(GDK_SELECTION_PRIMARY, arg); break;
    case 'p': gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), on_clip_text, NULL); break;
    case 'P': gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), on_clip_text, NULL); break;
    case 't': gtk_window_set_title(T.win, *arg ? arg : "TTerminal"); break;
    case 'u': open_link(arg); break;
    case 'n': new_window(); break;
    case 'm': gtk_window_iconify(T.win); break;
    case 'x': T.maximized ? gtk_window_unmaximize(T.win) : gtk_window_maximize(T.win); break;
    case 'q': gtk_widget_destroy(GTK_WIDGET(T.win)); break;
    case 's': begin_resize(atoi(arg)); break;
    case 'd':
        if (T.btn) gtk_window_begin_move_drag(T.win, T.btn, T.bx, T.by, T.btime);
        break;
    }
    g_free(msg);
}

// ---- web view ----

static const char *mime_of(const char *path)
{
    if (g_str_has_suffix(path, ".html")) return "text/html";
    if (g_str_has_suffix(path, ".css")) return "text/css";
    if (g_str_has_suffix(path, ".js")) return "text/javascript";
    return "application/octet-stream";
}

// Serves the web assets compiled into the binary
static void serve(WebKitURISchemeRequest *req, gpointer data)
{
    const char *path = webkit_uri_scheme_request_get_path(req);
    char *res = g_strconcat("/tterminal/web", path, NULL);
    gsize size = 0;
    GInputStream *in = NULL;
    if (g_resources_get_info(res, 0, &size, NULL, NULL)) in = g_resources_open_stream(res, 0, NULL);
    if (in) {
        webkit_uri_scheme_request_finish(req, in, size, mime_of(path));
        g_object_unref(in);
    } else {
        GError *err = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Not found: %s", path);
        webkit_uri_scheme_request_finish_error(req, err);
        g_error_free(err);
    }
    g_free(res);
}

// Keeps the view on the app; links go through the 'u' message
static gboolean on_policy(WebKitWebView *w, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, gpointer data)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
    WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && g_str_has_prefix(uri, ORIGIN)) return FALSE;
    webkit_policy_decision_ignore(d);
    return TRUE;
}

// Stays hidden until the page has loaded
static void on_load(WebKitWebView *w, WebKitLoadEvent ev, gpointer data)
{
    if (ev != WEBKIT_LOAD_FINISHED || gtk_widget_get_visible(GTK_WIDGET(T.win))) return;
    gtk_widget_show_all(GTK_WIDGET(T.win));
    gtk_window_present(T.win);
}

// The shell keeps running if the page crashes; reload and resume streaming
static void on_web_crash(WebKitWebView *w, WebKitWebProcessTerminationReason reason, gpointer data)
{
    T.page_ready = T.in_flight = FALSE;
    webkit_web_view_load_uri(T.web, ORIGIN "index.html");
}

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    if (e->type == GDK_BUTTON_PRESS) T.btn = e->button, T.bx = e->x_root, T.by = e->y_root, T.btime = e->time;
    return FALSE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    T.btn = 0;
    return FALSE;
}

static WebKitWebView *create_web_view(gboolean opaque)
{
    char *data_dir = g_build_filename(g_get_user_data_dir(), "tterminal", NULL);
    char *cache_dir = g_build_filename(g_get_user_cache_dir(), "tterminal", NULL);
    WebKitWebsiteDataManager *dm = webkit_website_data_manager_new(
        "base-data-directory", data_dir, "base-cache-directory", cache_dir, NULL);
    WebKitWebContext *ctx = webkit_web_context_new_with_website_data_manager(dm);
    webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_register_uri_scheme(ctx, "tterminal", serve, NULL, NULL);
    webkit_security_manager_register_uri_scheme_as_secure(webkit_web_context_get_security_manager(ctx), "tterminal");

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    g_signal_connect(ucm, "script-message-received::tt", G_CALLBACK(on_message), NULL);
    webkit_user_content_manager_register_script_message_handler(ucm, "tt");
    WebKitUserScript *script = webkit_user_script_new(
        opaque ? "document.documentElement.classList.add('gtk','opaque')"
               : "document.documentElement.classList.add('gtk')",
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    WebKitWebView *web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", ctx, "user-content-manager", ucm, NULL));
    g_object_unref(ctx);
    g_object_unref(dm);
    g_object_unref(ucm);
    g_free(data_dir);
    g_free(cache_dir);

    // TTERMINAL_NO_GPU: plain CPU painting, faster on machines with only software OpenGL (VMs)
    gboolean gpu = g_getenv("TTERMINAL_NO_GPU") == NULL;
    g_object_set(webkit_web_view_get_settings(web),
        "enable-developer-extras", g_getenv("TTERMINAL_DEBUG") != NULL,
        "enable-page-cache", FALSE,
        "enable-back-forward-navigation-gestures", FALSE,
        "enable-webgl", gpu,
        "hardware-acceleration-policy", gpu ? WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS : WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER,
        NULL);
    webkit_web_view_set_background_color(web, &(GdkRGBA){ 0, 0, 0, 0 });

    g_signal_connect(web, "context-menu", G_CALLBACK(gtk_true), NULL);
    g_signal_connect(web, "decide-policy", G_CALLBACK(on_policy), NULL);
    g_signal_connect(web, "load-changed", G_CALLBACK(on_load), NULL);
    g_signal_connect(web, "web-process-terminated", G_CALLBACK(on_web_crash), NULL);
    g_signal_connect(web, "button-press-event", G_CALLBACK(on_press), NULL);
    g_signal_connect(web, "button-release-event", G_CALLBACK(on_release), NULL);
    return web;
}

// ---- window ----

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    return FALSE;
}

static void on_active(GObject *o, GParamSpec *p, gpointer data)
{
    gboolean active = gtk_window_is_active(T.win);
    post('f', active ? "1" : "0");
    if (active && T.web) gtk_widget_grab_focus(GTK_WIDGET(T.web));
}

static gboolean on_state(GtkWidget *w, GdkEventWindowState *e, gpointer data)
{
    gboolean max = (e->new_window_state & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) != 0;
    if (max != T.maximized) {
        T.maximized = max;
        post('m', max ? "1" : "0");
    }
    return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    T.web = NULL;
    T.page_ready = FALSE;
    if (!T.maximized) save_size();
    if (T.pid > 0) kill(-T.pid, SIGHUP);
    gtk_main_quit();
}

static void on_child_exit(GPid pid, int status, gpointer data)
{
    g_spawn_close_pid(pid);
    T.pid = 0;
    if (T.win) gtk_widget_destroy(GTK_WIDGET(T.win));
}

int main(int argc, char **argv)
{
    // Big output buffers come from mmap and go back to the OS when freed
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        puts("Usage: tterminal [folder]\nOpens bash in folder, or in your home directory.");
        return 0;
    }

    T.cwd = argc > 1 && g_file_test(argv[1], G_FILE_TEST_IS_DIR) ? realpath(argv[1], NULL) : NULL;
    if (!T.cwd) T.cwd = g_strdup(g_get_home_dir());
    T.size_file = g_build_filename(g_get_user_config_dir(), "tterminal", "size", NULL);
    load_size();

    // bash starts first and loads in parallel with the window
    char *shell = find_shell();
    spawn_shell(shell);
    g_free(shell);

    gtk_init(&argc, &argv);
    g_set_prgname("tterminal");
    g_set_application_name("TTerminal");
    T.pending = g_byte_array_sized_new(READ_CHUNK);
    T.outq = g_byte_array_new();
    g_child_watch_add(T.pid, on_child_exit, NULL);
    update_read_watch();

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    T.win = GTK_WINDOW(win);
    gtk_window_set_title(T.win, "TTerminal");
    gtk_window_set_default_size(T.win, 1000, 620);
    gtk_window_set_position(T.win, GTK_WIN_POS_CENTER);
    gtk_window_set_decorated(T.win, FALSE);
    gtk_window_set_geometry_hints(T.win, NULL, &(GdkGeometry){ .min_width = 420, .min_height = 220 }, GDK_HINT_MIN_SIZE);
    GdkPixbuf *icon = gdk_pixbuf_new_from_resource("/tterminal/tterminal.png", NULL);
    if (icon) {
        gtk_window_set_default_icon(icon);
        g_object_unref(icon);
    }

    // Transparent window for the rounded, tinted frame (needs a compositor)
    GdkScreen *screen = gtk_widget_get_screen(win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    gboolean composited = rgba && gdk_screen_is_composited(screen);
    if (composited) {
        gtk_widget_set_visual(win, rgba);
        gtk_widget_set_app_paintable(win, TRUE);
        g_signal_connect(win, "draw", G_CALLBACK(on_draw), NULL);
    }

    T.web = create_web_view(!composited);
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(T.web));
    g_signal_connect(win, "notify::is-active", G_CALLBACK(on_active), NULL);
    g_signal_connect(win, "window-state-event", G_CALLBACK(on_state), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);
    gtk_widget_realize(win);
    webkit_web_view_load_uri(T.web, ORIGIN "index.html");

    gtk_main();
    T.win = NULL;
    return 0;
}
