#include "gemu/monitor.h"
#include "gemu/vnc.h"
#include "gemu/util.h"

static int  gemu_monitor_add_bp(GemuMonitor *mon, GemuBpType type, uint32_t addr);
static bool gemu_monitor_del_bp(GemuMonitor *mon, int id);
static void gemu_monitor_clear_bps(GemuMonitor *mon);
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <io.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define isatty(fd)   _isatty(fd)
#  define STDIN_FILENO 0
#  define STDOUT_FILENO 1
#  define STDERR_FILENO 2
#  define strcasecmp   _stricmp
#  define strncasecmp  _strnicmp
typedef SOCKET sock_t;
#  define INVALID_SOCK   INVALID_SOCKET
#  define sock_close(s)  closesocket(s)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#else
#  include <strings.h>
#  include <termios.h>
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
typedef int sock_t;
#  define INVALID_SOCK   (-1)
#  define sock_close(s)  close(s)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#endif

#define QUEUE_SIZE       32
#define MEDIA_DEVICE_MAX 16
#define ROM_ENTRY_MAX    16
#define BP_MAX           32

typedef struct {
    uint32_t addr;
    uint32_t size;
    char     file[512];
} MonRomEntry;

typedef struct {
    int        id;
    GemuBpType type;
    uint32_t   addr;
} MonBpEntry;

typedef enum {
    MON_ENTRY_CMD,
    MON_ENTRY_TEXT,
} MonEntryType;

typedef struct {
    MonEntryType type;
    GemuMonCmd   cmd;
    uint32_t     step_count; /* only used when cmd == GEMU_MON_STEP */
    char         text[256];
} MonEntry;

typedef enum {
    MON_BACKEND_STDIO,
    MON_BACKEND_NONE,
    MON_BACKEND_TELNET,
    MON_BACKEND_GMP,
    MON_BACKEND_GMP_STDIO,
} MonBackend;

struct GemuMonitor {
    MonEntry        queue[QUEUE_SIZE];
    int             head, tail;
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            running;
    bool            thread_started;
    volatile bool   thread_done;      /* reader thread has returned */
    bool            thread_abandoned; /* Windows: reader stuck in console
                                         read; leak mon instead of joining */
    bool            paused;
    char            last_text[256];
    uint32_t        last_step_count;
    GemuMediaDevice media[MEDIA_DEVICE_MAX];
    GemuVncServer  *vnc;
    int             n_media;
    void          (*input_reset_cb)(void *ud);
    void           *input_reset_ud;
    MonRomEntry     rom_entries[ROM_ENTRY_MAX];
    int             n_rom_entries;
    MonBpEntry      bp_entries[BP_MAX];
    int             n_bps;
    int             next_bp_id;
    bool            has_exec_bp;
    bool            has_mem_bp;
    void          (*cpu_state_cb)(void *ud, char *buf, size_t buf_len);
    void           *cpu_state_ud;
    bool          (*screendump_cb)(void *ud, const char *path);
    void           *screendump_ud;
    MonBackend      backend;
    char            telnet_host[64];
    int             telnet_port;
    sock_t          listen_fd;
    sock_t          client_fd;
};

static char default_monitor_spec[256] = "stdio";
static FILE *qmp_stdio_out;
static bool  qmp_stdio_redirected;

/* ── Transport helpers ───────────────────────────────────────────────────── */

void gemu_monitor_set_default(const char *spec) {
    snprintf(default_monitor_spec, sizeof(default_monitor_spec), "%s",
             (spec && spec[0]) ? spec : "stdio");

    if (strcasecmp(default_monitor_spec, "gmp-stdio") == 0 &&
        !qmp_stdio_redirected) {
        fflush(stdout);
#ifdef _WIN32
        int qmp_fd = _dup(_fileno(stdout));
        if (qmp_fd >= 0) {
            qmp_stdio_out = _fdopen(qmp_fd, "w");
            if (qmp_stdio_out)
                setvbuf(qmp_stdio_out, NULL, _IONBF, 0);
            _dup2(_fileno(stderr), _fileno(stdout));
            setvbuf(stdout, NULL, _IONBF, 0);
        }
#else
        int qmp_fd = dup(fileno(stdout));
        if (qmp_fd >= 0) {
            qmp_stdio_out = fdopen(qmp_fd, "w");
            if (qmp_stdio_out)
                setvbuf(qmp_stdio_out, NULL, _IONBF, 0);
            freopen("/dev/stderr", "w", stdout);
            setvbuf(stdout, NULL, _IONBF, 0);
        }
#endif
        qmp_stdio_redirected = true;
    }
}

static bool sendall(sock_t fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        int r = send(fd, p, (int)n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void mon_vprintf(const GemuMonitor *mon, const char *fmt, va_list ap) {
    if (!mon || mon->backend == MON_BACKEND_NONE) return;
    if (mon->backend == MON_BACKEND_TELNET) {
        if (mon->client_fd == INVALID_SOCK) return;
        /* 64 KiB: large enough for a full IA-64 CPU state dump (128 GRs +
         * dtr/itr/dtc/itc TLB entries), which the old 2 KiB silently cut
         * off well before reaching the TLB dump. */
        char buf[65536];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (n <= 0) return;
        size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        sendall(mon->client_fd, buf, len);
        return;
    }
    vprintf(fmt, ap);
    fflush(stdout);
}

static void mon_printf(const GemuMonitor *mon, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mon_vprintf(mon, fmt, ap);
    va_end(ap);
}

static bool parse_monitor_spec(GemuMonitor *mon, const char *spec) {
    spec = (spec && spec[0]) ? spec : "stdio";
    mon->backend = MON_BACKEND_STDIO;
    mon->telnet_host[0] = '\0';
    mon->telnet_port = 0;

    if (strcasecmp(spec, "stdio") == 0)
        return true;
    if (strcasecmp(spec, "gmp-stdio") == 0) {
        mon->backend = MON_BACKEND_GMP_STDIO;
        return true;
    }
    if (strcasecmp(spec, "none") == 0) {
        mon->backend = MON_BACKEND_NONE;
        return true;
    }

    const char *prefix = "telnet:";
    size_t prefix_len = strlen(prefix);
    bool is_gmp = false;
    if (strncasecmp(spec, "gmp:", 4) == 0) {
        prefix = "gmp:";
        prefix_len = 4;
        is_gmp = true;
    }
    if (strncasecmp(spec, prefix, prefix_len) == 0) {
        char target[128];
        const char *addr = spec + prefix_len;
        const char *comma = strchr(addr, ',');
        size_t len = comma ? (size_t)(comma - addr) : strlen(addr);
        if (len == 0 || len >= sizeof(target)) {
            fprintf(stderr, "monitor: invalid telnet address '%s'\n", spec);
            mon->backend = MON_BACKEND_NONE;
            return false;
        }
        memcpy(target, addr, len);
        target[len] = '\0';

        char *colon = strrchr(target, ':');
        if (!colon || colon == target || colon[1] == '\0') {
            fprintf(stderr, "monitor: expected %sHOST:PORT%s\n",
                    prefix, is_gmp ? "" : ",server,nowait");
            mon->backend = MON_BACKEND_NONE;
            return false;
        }
        *colon++ = '\0';
        char *end = NULL;
        long port = strtol(colon, &end, 10);
        if (port <= 0 || port > 65535 || (end && *end != '\0')) {
            fprintf(stderr, "monitor: invalid telnet port '%s'\n", colon);
            mon->backend = MON_BACKEND_NONE;
            return false;
        }
        if (strlen(target) >= sizeof(mon->telnet_host)) {
            fprintf(stderr, "monitor: telnet host is too long\n");
            mon->backend = MON_BACKEND_NONE;
            return false;
        }
        snprintf(mon->telnet_host, sizeof(mon->telnet_host), "%s", target);
        mon->telnet_port = (int)port;
        mon->backend = is_gmp ? MON_BACKEND_GMP : MON_BACKEND_TELNET;
        return true;
    }

    fprintf(stderr, "monitor: unknown backend '%s' (using stdio)\n", spec);
    return false;
}

static bool monitor_open_telnet(GemuMonitor *mon) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "monitor: WSAStartup failed\n");
        return false;
    }
#endif
    mon->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mon->listen_fd == INVALID_SOCK) {
        fprintf(stderr, "monitor: socket failed\n");
        return false;
    }

    int one = 1;
    setsockopt(mon->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)mon->telnet_port);
    if (strcmp(mon->telnet_host, "*") == 0 ||
        strcmp(mon->telnet_host, "0.0.0.0") == 0) {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        /* inet_addr instead of inet_pton: the latter is Vista+ on Windows */
        a.sin_addr.s_addr = inet_addr(mon->telnet_host);
        if (a.sin_addr.s_addr == INADDR_NONE) {
            fprintf(stderr, "monitor: invalid listen host '%s'\n", mon->telnet_host);
            sock_close(mon->listen_fd);
            mon->listen_fd = INVALID_SOCK;
            return false;
        }
    }

    if (bind(mon->listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(mon->listen_fd, 1) < 0) {
        fprintf(stderr, "monitor: failed to listen on %s:%d\n",
                mon->telnet_host, mon->telnet_port);
        sock_close(mon->listen_fd);
        mon->listen_fd = INVALID_SOCK;
        return false;
    }

    fprintf(stderr, "monitor: %s listening on %s:%d\n",
            mon->backend == MON_BACKEND_GMP ? "gmp" : "telnet",
            mon->telnet_host, mon->telnet_port);
    return true;
}

/* ── Media helpers ───────────────────────────────────────────────────────── */

static char *next_token(char **p) {
    char *s = *p;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) {
        *p = s;
        return NULL;
    }
    char *tok = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) *s++ = '\0';
    *p = s;
    return tok;
}

/* Skip leading whitespace; if the argument is wrapped in matching single or
 * double quotes, strip them so callers receive a bare path. */
static char *unquote_arg(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\'' || *s == '"') {
        char q = *s++;
        char *end = s + strlen(s);
        if (end > s && *(end - 1) == q) *(end - 1) = '\0';
    }
    return s;
}

static GemuMediaDevice *find_media(GemuMonitor *mon, const char *name) {
    if (!mon || !name) return NULL;
    for (int i = 0; i < mon->n_media; i++) {
        if (strcasecmp(mon->media[i].name, name) == 0)
            return &mon->media[i];
    }
    return NULL;
}

static void info_block(const GemuMonitor *mon) {
    if (!mon || mon->n_media == 0) {
        mon_printf(mon, "No block devices.\n");
        return;
    }
    for (int i = 0; i < mon->n_media; i++) {
        const GemuMediaDevice *dev = &mon->media[i];
        const char *type      = dev->kind ? dev->kind : dev->name;
        int         removable = dev->eject ? 1 : 0;
        int         flippable = dev->flip ? 1 : 0;
        const char *file      = dev->file[0] ? dev->file : "(none)";
        mon_printf(mon, "%s: type=%s removable=%d flippable=%d file=%s\n",
                   dev->name, type, removable, flippable, file);
    }
}

static void info_roms(const GemuMonitor *mon) {
    if (!mon || mon->n_rom_entries == 0) {
        mon_printf(mon, "No ROMs loaded.\n");
        return;
    }
    for (int i = 0; i < mon->n_rom_entries; i++) {
        const MonRomEntry *e = &mon->rom_entries[i];
        mon_printf(mon, "addr=0x%04x size=0x%04x file=%s\n",
                   e->addr, e->size, e->file);
    }
}

static void info_breakpoints(const GemuMonitor *mon) {
    if (!mon || mon->n_bps == 0) {
        mon_printf(mon, "No breakpoints set.\n");
        return;
    }
    for (int i = 0; i < mon->n_bps; i++) {
        const MonBpEntry *e = &mon->bp_entries[i];
        const char *type;
        if      (e->type == GEMU_BP_EXEC)  type = "exec ";
        else if (e->type == GEMU_BP_READ)  type = "read ";
        else if (e->type == GEMU_BP_WRITE) type = "write";
        else                               type = "rw   ";
        mon_printf(mon, "  %2d  %s  0x%04x\n", e->id, type, e->addr);
    }
}

static void info_cpu(const GemuMonitor *mon) {
    if (!mon || !mon->cpu_state_cb) {
        mon_printf(mon, "CPU state is not available for this machine.\n");
        return;
    }
    /* 64 KiB: 128 GRs plus IA-64's full TLB dump (dtr/itr/dtc/itc) can
     * exceed the old 4 KiB, silently truncating the TLB entries off the
     * end of every dump. */
    char buf[65536];
    mon->cpu_state_cb(mon->cpu_state_ud, buf, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    mon_printf(mon, "%s\n", buf);
}

static bool dispatch_media(GemuMonitor *mon, const char *line,
                           GemuMonCmd *out_cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line ? line : "");

    char *p = buf;
    char *verb = next_token(&p);
    if (!verb) return false;

    if (strcasecmp(verb, "cpu") == 0) {
        char *extra = next_token(&p);
        if (extra) mon_printf(mon, "usage: cpu\n");
        else       info_cpu(mon);
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    if (strcasecmp(verb, "screendump") == 0) {
        char *arg = unquote_arg(p);
        if (!*arg) {
            mon_printf(mon, "usage: screendump <filename>[.png]\n");
        } else if (!mon->screendump_cb) {
            mon_printf(mon, "screendump: not supported by this machine\n");
        } else {
            mon->screendump_cb(mon->screendump_ud, arg);
        }
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    if (strcasecmp(verb, "reset") == 0) {
        char *sub = next_token(&p);
        if (sub && strcasecmp(sub, "keys") == 0) {
            char *extra = next_token(&p);
            if (extra) {
                mon_printf(mon, "usage: reset keys\n");
            } else if (!mon->input_reset_cb) {
                mon_printf(mon, "reset keys: input reset is not available\n");
            } else {
                mon->input_reset_cb(mon->input_reset_ud);
                mon_printf(mon, "input bindings reset to defaults\n");
            }
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
    }

    if (strcasecmp(verb, "info") == 0) {
        char *sub = next_token(&p);
        if (!sub || strcasecmp(sub, "block") == 0) {
            info_block(mon);
        } else if (strcasecmp(sub, "roms") == 0) {
            info_roms(mon);
        } else if (strcasecmp(sub, "cpu") == 0) {
            info_cpu(mon);
        } else if (strcasecmp(sub, "breakpoints") == 0 || strcasecmp(sub, "bp") == 0) {
            info_breakpoints(mon);
        } else {
            mon_printf(mon, "info: unknown subcommand '%s' (try 'info block', 'info cpu', 'info roms', 'info breakpoints')\n", sub);
        }
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    /* break / b / rwatch / watch / awatch / wwatch */
    bool is_break  = strcasecmp(verb, "break") == 0 || strcmp(verb, "b") == 0;
    bool is_rwatch = strcasecmp(verb, "rwatch") == 0;
    bool is_watch  = strcasecmp(verb, "watch")  == 0 || strcasecmp(verb, "awatch") == 0;
    bool is_wwatch = strcasecmp(verb, "wwatch") == 0;
    if (is_break || is_rwatch || is_watch || is_wwatch) {
        char *arg = next_token(&p);
        if (!arg) {
            mon_printf(mon, "usage: %s <addr>\n", verb);
        } else {
            uint32_t addr = (uint32_t)strtoul(arg, NULL, 16);
            GemuBpType type = is_break  ? GEMU_BP_EXEC
                            : is_rwatch ? GEMU_BP_READ
                            : is_wwatch ? GEMU_BP_WRITE
                            : (GemuBpType)(GEMU_BP_READ | GEMU_BP_WRITE);
            int id = gemu_monitor_add_bp(mon, type, addr);
            const char *tname = is_break  ? "Exec breakpoint"
                              : is_rwatch ? "Read watchpoint"
                              : is_wwatch ? "Write watchpoint"
                              : "Access watchpoint";
            mon_printf(mon, "%s %d set at 0x%04x\n", tname, id, addr);
        }
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    /* delete / del */
    if (strcasecmp(verb, "delete") == 0 || strcasecmp(verb, "del") == 0) {
        char *arg = next_token(&p);
        if (!arg) {
            gemu_monitor_clear_bps(mon);
            mon_printf(mon, "All breakpoints deleted.\n");
        } else {
            int id = (int)strtol(arg, NULL, 10);
            if (gemu_monitor_del_bp(mon, id))
                mon_printf(mon, "Breakpoint %d deleted.\n", id);
            else
                mon_printf(mon, "No breakpoint with id %d.\n", id);
        }
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    bool is_change = strcasecmp(verb, "change") == 0;
    bool is_eject  = strcasecmp(verb, "eject") == 0;
    bool is_flip   = strcasecmp(verb, "flip") == 0;
    if (!is_change && !is_eject && !is_flip)
        return false;

    char *dev_name = next_token(&p);
    if (!dev_name) {
        mon_printf(mon, "usage: change <device> <file> | eject <device> | flip <device>\n");
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    GemuMediaDevice *dev = find_media(mon, dev_name);
    if (!dev) {
        mon_printf(mon, "%s: no such media device '%s' (try 'media')\n",
                   verb, dev_name);
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    GemuMediaResult result = GEMU_MEDIA_ERR;
    char err[256] = "";
    if (is_flip) {
        char *extra = next_token(&p);
        if (extra) {
            mon_printf(mon, "usage: flip <device>\n");
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        if (!dev->flip) {
            mon_printf(mon, "%s: device '%s' cannot be flipped\n", verb, dev_name);
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        result = dev->flip(dev->ud, err, sizeof(err));
    } else if (is_eject) {
        char *extra = next_token(&p);
        if (extra) {
            mon_printf(mon, "usage: eject <device>\n");
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        if (!dev->eject) {
            mon_printf(mon, "%s: device '%s' cannot be ejected\n", verb, dev_name);
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        result = dev->eject(dev->ud, err, sizeof(err));
        if (result != GEMU_MEDIA_ERR)
            dev->file[0] = '\0';
    } else {
        char *arg = unquote_arg(p);
        if (!*arg) {
            mon_printf(mon, "usage: change <device> <file>\n");
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        if (strcasecmp(arg, "eject") == 0) {
            if (!dev->eject) {
                mon_printf(mon, "change: device '%s' cannot be ejected\n", dev_name);
                *out_cmd = GEMU_MON_NONE;
                return true;
            }
            result = dev->eject(dev->ud, err, sizeof(err));
            if (result != GEMU_MEDIA_ERR)
                dev->file[0] = '\0';
        } else {
            if (!dev->change) {
                mon_printf(mon, "change: device '%s' cannot be changed\n", dev_name);
                *out_cmd = GEMU_MON_NONE;
                return true;
            }
            result = dev->change(dev->ud, arg, err, sizeof(err));
            if (result != GEMU_MEDIA_ERR)
                snprintf(dev->file, sizeof(dev->file), "%s", arg);
        }
    }

    if (result == GEMU_MEDIA_ERR) {
        if (err[0])
            mon_printf(mon, "%s: %s\n", verb, err);
        else
            mon_printf(mon, "%s: failed for media device '%s'\n", verb, dev_name);
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    *out_cmd = (result == GEMU_MEDIA_OK_RESET) ? GEMU_MON_RESET : GEMU_MON_NONE;
    return true;
}

/* ── Queue helpers ────────────────────────────────────────────────────────── */

static void enqueue(GemuMonitor *mon, GemuMonCmd cmd, uint32_t step_count) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail] = (MonEntry){
            .type = MON_ENTRY_CMD,
            .cmd = cmd,
            .step_count = step_count,
            .text = {0},
        };
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
}

static void enqueue_text(GemuMonitor *mon, const char *line) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail].type = MON_ENTRY_TEXT;
        mon->queue[mon->tail].cmd = GEMU_MON_CUSTOM;
        snprintf(mon->queue[mon->tail].text,
                 sizeof(mon->queue[mon->tail].text), "%s", line);
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
}

/* ── Reader thread ────────────────────────────────────────────────────────── */

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' '  || line[len-1] == '\t'))
        line[--len] = '\0';
}

static bool monitor_handle_line(GemuMonitor *mon, char *line) {
    trim_line(line);
    if (line[0] == '\0') return true;

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        mon_printf(mon,
        "  break <addr>   -- set exec breakpoint (short: b)\n"
        "  change <device> <file> -- insert/change media\n"
        "  change vnc password -- set VNC password\n"
        "  cont / c -- resume emulation\n"
        "  delete [N] -- delete breakpoint N (or all if N omitted)\n"
        "  dipswitch -- lists DIP switches (when supported)\n"
        "  dipswitch [name] [value] -- sets DIP switch (when supported)\n"
        "  eject <device> -- eject media\n"
        "  flip <device> -- flip disk media to the next side\n"
        "  gamegenie add [code] -- add game genie code\n"
        "  gamegenie list -- show active game genie codes\n"
        "  gamegenie delete [code] -- deletes game genie code\n"
        "  info block -- list block devices\n"
        "  info breakpoints -- list active breakpoints\n"
        "  info cpu -- show machine CPU state (short: cpu)\n"
        "  info roms  -- list loaded ROM images\n"
        "  q / quit -- quits the machine immediately\n"
        "  reset -- reset the machine\n"
        "  reset keys -- reset input bindings to defaults\n"
        "  rwatch <addr>  -- set read watchpoint\n"
        "  screendump <file>[.png] -- save screenshot (PPM or PNG)\n"
        "  step / s [count] -- step through (x) instructions (defaults to 1)\n"
        "  stop / halt -- halt emulation\n"
        "  watch <addr>   -- set read+write watchpoint\n"
        "  wwatch <addr>  -- set write watchpoint\n");
    } else if (!strcmp(line, "reset") || !strcmp(line, "system_reset")) {
        enqueue(mon, GEMU_MON_RESET, 0);
    } else if (!strcmp(line, "q") || !strcmp(line, "quit")) {
        enqueue(mon, GEMU_MON_QUIT, 0);
        return false;
    } else if (!strcmp(line, "stop") || !strcmp(line, "halt")) {
        enqueue(mon, GEMU_MON_STOP, 0);
    } else if (!strcmp(line, "cont") || !strcmp(line, "c") ||
               !strcmp(line, "resume") || !strcmp(line, "continue")) {
        enqueue(mon, GEMU_MON_CONT, 0);
    } else if ((strncmp(line, "step", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) ||
               (line[0] == 's' && (line[1] == '\0' || line[1] == ' '))) {
        char *p = line + (line[1] == 't' ? 4 : 1);
        while (*p == ' ') p++;
        uint32_t count = (*p != '\0') ? (uint32_t)strtoul(p, NULL, 10) : 1;
        if (count == 0) count = 1;
        enqueue(mon, GEMU_MON_STEP, count);
    } else {
        enqueue_text(mon, line);
    }
    return true;
}

static bool read_password_stdio(char *out, size_t out_len) {
    size_t n = 0;
#ifndef _WIN32
    struct termios oldt, raw;
    bool have_term = tcgetattr(STDIN_FILENO, &oldt) == 0;
    if (have_term) {
        raw = oldt;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
#endif

    int ch;
    while ((ch = getchar()) != EOF) {
        if (ch == '\n' || ch == '\r') break;
        if (ch == 8 || ch == 127) {
            if (n > 0) {
                n--;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }
        if (n + 1 < out_len)
            out[n++] = (char)ch;
        fputc('*', stdout);
        fflush(stdout);
    }
    out[n] = '\0';
    fputc('\n', stdout);
    fflush(stdout);

#ifndef _WIN32
    if (have_term)
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
    return ch != EOF;
}

static bool read_password_telnet(GemuMonitor *mon, char *out, size_t out_len) {
    size_t n = 0;
    while (n + 1 < out_len) {
        unsigned char ch;
        int r = recv(mon->client_fd, (char *)&ch, 1, 0);
        if (r <= 0) return false;
        if (ch == 255) {
            unsigned char cmd;
            if (recv(mon->client_fd, (char *)&cmd, 1, 0) <= 0) return false;
            if (cmd >= 251 && cmd <= 254) {
                unsigned char opt;
                if (recv(mon->client_fd, (char *)&opt, 1, 0) <= 0) return false;
            }
            continue;
        }
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (ch == 8 || ch == 127) {
            if (n > 0) {
                n--;
                sendall(mon->client_fd, "\b \b", 3);
            }
            continue;
        }
        out[n++] = (char)ch;
        sendall(mon->client_fd, "*", 1);
    }
    out[n] = '\0';
    sendall(mon->client_fd, "\n", 1);
    return true;
}

static bool monitor_change_vnc_password(GemuMonitor *mon) {
    if (!mon->vnc) {
        mon_printf(mon, "vnc: not active\n");
        return true;
    }

    char password[256];
    mon_printf(mon, "Password: ");
    bool ok = (mon->backend == MON_BACKEND_TELNET)
            ? read_password_telnet(mon, password, sizeof(password))
            : read_password_stdio(password, sizeof(password));
    if (!ok) return false;
    if (strlen(password) > 8)
        mon_printf(mon, "warning: VNC passwords are limited to 8 characters; only the first 8 will be used\n");
    gemu_vnc_set_password(mon->vnc, password);
    mon_printf(mon, "vnc: password changed\n");
    return true;
}

static bool telnet_read_line(sock_t fd, char *line, size_t len) {
    size_t n = 0;
    while (n + 1 < len) {
        unsigned char ch;
        int r = recv(fd, (char *)&ch, 1, 0);
        if (r <= 0) return false;
        if (ch == 255) { /* Telnet IAC: discard command and optional argument. */
            unsigned char cmd;
            if (recv(fd, (char *)&cmd, 1, 0) <= 0) return false;
            if (cmd >= 251 && cmd <= 254) {
                unsigned char opt;
                if (recv(fd, (char *)&opt, 1, 0) <= 0) return false;
            }
            continue;
        }
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (ch == 8 || ch == 127) {
            if (n > 0) n--;
            continue;
        }
        line[n++] = (char)ch;
    }
    line[n] = '\0';
    return true;
}

static void monitor_stdio_loop(GemuMonitor *mon) {
    char line[256];
    while (mon->running) {
        mon_printf(mon, "(gemu) ");
        if (!fgets(line, sizeof(line), stdin)) break;
        trim_line(line);
        if (strcasecmp(line, "change vnc password") == 0) {
            if (!monitor_change_vnc_password(mon)) break;
            continue;
        }
        if (!monitor_handle_line(mon, line)) break;
    }
}

static void monitor_telnet_loop(GemuMonitor *mon) {
    while (mon->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mon->listen_fd, &rfds);
        struct timeval tv = {1, 0};
        int rc = select((int)mon->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;

        sock_t client = accept(mon->listen_fd, NULL, NULL);
        if (client == INVALID_SOCK) continue;
        mon->client_fd = client;
        mon_printf(mon, "GEMU monitor\n");

        char line[256];
        bool quit_requested = false;
        while (mon->running) {
            mon_printf(mon, "(gemu) ");
            if (!telnet_read_line(client, line, sizeof(line))) break;
            trim_line(line);
            if (strcasecmp(line, "change vnc password") == 0) {
                if (!monitor_change_vnc_password(mon)) break;
                continue;
            }
            if (!monitor_handle_line(mon, line)) {
                quit_requested = true;
                mon->running = false;
                break;
            }
        }

        sock_close(client);
        mon->client_fd = INVALID_SOCK;
        if (quit_requested) return;
    }
}

static const char *skip_json_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static void json_escape(char *out, size_t out_len, const char *in) {
    size_t o = 0;
    if (out_len == 0) return;
    for (size_t i = 0; in && in[i] && o + 1 < out_len; i++) {
        unsigned char ch = (unsigned char)in[i];
        if ((ch == '"' || ch == '\\') && o + 2 < out_len) {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch == '\n' && o + 2 < out_len) {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (ch == '\r' && o + 2 < out_len) {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (ch == '\t' && o + 2 < out_len) {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (ch >= 0x20) {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

static void qmp_send_error(GemuMonitor *mon, const char *klass, const char *desc) {
    char esc[512];
    json_escape(esc, sizeof(esc), desc);
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
                     "{\"error\":{\"class\":\"%s\",\"desc\":\"%s\"}}\r\n",
                     klass ? klass : "GenericError", esc);
    if (n <= 0) return;
    size_t len = (size_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1);
    if (mon->backend == MON_BACKEND_GMP_STDIO) {
        FILE *out = qmp_stdio_out ? qmp_stdio_out : stdout;
        fwrite(buf, 1, len, out);
        fflush(out);
    } else {
        sendall(mon->client_fd, buf, len);
    }
}

static void qmp_send_error_id(GemuMonitor *mon, const char *klass,
                              const char *desc, long id) {
    char esc[512];
    json_escape(esc, sizeof(esc), desc);
    char buf[768];
    int n;
    if (id >= 0) {
        n = snprintf(buf, sizeof(buf),
                     "{\"error\":{\"class\":\"%s\",\"desc\":\"%s\"},\"id\":%ld}\r\n",
                     klass ? klass : "GenericError", esc, id);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"error\":{\"class\":\"%s\",\"desc\":\"%s\"}}\r\n",
                     klass ? klass : "GenericError", esc);
    }
    if (n <= 0) return;
    size_t len = (size_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1);
    if (mon->backend == MON_BACKEND_GMP_STDIO) {
        FILE *out = qmp_stdio_out ? qmp_stdio_out : stdout;
        fwrite(buf, 1, len, out);
        fflush(out);
    } else {
        sendall(mon->client_fd, buf, len);
    }
}

static void qmp_write(GemuMonitor *mon, const char *buf, size_t len) {
    if (mon->backend == MON_BACKEND_GMP_STDIO) {
        FILE *out = qmp_stdio_out ? qmp_stdio_out : stdout;
        fwrite(buf, 1, len, out);
        fflush(out);
    } else {
        sendall(mon->client_fd, buf, len);
    }
}

static void qmp_send_return(GemuMonitor *mon, const char *result, long id) {
    char buf[768];
    int n;
    if (id >= 0) {
        n = snprintf(buf, sizeof(buf), "{\"return\":%s,\"id\":%ld}\r\n",
                     result ? result : "{}", id);
    } else {
        n = snprintf(buf, sizeof(buf), "{\"return\":%s}\r\n",
                     result ? result : "{}");
    }
    if (n <= 0) return;
    qmp_write(mon, buf,
              (size_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
}

static void qmp_send_return_string(GemuMonitor *mon, const char *result, long id) {
    char esc[2048];
    json_escape(esc, sizeof(esc), result ? result : "");
    char quoted[2300];
    snprintf(quoted, sizeof(quoted), "\"%s\"", esc);
    qmp_send_return(mon, quoted, id);
}

static void qmp_invalid_keyword(GemuMonitor *mon, const char *line) {
    const char *p = skip_json_ws(line);
    char word[64];
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && n + 1 < sizeof(word))
        n++;
    memcpy(word, p, n);
    word[n] = '\0';

    char desc[160];
    snprintf(desc, sizeof(desc), "JSON parse error, invalid keyword '%s'", word);
    qmp_send_error(mon, "GenericError", desc);
}

static bool qmp_get_string(const char *line, const char *key,
                           char *out, size_t out_len) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(line, needle);
    if (!p) return false;
    p += strlen(needle);
    p = skip_json_ws(p);
    if (*p != ':') return false;
    p = skip_json_ws(p + 1);
    if (*p != '"') return false;
    p++;

    size_t n = 0;
    bool escape = false;
    while (*p && (escape || *p != '"')) {
        if (escape) {
            if (n + 1 < out_len)
                out[n++] = *p;
            escape = false;
        } else if (*p == '\\') {
            escape = true;
        } else if (n + 1 < out_len) {
            out[n++] = *p;
        }
        p++;
    }
    if (*p != '"') return false;
    if (out_len)
        out[n] = '\0';
    return true;
}

static long qmp_get_id(const char *line) {
    const char *p = strstr(line, "\"id\"");
    if (!p) return -1;
    p += 4;
    p = skip_json_ws(p);
    if (*p != ':') return -1;
    p = skip_json_ws(p + 1);
    if (*p == '"') p++;
    char *end = NULL;
    long id = strtol(p, &end, 10);
    return (end && end != p) ? id : -1;
}

static void appendf(char *out, size_t out_len, const char *fmt, ...) {
    if (!out || out_len == 0) return;
    size_t used = strlen(out);
    if (used >= out_len - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, out_len - used, fmt, ap);
    va_end(ap);
}

static void qmp_human_monitor_command(GemuMonitor *mon, const char *cmd,
                                      long id) {
    char out[2048] = "";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", cmd ? cmd : "");
    trim_line(buf);

    char *p = buf;
    char *verb = next_token(&p);
    if (!verb) {
        qmp_send_return_string(mon, "", id);
        return;
    }

    if (strcasecmp(verb, "info") == 0) {
        char *sub = next_token(&p);
        if (!sub || strcasecmp(sub, "block") == 0) {
            if (!mon || mon->n_media == 0) {
                appendf(out, sizeof(out), "No block devices.\n");
            } else {
                for (int i = 0; i < mon->n_media; i++) {
                    const GemuMediaDevice *dev = &mon->media[i];
                    const char *type      = dev->kind ? dev->kind : dev->name;
                    int         removable = dev->eject ? 1 : 0;
                    int         flippable = dev->flip ? 1 : 0;
                    const char *file      = dev->file[0] ? dev->file : "(none)";
                    appendf(out, sizeof(out),
                            "%s: type=%s removable=%d flippable=%d file=%s\n",
                            dev->name, type, removable, flippable, file);
                }
            }
        } else if (strcasecmp(sub, "roms") == 0) {
            if (!mon || mon->n_rom_entries == 0) {
                appendf(out, sizeof(out), "No ROMs loaded.\n");
            } else {
                for (int i = 0; i < mon->n_rom_entries; i++) {
                    const MonRomEntry *e = &mon->rom_entries[i];
                    appendf(out, sizeof(out),
                            "addr=0x%04x size=0x%04x file=%s\n",
                            e->addr, e->size, e->file);
                }
            }
        } else if (strcasecmp(sub, "breakpoints") == 0 ||
                   strcasecmp(sub, "bp") == 0) {
            if (!mon || mon->n_bps == 0) {
                appendf(out, sizeof(out), "No breakpoints set.\n");
            } else {
                for (int i = 0; i < mon->n_bps; i++) {
                    const MonBpEntry *e = &mon->bp_entries[i];
                    const char *type;
                    if      (e->type == GEMU_BP_EXEC)  type = "exec ";
                    else if (e->type == GEMU_BP_READ)  type = "read ";
                    else if (e->type == GEMU_BP_WRITE) type = "write";
                    else                               type = "rw   ";
                    appendf(out, sizeof(out), "  %2d  %s  0x%04x\n",
                            e->id, type, e->addr);
                }
            }
        } else {
            appendf(out, sizeof(out),
                    "info: unknown subcommand '%s' (try 'info block', 'info roms', 'info breakpoints')\n",
                    sub);
        }
        qmp_send_return_string(mon, out, id);
        return;
    }

    GemuMonCmd ignored = GEMU_MON_NONE;
    if (dispatch_media(mon, cmd, &ignored)) {
        qmp_send_return_string(mon, "", id);
        return;
    }

    enqueue_text(mon, cmd);
    qmp_send_return_string(mon, "", id);
}

static bool qmp_json_well_formed_object(const char *line) {
    const char *p = skip_json_ws(line);
    if (*p != '{') return false;

    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (; *p; p++) {
        char ch = *p;
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            depth++;
        } else if (ch == '}' || ch == ']') {
            depth--;
            if (depth < 0) return false;
        }
    }
    return depth == 0 && !in_string && !escape;
}

static void qmp_handle_line(GemuMonitor *mon, char *line) {
    trim_line(line);
    const char *p = skip_json_ws(line);
    if (*p == '\0')
        return;
    if (*p != '{') {
        qmp_invalid_keyword(mon, line);
        return;
    }
    if (!qmp_json_well_formed_object(line)) {
        qmp_send_error(mon, "GenericError", "JSON parse error");
        return;
    }

    char execute[96];
    long id = qmp_get_id(line);
    if (!qmp_get_string(line, "execute", execute, sizeof(execute))) {
        qmp_send_error_id(mon, "GenericError", "QMP command is missing execute", id);
        return;
    }

    if (strcmp(execute, "qmp_capabilities") == 0) {
        qmp_send_return(mon, "{}", id);
    } else if (strcmp(execute, "system_reset") == 0) {
        enqueue(mon, GEMU_MON_RESET, 0);
        qmp_send_return(mon, "{}", id);
    } else if (strcmp(execute, "cont") == 0) {
        enqueue(mon, GEMU_MON_CONT, 0);
        qmp_send_return(mon, "{}", id);
    } else if (strcmp(execute, "stop") == 0) {
        enqueue(mon, GEMU_MON_STOP, 0);
        qmp_send_return(mon, "{}", id);
    } else if (strcmp(execute, "human-monitor-command") == 0) {
        char command[256];
        qmp_get_string(line, "command-line", command, sizeof(command));
        qmp_human_monitor_command(mon, command, id);
    } else {
        qmp_send_error_id(mon, "CommandNotFound",
                          "The command has not been found", id);
    }
}

static void qmp_send_greeting(GemuMonitor *mon) {
    static const char greeting[] =
        "{\"QMP\":{\"version\":{\"qemu\":{\"micro\":0,\"minor\":6,\"major\":1},"
        "\"gemu\":{\"micro\":0,\"minor\":0,\"major\":1},\"package\":\"\"},"
        "\"capabilities\":[]}}\r\n";
    if (mon->backend == MON_BACKEND_GMP_STDIO) {
        FILE *out = qmp_stdio_out ? qmp_stdio_out : stdout;
        fputs(greeting, out);
        fflush(out);
    } else {
        sendall(mon->client_fd, greeting, sizeof(greeting) - 1);
    }
}

static void monitor_gmp_stdio_loop(GemuMonitor *mon) {
    qmp_send_greeting(mon);
    char line[512];
    while (mon->running && fgets(line, sizeof(line), stdin))
        qmp_handle_line(mon, line);
}

static void monitor_gmp_loop(GemuMonitor *mon) {
    while (mon->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mon->listen_fd, &rfds);
        struct timeval tv = {1, 0};
        int rc = select((int)mon->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;

        sock_t client = accept(mon->listen_fd, NULL, NULL);
        if (client == INVALID_SOCK) continue;
        mon->client_fd = client;
        qmp_send_greeting(mon);

        char line[512];
        while (mon->running && telnet_read_line(client, line, sizeof(line))) {
            qmp_handle_line(mon, line);
        }

        sock_close(client);
        mon->client_fd = INVALID_SOCK;
    }
}

static void *monitor_thread(void *arg) {
    GemuMonitor *mon = arg;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    if (mon->backend == MON_BACKEND_GMP_STDIO)
        monitor_gmp_stdio_loop(mon);
    else if (mon->backend == MON_BACKEND_GMP)
        monitor_gmp_loop(mon);
    else if (mon->backend == MON_BACKEND_TELNET)
        monitor_telnet_loop(mon);
    else
        monitor_stdio_loop(mon);

    mon->thread_done = true;
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GemuMonitor *gemu_monitor_create(void) {
    GemuMonitor *mon = calloc(1, sizeof(*mon));
    if (!mon) return NULL;
    pthread_mutex_init(&mon->lock, NULL);
    mon->listen_fd = INVALID_SOCK;
    mon->client_fd = INVALID_SOCK;
    parse_monitor_spec(mon, default_monitor_spec);
    return mon;
}

void gemu_monitor_destroy(GemuMonitor *mon) {
    if (!mon) return;
    gemu_monitor_stop(mon);
    if (mon->thread_abandoned)
        return;   /* reader may still touch mon - leak it, we're exiting */
    pthread_mutex_destroy(&mon->lock);
    free(mon);
}

void gemu_monitor_enqueue_reset(GemuMonitor *mon) {
    if (mon) enqueue(mon, GEMU_MON_RESET, 0);
}

void gemu_monitor_enqueue_quit(GemuMonitor *mon) {
    if (mon) enqueue(mon, GEMU_MON_QUIT, 0);
}

void gemu_monitor_shutdown_or_pause(GemuMonitor *mon, bool no_shutdown) {
    if (!no_shutdown) {
        gemu_monitor_enqueue_quit(mon);
        return;
    }
    if (mon) {
        pthread_mutex_lock(&mon->lock);
        mon->paused = true;
        pthread_mutex_unlock(&mon->lock);
    }
}

bool gemu_monitor_register_media(GemuMonitor *mon,
                                 const GemuMediaDevice *dev) {
    if (!mon || !dev || !dev->name || mon->n_media >= MEDIA_DEVICE_MAX)
        return false;
    mon->media[mon->n_media++] = *dev;
    return true;
}

void gemu_monitor_set_vnc(GemuMonitor *mon, GemuVncServer *vnc) {
    if (mon) mon->vnc = vnc;
}

void gemu_monitor_set_input_reset_cb(GemuMonitor *mon,
                                     void (*cb)(void *ud),
                                     void *ud) {
    if (!mon) return;
    mon->input_reset_cb = cb;
    mon->input_reset_ud = ud;
}

void gemu_monitor_start(GemuMonitor *mon) {
    if (!mon || mon->backend == MON_BACKEND_NONE) return;
    if (mon->backend == MON_BACKEND_STDIO && !isatty(STDIN_FILENO))
        return; /* no console, stay silent */
    if ((mon->backend == MON_BACKEND_TELNET || mon->backend == MON_BACKEND_GMP) &&
        !monitor_open_telnet(mon))
        return;
    mon->running = true;
    mon->thread_done = false;
    if (pthread_create(&mon->thread, NULL, monitor_thread, mon) == 0) {
        mon->thread_started = true;
    } else {
        mon->running = false;
        if (mon->listen_fd != INVALID_SOCK) {
            sock_close(mon->listen_fd);
            mon->listen_fd = INVALID_SOCK;
        }
    }
}

void gemu_monitor_stop(GemuMonitor *mon) {
    if (!mon || !mon->thread_started) return;
    mon->running = false;
    if (mon->listen_fd != INVALID_SOCK) {
        sock_close(mon->listen_fd);
        mon->listen_fd = INVALID_SOCK;
    }
    if (mon->client_fd != INVALID_SOCK) {
        sock_close(mon->client_fd);
        mon->client_fd = INVALID_SOCK;
    }
#ifdef _WIN32
    /* pthread_cancel doesn't interrupt fgets(stdin) on Windows.
     * Inject a synthetic Enter keypress (down+up, '\r' - the console's
     * cooked line reader completes on carriage return, not linefeed) so
     * the stdio loop unblocks, sees mon->running == false, and exits. */
    if (mon->backend == MON_BACKEND_STDIO) {
        INPUT_RECORD ir[2] = {0};
        for (int i = 0; i < 2; i++) {
            ir[i].EventType = KEY_EVENT;
            ir[i].Event.KeyEvent.bKeyDown        = (i == 0);
            ir[i].Event.KeyEvent.wRepeatCount    = 1;
            ir[i].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            ir[i].Event.KeyEvent.uChar.AsciiChar = '\r';
        }
        DWORD written;
        WriteConsoleInputA(GetStdHandle(STD_INPUT_HANDLE), &ir[0], 2, &written);
    }
    /* A console read can't be forcibly cancelled on Windows.  Give the
     * reader a moment to notice, then abandon the thread rather than hang
     * the shutdown path - the process is exiting and will reclaim it. */
    for (int i = 0; i < 50 && !mon->thread_done; i++)
        gemu_sleep_ms(10);
    if (mon->thread_done) {
        pthread_join(mon->thread, NULL);
    } else {
        mon->thread_abandoned = true;
        fprintf(stderr, "monitor: console reader did not exit; abandoning it\n");
    }
#else
    pthread_cancel(mon->thread);
    pthread_join(mon->thread, NULL);
#endif
    mon->thread_started = false;
    if (mon->backend == MON_BACKEND_STDIO)
        printf("\n"); /* tidy up any dangling prompt */
}

GemuMonCmd gemu_monitor_poll(GemuMonitor *mon) {
    if (!mon) return GEMU_MON_NONE;
    pthread_mutex_lock(&mon->lock);
    GemuMonCmd cmd = GEMU_MON_NONE;
    if (mon->head != mon->tail) {
        MonEntry entry = mon->queue[mon->head];
        mon->head = (mon->head + 1) % QUEUE_SIZE;
        if (entry.type == MON_ENTRY_TEXT) {
            cmd = GEMU_MON_CUSTOM;
            snprintf(mon->last_text, sizeof(mon->last_text),
                     "%s", entry.text);
            dispatch_media(mon, mon->last_text, &cmd);
        } else {
            cmd = entry.cmd;
            mon->last_text[0] = '\0';
            if (cmd == GEMU_MON_STEP)
                mon->last_step_count = entry.step_count ? entry.step_count : 1;
        }
        /* Keep paused state in sync so callers can just check is_paused() */
        if (cmd == GEMU_MON_STOP || cmd == GEMU_MON_STEP) mon->paused = true;
        if (cmd == GEMU_MON_CONT) mon->paused = false;
    }
    pthread_mutex_unlock(&mon->lock);
    return cmd;
}

uint32_t gemu_monitor_step_count(const GemuMonitor *mon) {
    return (mon && mon->last_step_count) ? mon->last_step_count : 1;
}

const char *gemu_monitor_command_text(const GemuMonitor *mon) {
    return mon ? mon->last_text : "";
}

void gemu_monitor_unknown_command(const GemuMonitor *mon) {
    const char *text = gemu_monitor_command_text(mon);
    if (text && text[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", text);
        char *cmd = strtok(buf, " \t");
        if (cmd && strcasecmp(cmd, "dipswitch") == 0) {
            mon_printf(mon, "dipswitch: dip switches are not available on this machine\n");
            return;
        }
        mon_printf(mon, "unknown command '%s' (try 'help')\n", text);
    }
}

bool gemu_monitor_is_paused(const GemuMonitor *mon) {
    return mon && mon->paused;
}

void gemu_monitor_register_rom(GemuMonitor *mon,
                               uint32_t addr, uint32_t size,
                               const char *file) {
    if (!mon || mon->n_rom_entries >= ROM_ENTRY_MAX || !file) return;
    MonRomEntry *e = &mon->rom_entries[mon->n_rom_entries++];
    e->addr = addr;
    e->size = size;
    snprintf(e->file, sizeof(e->file), "%s", file);
}

void gemu_monitor_clear_roms(GemuMonitor *mon) {
    if (mon) mon->n_rom_entries = 0;
}

void gemu_monitor_set_cpu_state_cb(GemuMonitor *mon,
                                   void (*cb)(void *ud,
                                              char *buf,
                                              size_t buf_len),
                                   void *ud) {
    if (!mon) return;
    mon->cpu_state_cb = cb;
    mon->cpu_state_ud = ud;
}

void gemu_monitor_set_screendump_cb(GemuMonitor *mon,
                                    bool (*cb)(void *ud, const char *path),
                                    void *ud) {
    if (!mon) return;
    mon->screendump_cb = cb;
    mon->screendump_ud = ud;
}

/* ── Breakpoints ─────────────────────────────────────────────────────────── */

static int gemu_monitor_add_bp(GemuMonitor *mon, GemuBpType type, uint32_t addr) {
    if (!mon || mon->n_bps >= BP_MAX) return -1;
    MonBpEntry *e = &mon->bp_entries[mon->n_bps++];
    e->id   = ++mon->next_bp_id;
    e->type = type;
    e->addr = addr;
    if (type & GEMU_BP_EXEC)                    mon->has_exec_bp = true;
    if (type & (GEMU_BP_READ | GEMU_BP_WRITE))  mon->has_mem_bp  = true;
    return e->id;
}

static bool gemu_monitor_del_bp(GemuMonitor *mon, int id) {
    if (!mon) return false;
    for (int i = 0; i < mon->n_bps; i++) {
        if (mon->bp_entries[i].id != id) continue;
        memmove(&mon->bp_entries[i], &mon->bp_entries[i + 1],
                (size_t)(mon->n_bps - i - 1) * sizeof(MonBpEntry));
        mon->n_bps--;
        mon->has_exec_bp = false;
        mon->has_mem_bp  = false;
        for (int j = 0; j < mon->n_bps; j++) {
            if (mon->bp_entries[j].type & GEMU_BP_EXEC)
                mon->has_exec_bp = true;
            if (mon->bp_entries[j].type & (GEMU_BP_READ | GEMU_BP_WRITE))
                mon->has_mem_bp = true;
        }
        return true;
    }
    return false;
}

static void gemu_monitor_clear_bps(GemuMonitor *mon) {
    if (!mon) return;
    mon->n_bps       = 0;
    mon->has_exec_bp = false;
    mon->has_mem_bp  = false;
}

bool gemu_monitor_check_exec(GemuMonitor *mon, uint32_t addr) {
    if (!mon || !mon->has_exec_bp) return false;
    for (int i = 0; i < mon->n_bps; i++) {
        if ((mon->bp_entries[i].type & GEMU_BP_EXEC) && mon->bp_entries[i].addr == addr) {
            mon_printf(mon, "Breakpoint %d hit: exec at 0x%04x\n",
                       mon->bp_entries[i].id, addr);
            mon->paused = true;
            return true;
        }
    }
    return false;
}

bool gemu_monitor_has_exec_breakpoints(const GemuMonitor *mon) {
    return mon && mon->has_exec_bp;
}

bool gemu_monitor_has_mem_breakpoints(const GemuMonitor *mon) {
    return mon && mon->has_mem_bp;
}

bool gemu_monitor_check_read(GemuMonitor *mon, uint32_t addr) {
    if (!mon || !mon->has_mem_bp) return false;
    for (int i = 0; i < mon->n_bps; i++) {
        if ((mon->bp_entries[i].type & GEMU_BP_READ) && mon->bp_entries[i].addr == addr) {
            mon_printf(mon, "Watchpoint %d hit: read at 0x%04x\n",
                       mon->bp_entries[i].id, addr);
            mon->paused = true;
            return true;
        }
    }
    return false;
}

bool gemu_monitor_check_write(GemuMonitor *mon, uint32_t addr) {
    if (!mon || !mon->has_mem_bp) return false;
    for (int i = 0; i < mon->n_bps; i++) {
        if ((mon->bp_entries[i].type & GEMU_BP_WRITE) && mon->bp_entries[i].addr == addr) {
            mon_printf(mon, "Watchpoint %d hit: write at 0x%04x\n",
                       mon->bp_entries[i].id, addr);
            mon->paused = true;
            return true;
        }
    }
    return false;
}
