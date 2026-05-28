/* src/main.c -- axctl CLI entry point
 *
 * Provides three modes:
 *   1. daemon   -- Start the IPC daemon (JSON-RPC server over Unix socket)
 *   2. subscribe -- Stream events from a running daemon
 *   3. RPC      -- Send a single JSON-RPC request to the daemon
 *
 * Usage: axctl [-c <config>] <command> <action> [args...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <json-c/json.h>

#include "ipc/compositor.h"
#include "server/server.h"
#include "config/config.h"
#include "ipc/errors.h"
#include "utils/log.h"
#include "utils/strutil.h"

#define VERSION "0.1.0-c"

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void usage(void);
static void run_daemon(const char *custom_config_path);
static void run_subscribe(void);
static void handle_rpc(const char *category, int argc, char **argv);
static char *capitalize_action(const char *s);
static char *get_socket_path(void);

/* Signal handling for clean shutdown */
static volatile sig_atomic_t g_running = 1;
static axctl_server_t *g_server = NULL;
static axctl_config_watcher_t *g_watcher = NULL;

static void *server_thread_fn(void *arg)
{
    axctl_server_start((axctl_server_t *)arg);
    return NULL;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    /* Check for version flag */
    if (strcmp(argv[1], "--version") == 0 ||
        strcmp(argv[1], "-v") == 0 ||
        strcmp(argv[1], "version") == 0) {
        printf("axctl %s\n", VERSION);
        return 0;
    }

    /* Parse -c flag before command (daemon only) */
    const char *custom_config = NULL;
    int cmd_start = 1;

    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        custom_config = argv[2];
        cmd_start = 3;
    }

    if (cmd_start >= argc) {
        usage();
        return 1;
    }

    const char *command = argv[cmd_start];

    if (strcmp(command, "daemon") == 0) {
        run_daemon(custom_config);
    } else if (strcmp(command, "subscribe") == 0) {
        run_subscribe();
    } else if (strcmp(command, "window") == 0 ||
               strcmp(command, "workspace") == 0 ||
               strcmp(command, "monitor") == 0 ||
               strcmp(command, "layout") == 0 ||
               strcmp(command, "config") == 0 ||
               strcmp(command, "system") == 0) {
        if (cmd_start + 1 >= argc) {
            usage();
            return 1;
        }
        handle_rpc(command, argc - cmd_start - 1, argv + cmd_start + 1);
    } else {
        usage();
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("Usage: axctl [-c <path>] <command> <action> [args]\n\n");
    printf("Options:\n");
    printf("  -c <path>                 Use custom config file path (daemon only)\n\n");
    printf("Commands:\n");
    printf("  daemon                    Start the IPC daemon\n");
    printf("  subscribe                 Stream events from the daemon\n\n");
    printf("  window <action> [args]\n");
    printf("    list                    List all windows\n");
    printf("    active                  Get active window ID\n");
    printf("    focus <id>              Focus a window\n");
    printf("    focus-dir <l|r|u|d>     Focus in direction\n");
    printf("    close [id]              Close a window\n");
    printf("    move <dir> [id]         Move window\n");
    printf("    resize <w> <h> [id]     Resize window\n");
    printf("    toggle-floating [id]    Toggle floating\n");
    printf("    fullscreen <0|1> [id]   Set fullscreen\n");
    printf("    maximize <0|1> [id]     Set maximized\n");
    printf("    pin <0|1> [id]          Pin window\n");
    printf("    toggle-group [id]       Toggle window group\n");
    printf("    group-nav <f|b>         Navigate group tabs\n");
    printf("    layout-prop <k> <v> [id] Set layout property\n");
    printf("    move-pixel <x> <y> [id] Move window by pixel\n");
    printf("    move-to-workspace-silent <ws> [id] Move window silently\n\n");
    printf("  workspace <action> [args]\n");
    printf("    list                    List all workspaces\n");
    printf("    active                  Get active workspace\n");
    printf("    switch <id>             Switch workspace\n");
    printf("    move-to <ws_id> [win_id] Move window to workspace\n");
    printf("    toggle-special [name]   Toggle special workspace\n\n");
    printf("  monitor <action> [args]\n");
    printf("    list                    List all monitors\n");
    printf("    focus <id>              Focus monitor\n");
    printf("    move-to <mon_id> [win_id] Move window to monitor\n");
    printf("    set-dpms <mon_id> <0|1> Set DPMS on/off\n\n");
    printf("  layout <action> [args]\n");
    printf("    set <name>              Set layout\n\n");
    printf("  config <action> [args]\n");
    printf("    get <key>               Get config value\n");
    printf("    set <key> <value>       Set config key\n");
    printf("    batch <json>            Batch apply configs\n");
    printf("    apply <json>            Apply declarative universal config\n");
    printf("    reload                  Reload config\n");
    printf("    bind-key <mods> <key> <cmd> Bind a key\n");
    printf("    unbind-key <mods> <key> Unbind a key\n\n");
    printf("  system <action> [args]\n");
    printf("    execute <cmd>           Execute command\n");
    printf("    get-cursor-position     Get cursor position\n");
    printf("    get-capabilities        Get compositor capabilities\n");
    printf("    switch-keyboard-layout  Switch keyboard layout\n");
    printf("    idle-inhibit <0|1>      Inhibit idle\n");
    printf("    idle-wait <ms>          Wait for idle\n");
    printf("    is-idle <ms>            Check if idle\n");
    printf("    exit                    Exit compositor\n");
}

/* ------------------------------------------------------------------ */
/* Socket path                                                         */
/* ------------------------------------------------------------------ */

static char *get_socket_path(void)
{
    return axctl_sprintf("/tmp/axctl-%d.sock", getuid());
}

/* ------------------------------------------------------------------ */
/* Daemon mode                                                         */
/* ------------------------------------------------------------------ */

/* Config reload callback */
static axctl_compositor_t *g_compositor = NULL;

static void config_reload_cb(axctl_toml_config_t *cfg, void *userdata)
{
    (void)userdata;
    log_info("Config changed, reloading...");
    if (g_compositor) {
        int rc = axctl_apply_config(cfg, g_compositor);
        if (rc != 0) {
            log_error("Error applying config: %s", axctl_get_error());
        }
    }
}

static void run_daemon(const char *custom_config_path)
{
    /* Detect compositor */
    axctl_compositor_t *comp = axctl_detect_compositor();
    if (!comp) {
        fprintf(stderr, "Error: no supported compositor detected\n");
        exit(1);
    }
    g_compositor = comp;

    printf("Detected compositor: %s\n", comp->name);
    printf("Creating server...\n");

    char *socket_path = get_socket_path();

    /* Single instance check */
    int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (test_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
        if (connect(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(test_fd);
            fprintf(stderr, "Error: axctl daemon is already running.\n");
            free(socket_path);
            exit(1);
        }
        close(test_fd);
    }
    unlink(socket_path); /* Clean up stale socket */

    g_server = axctl_server_create(comp, socket_path);

    printf("Starting axctl daemon on %s\n", socket_path);

    /* Load TOML config if it exists */
    char *config_path = custom_config_path ?
        axctl_strdup(custom_config_path) : axctl_default_config_path();

    struct stat st;
    if (stat(config_path, &st) == 0) {
        axctl_toml_config_t *cfg = axctl_load_config(config_path);
        if (cfg) {
            printf("[axctl-config] Loaded config from %s\n", config_path);
            if (axctl_apply_config(cfg, comp) != 0) {
                fprintf(stderr, "[axctl-config] Error applying config\n");
            }
            axctl_free_config(cfg);
        } else {
            fprintf(stderr, "[axctl-config] Error loading config: %s\n",
                    config_path);
        }

        /* Start config watcher */
        g_watcher = axctl_config_watcher_create();
        if (g_watcher) {
            axctl_config_watcher_start(g_watcher, config_path,
                                        config_reload_cb, NULL);
        }
    } else {
        printf("[axctl-config] No config file at %s, skipping\n", config_path);
    }
    free(config_path);

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Start server (blocking) in a thread so we can handle signals */
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread_fn, g_server);

    /* Wait for signal */
    while (g_running) {
        sleep(1);
    }

    /* Cleanup */
    if (g_watcher) axctl_config_watcher_stop(g_watcher);
    unlink(socket_path);
    free(socket_path);
    axctl_server_destroy(g_server);
    axctl_compositor_destroy(comp);
}

/* ------------------------------------------------------------------ */
/* Subscribe mode                                                      */
/* ------------------------------------------------------------------ */

static void run_subscribe(void)
{
    char *socket_path = get_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        free(socket_path);
        exit(1);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Error connecting to daemon: %s\n", strerror(errno));
        close(fd);
        free(socket_path);
        exit(1);
    }
    free(socket_path);

    /* Send subscribe request */
    json_object *req = json_object_new_object();
    json_object_object_add(req, "id", json_object_new_int(1));
    json_object_object_add(req, "method",
        json_object_new_string("System.Subscribe"));
    json_object_object_add(req, "params", json_object_new_object());

    const char *req_str = json_object_to_json_string(req);
    write(fd, req_str, strlen(req_str));
    write(fd, "\n", 1);
    json_object_put(req);

    /* Read events using a proper line buffer.
     * JSON messages can be large (especially State.Dump with many windows),
     * so we must handle messages that span multiple read() calls.
     * Each message is terminated by '\n'. */
    size_t line_cap = 262144;  /* 256KB initial capacity */
    char *line_buf = malloc(line_cap);
    size_t line_len = 0;

    if (!line_buf) {
        fprintf(stderr, "Out of memory\n");
        close(fd);
        exit(1);
    }

    char read_buf[8192];
    while (1) {
        ssize_t n = read(fd, read_buf, sizeof(read_buf));
        if (n <= 0) {
            fprintf(stderr, "Connection closed\n");
            break;
        }

        /* Append to line buffer and process complete lines */
        for (ssize_t i = 0; i < n; i++) {
            if (read_buf[i] == '\n') {
                /* Complete line: NUL-terminate and process */
                line_buf[line_len] = '\0';

                if (line_len > 0) {
                    json_object *msg = json_tokener_parse(line_buf);
                    if (msg) {
                        json_object *jrpc = NULL;
                        if (json_object_object_get_ex(msg, "jsonrpc", &jrpc)) {
                            printf("%s\n", json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN));
                            fflush(stdout);
                        }
                        json_object_put(msg);
                    }
                }
                line_len = 0;
            } else {
                /* Append character; grow buffer if needed */
                if (line_len + 1 >= line_cap) {
                    line_cap *= 2;
                    char *new_buf = realloc(line_buf, line_cap);
                    if (!new_buf) {
                        fprintf(stderr, "Out of memory\n");
                        free(line_buf);
                        close(fd);
                        exit(1);
                    }
                    line_buf = new_buf;
                }
                line_buf[line_len++] = read_buf[i];
            }
        }
    }

    free(line_buf);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* RPC client mode                                                     */
/* ------------------------------------------------------------------ */

/* capitalize_action: "focus-dir" -> "FocusDir" */
static char *capitalize_action(const char *s)
{
    if (!s || !*s) return axctl_strdup("");

    char *result = calloc(strlen(s) + 1, 1);
    int ri = 0;
    int capitalize_next = 1;

    for (int i = 0; s[i]; i++) {
        if (s[i] == '-') {
            capitalize_next = 1;
        } else {
            if (capitalize_next && s[i] >= 'a' && s[i] <= 'z') {
                result[ri++] = s[i] - 32;
            } else {
                result[ri++] = s[i];
            }
            capitalize_next = 0;
        }
    }

    return result;
}

static void handle_rpc(const char *category, int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return;
    }

    char *cap_cat = capitalize_action(category);
    char *cap_act = capitalize_action(argv[0]);
    char *method = axctl_sprintf("%s.%s", cap_cat, cap_act);
    free(cap_cat);
    free(cap_act);

    /* Build params */
    json_object *params = json_object_new_object();

    /* Map method + args to params (same logic as Go main.go) */
    if (strcmp(method, "Window.Focus") == 0 && argc > 1)
        json_object_object_add(params, "id", json_object_new_string(argv[1]));
    else if (strcmp(method, "Window.FocusDir") == 0 && argc > 1)
        json_object_object_add(params, "direction", json_object_new_string(argv[1]));
    else if (strcmp(method, "Window.Close") == 0 && argc > 1)
        json_object_object_add(params, "id", json_object_new_string(argv[1]));
    else if (strcmp(method, "Window.Move") == 0) {
        if (argc > 1) json_object_object_add(params, "direction", json_object_new_string(argv[1]));
        if (argc > 2) json_object_object_add(params, "id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Window.Resize") == 0) {
        if (argc > 2) {
            json_object_object_add(params, "width", json_object_new_int(atoi(argv[1])));
            json_object_object_add(params, "height", json_object_new_int(atoi(argv[2])));
        }
        if (argc > 3) json_object_object_add(params, "id", json_object_new_string(argv[3]));
    }
    else if (strcmp(method, "Window.ToggleFloating") == 0 && argc > 1)
        json_object_object_add(params, "id", json_object_new_string(argv[1]));
    else if (strcmp(method, "Window.Fullscreen") == 0) {
        if (argc > 1) json_object_object_add(params, "state", json_object_new_boolean(strcmp(argv[1], "1") == 0));
        if (argc > 2) json_object_object_add(params, "id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Window.Maximize") == 0) {
        if (argc > 1) json_object_object_add(params, "state", json_object_new_boolean(strcmp(argv[1], "1") == 0));
        if (argc > 2) json_object_object_add(params, "id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Window.Pin") == 0) {
        if (argc > 1) json_object_object_add(params, "state", json_object_new_boolean(strcmp(argv[1], "1") == 0));
        if (argc > 2) json_object_object_add(params, "id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Window.ToggleGroup") == 0 && argc > 1)
        json_object_object_add(params, "id", json_object_new_string(argv[1]));
    else if (strcmp(method, "Window.GroupNav") == 0 && argc > 1)
        json_object_object_add(params, "direction", json_object_new_string(argv[1]));
    else if (strcmp(method, "Window.LayoutProp") == 0) {
        if (argc > 2) {
            json_object_object_add(params, "key", json_object_new_string(argv[1]));
            json_object_object_add(params, "value", json_object_new_string(argv[2]));
        }
        if (argc > 3) json_object_object_add(params, "id", json_object_new_string(argv[3]));
    }
    else if (strcmp(method, "Window.MovePixel") == 0) {
        if (argc > 2) {
            json_object_object_add(params, "x", json_object_new_int(atoi(argv[1])));
            json_object_object_add(params, "y", json_object_new_int(atoi(argv[2])));
        }
        if (argc > 3) json_object_object_add(params, "id", json_object_new_string(argv[3]));
    }
    else if (strcmp(method, "Window.MoveToWorkspaceSilent") == 0) {
        if (argc > 1) json_object_object_add(params, "workspace_id", json_object_new_string(argv[1]));
        if (argc > 2) json_object_object_add(params, "window_id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Workspace.Switch") == 0 && argc > 1)
        json_object_object_add(params, "id", json_object_new_string(argv[1]));
    else if (strcmp(method, "Workspace.MoveTo") == 0) {
        if (argc > 1) json_object_object_add(params, "workspace_id", json_object_new_string(argv[1]));
        if (argc > 2) json_object_object_add(params, "window_id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Workspace.ToggleSpecial") == 0 && argc > 1)
        json_object_object_add(params, "name", json_object_new_string(argv[1]));
    else if (strcmp(method, "Monitor.Focus") == 0 && argc > 1)
        json_object_object_add(params, "id", json_object_new_string(argv[1]));
    else if (strcmp(method, "Monitor.MoveTo") == 0) {
        if (argc > 1) json_object_object_add(params, "monitor_id", json_object_new_string(argv[1]));
        if (argc > 2) json_object_object_add(params, "window_id", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Monitor.SetDpms") == 0) {
        if (argc > 1) json_object_object_add(params, "monitor_id", json_object_new_string(argv[1]));
        if (argc > 2) json_object_object_add(params, "on", json_object_new_boolean(strcmp(argv[2], "1") == 0));
    }
    else if (strcmp(method, "Layout.Set") == 0 && argc > 1)
        json_object_object_add(params, "name", json_object_new_string(argv[1]));
    else if (strcmp(method, "Config.Get") == 0 && argc > 1)
        json_object_object_add(params, "key", json_object_new_string(argv[1]));
    else if (strcmp(method, "Config.Set") == 0 && argc > 2) {
        json_object_object_add(params, "key", json_object_new_string(argv[1]));
        json_object_object_add(params, "value", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Config.Apply") == 0 && argc > 1)
        json_object_object_add(params, "payload", json_object_new_string(argv[1]));
    else if (strcmp(method, "Config.Batch") == 0 && argc > 1) {
        json_object *configs = json_tokener_parse(argv[1]);
        if (configs) json_object_object_add(params, "configs", configs);
    }
    else if (strcmp(method, "Config.RawBatch") == 0 && argc > 1)
        json_object_object_add(params, "command", json_object_new_string(argv[1]));
    else if (strcmp(method, "Config.BindKey") == 0 && argc > 3) {
        json_object_object_add(params, "mods", json_object_new_string(argv[1]));
        json_object_object_add(params, "key", json_object_new_string(argv[2]));
        json_object_object_add(params, "command", json_object_new_string(argv[3]));
    }
    else if (strcmp(method, "Config.UnbindKey") == 0 && argc > 2) {
        json_object_object_add(params, "mods", json_object_new_string(argv[1]));
        json_object_object_add(params, "key", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "Config.KeybindsBatch") == 0 && argc > 1)
        json_object_object_add(params, "payload", json_object_new_string(argv[1]));
    else if (strcmp(method, "System.Execute") == 0 && argc > 1)
        json_object_object_add(params, "command", json_object_new_string(argv[1]));
    else if (strcmp(method, "System.SwitchKeyboardLayout") == 0) {
        json_object_object_add(params, "action",
            json_object_new_string(argc > 1 ? argv[1] : "next"));
    }
    else if (strcmp(method, "System.SetKeyboardLayouts") == 0) {
        if (argc > 1) json_object_object_add(params, "layouts", json_object_new_string(argv[1]));
        if (argc > 2) json_object_object_add(params, "variants", json_object_new_string(argv[2]));
    }
    else if (strcmp(method, "System.IdleInhibit") == 0 && argc > 1)
        json_object_object_add(params, "on", json_object_new_boolean(strcmp(argv[1], "1") == 0));
    else if (strcmp(method, "System.IdleWait") == 0 ||
             strcmp(method, "System.ResumeWait") == 0 ||
             strcmp(method, "System.IsIdle") == 0 ||
             strcmp(method, "System.InputIdleWait") == 0 ||
             strcmp(method, "System.InputResumeWait") == 0 ||
             strcmp(method, "System.IsInputIdle") == 0) {
        if (argc > 1) json_object_object_add(params, "timeout_ms", json_object_new_int(atoi(argv[1])));
    }
    else if (strcmp(method, "System.IdleMonitorCreate") == 0) {
        if (argc > 1) json_object_object_add(params, "timeout_ms", json_object_new_int(atoi(argv[1])));
        if (argc > 2) json_object_object_add(params, "respect_inhibitors", json_object_new_boolean(strcmp(argv[2], "1") == 0));
        if (argc > 3) json_object_object_add(params, "enabled", json_object_new_boolean(strcmp(argv[3], "1") == 0));
    }
    else if (strcmp(method, "System.IdleMonitorUpdate") == 0) {
        if (argc > 1) json_object_object_add(params, "id", json_object_new_int(atoi(argv[1])));
        if (argc > 2) json_object_object_add(params, "timeout_ms", json_object_new_int(atoi(argv[2])));
        if (argc > 3) json_object_object_add(params, "respect_inhibitors", json_object_new_boolean(strcmp(argv[3], "1") == 0));
        if (argc > 4) json_object_object_add(params, "enabled", json_object_new_boolean(strcmp(argv[4], "1") == 0));
    }
    else if (strcmp(method, "System.IdleMonitorGet") == 0 ||
             strcmp(method, "System.IdleMonitorDestroy") == 0) {
        if (argc > 1) json_object_object_add(params, "id", json_object_new_int(atoi(argv[1])));
    }
    else if (strcmp(method, "System.IdleInhibitorCreate") == 0 && argc > 1)
        json_object_object_add(params, "enabled", json_object_new_boolean(strcmp(argv[1], "1") == 0));
    else if (strcmp(method, "System.IdleInhibitorSet") == 0 && argc > 2) {
        json_object_object_add(params, "id", json_object_new_int(atoi(argv[1])));
        json_object_object_add(params, "enabled", json_object_new_boolean(strcmp(argv[2], "1") == 0));
    }
    else if (strcmp(method, "System.IdleInhibitorGet") == 0 ||
             strcmp(method, "System.IdleInhibitorDestroy") == 0) {
        if (argc > 1) json_object_object_add(params, "id", json_object_new_int(atoi(argv[1])));
    }
    else if (strcmp(method, "System.InhibitSystem") == 0 && argc > 1)
        json_object_object_add(params, "on", json_object_new_boolean(strcmp(argv[1], "1") == 0));
    else if (strcmp(method, "System.AppInhibitCheck") == 0 && argc > 1) {
        json_object *patterns = json_object_new_array();
        for (int i = 1; i < argc; i++)
            json_object_array_add(patterns, json_object_new_string(argv[i]));
        json_object_object_add(params, "patterns", patterns);
    }

    /* Connect to daemon */
    char *socket_path = get_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Error creating socket\n");
        free(socket_path);
        free(method);
        json_object_put(params);
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Error connecting to daemon: %s\n", strerror(errno));
        close(fd);
        free(socket_path);
        free(method);
        json_object_put(params);
        return;
    }
    free(socket_path);

    /* Send request */
    json_object *req = json_object_new_object();
    json_object_object_add(req, "id", json_object_new_int(1));
    json_object_object_add(req, "method", json_object_new_string(method));
    json_object_object_add(req, "params", params);

    const char *req_str = json_object_to_json_string(req);
    write(fd, req_str, strlen(req_str));
    write(fd, "\n", 1);
    json_object_put(req);
    free(method);

    /* Read response */
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "No response from daemon\n");
        return;
    }
    buf[n] = 0;

    /* Parse response */
    json_object *resp = json_tokener_parse(buf);
    if (!resp) {
        fprintf(stderr, "Invalid response from daemon\n");
        return;
    }

    json_object *j_err = NULL, *j_result = NULL;
    if (json_object_object_get_ex(resp, "error", &j_err) &&
        json_object_get_string_len(j_err) > 0) {
        fprintf(stderr, "Error: %s\n", json_object_get_string(j_err));
    } else if (json_object_object_get_ex(resp, "result", &j_result)) {
        const char *r = json_object_get_string(j_result);
        if (r && strcmp(r, "ok") == 0) {
            printf("Success\n");
        } else {
            printf("%s\n", json_object_to_json_string_ext(j_result,
                JSON_C_TO_STRING_PRETTY));
        }
    }

    json_object_put(resp);
}
