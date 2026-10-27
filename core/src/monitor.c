/*
 * GEMU monitor - transports, command queue and public plumbing.
 *
 * The reader thread (one per monitor) owns the transport: it prints the
 * prompt, reads a line, queues it, and waits for the emulator thread to run
 * it before prompting again, so command output never interleaves with the
 * next prompt.  Parsing and running commands lives in monitor_cmds.c.
 */
#include "monitor_priv.h"
#include "gemu/vnc.h"
#include "gemu/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#ifdef _WIN32
#  include <io.h>
#  include <ws2tcpip.h>
#  define isatty(fd)   _isatty(fd)
#  define STDIN_FILENO 0
#  define STDOUT_FILENO 1
#  define STDERR_FILENO 2
#  define strcasecmp   _stricmp
#  define strncasecmp  _strnicmp
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
#  define INVALID_SOCK   (-1)
#  define sock_close(s)  close(s)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#endif

/* How long the reader waits for the emulator thread to run a command before
 * giving up and prompting anyway (the machine may be wedged). */
#define CONSOLE_WAIT_MS 2000

static char  default_monitor_spec[256] = "stdio";
static char  machine_name[64];
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

void gemu_monitor_set_machine(const char *name) {
    snprintf(machine_name, sizeof(machine_name), "%s", name ? name : "");
}

const char *mon_machine_name(void) {
    return machine_name[0] ? machine_name : NULL;
}

bool mon_sendall(sock_t fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        int r = send(fd, p, (int)n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void capture_append(GemuMonitor *mon, const char *s, size_t n) {
    if (mon->capture_len + n + 1 > mon->capture_size) {
        size_t size = mon->capture_size ? mon->capture_size : 1024;
        while (mon->capture_len + n + 1 > size) size *= 2;
        char *buf = realloc(mon->capture_buf, size);
        if (!buf) return;
        mon->capture_buf = buf;
        mon->capture_size = size;
    }
    memcpy(mon->capture_buf + mon->capture_len, s, n);
    mon->capture_len += n;
    mon->capture_buf[mon->capture_len] = '\0';
}

static void mon_vprintf(const GemuMonitor *cmon, const char *fmt, va_list ap) {
    if (!cmon) return;
    GemuMonitor *mon = (GemuMonitor *)cmon;
    if (mon->capturing || mon->backend == MON_BACKEND_TELNET) {
        /* 64 KiB: large enough for a full IA-64 CPU state dump (128 GRs +
         * dtr/itr/dtc/itc TLB entries).  On the stack: the reader and
         * emulator threads can both print to a telnet client. */
        char buf[65536];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (n <= 0) return;
        size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        if (mon->capturing)
            capture_append(mon, buf, len);
        else if (mon->client_fd != INVALID_SOCK)
            mon_sendall(mon->client_fd, buf, len);
        return;
    }
    if (mon->backend == MON_BACKEND_NONE) return;
    /* GMP backends have no console; machine output still goes to stdout
     * (which gmp-stdio has redirected to stderr). */
    vprintf(fmt, ap);
    fflush(stdout);
}

void mon_printf(const GemuMonitor *mon, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mon_vprintf(mon, fmt, ap);
    va_end(ap);
}

void gemu_monitor_printf(const GemuMonitor *mon, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (mon) {
        mon_vprintf(mon, fmt, ap);
    } else {
        vprintf(fmt, ap);
        fflush(stdout);
    }
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

/* ── Queue ────────────────────────────────────────────────────────────────── */

/* Queue an entry; returns its sequence number, or 0 if the queue is full. */
static uint64_t enqueue_entry(GemuMonitor *mon, MonEntryType type,
                              GemuMonCmd cmd, const char *text, bool capture) {
    uint64_t seq = 0;
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % MON_QUEUE_SIZE;
    if (next != mon->head) {
        MonEntry *e = &mon->queue[mon->tail];
        e->type = type;
        e->cmd = cmd;
        e->capture = capture;
        e->seq = seq = ++mon->next_seq;
        e->fn = NULL;
        e->fn_ud = NULL;
        snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
    return seq;
}

/* Wait (with mon->lock held) until entry seq has been dispatched.  Callers
 * disable cancellation around this: pthread_cond_timedwait is a cancellation
 * point and gemu_monitor_stop() would otherwise leave the lock held. */
static bool wait_done_locked(GemuMonitor *mon, uint64_t seq, unsigned timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (mon->done_seq < seq && mon->running) {
        if (pthread_cond_timedwait(&mon->done_cond, &mon->lock, &ts) != 0)
            break;
    }
    return mon->done_seq >= seq;
}

MonRunResult mon_run_line(GemuMonitor *mon, const char *line, bool capture,
                          unsigned timeout_ms, char **out) {
    if (out) *out = NULL;
    uint64_t seq = enqueue_entry(mon, MON_ENTRY_TEXT, GEMU_MON_NONE, line, capture);
    if (!seq) return MON_RUN_QUEUE_FULL;

    int oldstate;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
    pthread_mutex_lock(&mon->lock);
    bool done = wait_done_locked(mon, seq, timeout_ms);
    if (done && capture) {
        if (out) {
            *out = mon->capture_buf;
            mon->capture_buf = NULL;
        }
        mon->capture_len = mon->capture_size = 0;
    }
    pthread_mutex_unlock(&mon->lock);
    pthread_setcancelstate(oldstate, NULL);
    return done ? MON_RUN_OK : MON_RUN_TIMEOUT;
}

MonRunResult mon_run_fn(GemuMonitor *mon, MonEntryFn fn, void *ud,
                        unsigned timeout_ms) {
    uint64_t seq = 0;
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % MON_QUEUE_SIZE;
    if (next != mon->head) {
        MonEntry *e = &mon->queue[mon->tail];
        memset(e, 0, sizeof(*e));
        e->type = MON_ENTRY_FN;
        e->fn = fn;
        e->fn_ud = ud;
        e->seq = seq = ++mon->next_seq;
        mon->tail = next;
    }
    if (!seq) {
        pthread_mutex_unlock(&mon->lock);
        return MON_RUN_QUEUE_FULL;
    }
    int oldstate;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
    bool done = wait_done_locked(mon, seq, timeout_ms);
    pthread_mutex_unlock(&mon->lock);
    pthread_setcancelstate(oldstate, NULL);
    return done ? MON_RUN_OK : MON_RUN_TIMEOUT;
}

/* Queue a console line and wait until the emulator thread has run it. */
static void submit_line(GemuMonitor *mon, const char *line) {
    if (mon_run_line(mon, line, false, CONSOLE_WAIT_MS, NULL) == MON_RUN_QUEUE_FULL)
        mon_printf(mon, "monitor: command queue full, dropped '%s'\n", line);
}

/* ── Console reader ───────────────────────────────────────────────────────── */

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' '  || line[len-1] == '\t'))
        line[--len] = '\0';
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
                mon_sendall(mon->client_fd, "\b \b", 3);
            }
            continue;
        }
        out[n++] = (char)ch;
        mon_sendall(mon->client_fd, "*", 1);
    }
    out[n] = '\0';
    mon_sendall(mon->client_fd, "\n", 1);
    return true;
}

static bool monitor_change_vnc_password(GemuMonitor *mon) {
    GemuVncServer *vnc = gemu_vnc_get();
    if (!vnc) {
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
    char err[160];
    if (gemu_vnc_set_password(vnc, password, err, sizeof(err)))
        mon_printf(mon, "vnc: password changed\n");
    else
        mon_printf(mon, "vnc: %s\n", err);
    return true;
}

/* Handle one console line.  Returns false when the reader should stop
 * (quit, or the transport failed while reading a password). */
static bool console_handle_line(GemuMonitor *mon, char *line) {
    trim_line(line);
    if (line[0] == '\0') return true;
    if (mon_line_is_vnc_password(line))
        return monitor_change_vnc_password(mon);
    submit_line(mon, line);
    return !mon_line_is_quit(line);
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
    char line[MON_LINE_MAX];
    while (mon->running) {
        mon_printf(mon, "(gemu) ");
        if (!fgets(line, sizeof(line), stdin)) break;
        if (!console_handle_line(mon, line)) break;
    }
}

bool mon_accept_client(GemuMonitor *mon) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(mon->listen_fd, &rfds);
    struct timeval tv = {1, 0};
    if (select((int)mon->listen_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return false;
    sock_t client = accept(mon->listen_fd, NULL, NULL);
    if (client == INVALID_SOCK)
        return false;
    mon->client_fd = client;
    return true;
}

int mon_recv_client(GemuMonitor *mon, void *buf, size_t len) {
    return (int)recv(mon->client_fd, (char *)buf, (int)len, 0);
}

void mon_close_client(GemuMonitor *mon) {
    sock_close(mon->client_fd);
    mon->client_fd = INVALID_SOCK;
}

static void monitor_telnet_loop(GemuMonitor *mon) {
    while (mon->running) {
        if (!mon_accept_client(mon)) continue;
        mon_printf(mon, "GEMU monitor - type 'help' for more information\n");

        char line[MON_LINE_MAX];
        bool quit_requested = false;
        while (mon->running) {
            mon_printf(mon, "(gemu) ");
            if (!telnet_read_line(mon->client_fd, line, sizeof(line))) break;
            trim_line(line);
            if (mon_line_is_quit(line)) {
                submit_line(mon, line);
                quit_requested = true;
                mon->running = false;
                break;
            }
            if (!console_handle_line(mon, line)) break;
        }

        mon_close_client(mon);
        if (quit_requested) return;
    }
}

static void *monitor_thread(void *arg) {
    GemuMonitor *mon = arg;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    if (mon->backend == MON_BACKEND_GMP_STDIO)
        mon_qmp_stdio_loop(mon, qmp_stdio_out ? qmp_stdio_out : stdout);
    else if (mon->backend == MON_BACKEND_GMP)
        mon_qmp_socket_loop(mon);
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
    pthread_cond_init(&mon->done_cond, NULL);
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
    pthread_cond_destroy(&mon->done_cond);
    pthread_mutex_destroy(&mon->lock);
    free(mon->capture_buf);
    free(mon->syms);
    free(mon);
}

void gemu_monitor_pause(GemuMonitor *mon) {
    if (!mon) return;
    pthread_mutex_lock(&mon->lock);
    mon->paused = true;
    pthread_mutex_unlock(&mon->lock);
}

void gemu_monitor_enqueue_reset(GemuMonitor *mon) {
    if (mon) enqueue_entry(mon, MON_ENTRY_CMD, GEMU_MON_RESET, NULL, false);
}

void gemu_monitor_enqueue_quit(GemuMonitor *mon) {
    if (mon) enqueue_entry(mon, MON_ENTRY_CMD, GEMU_MON_QUIT, NULL, false);
}

bool gemu_monitor_enqueue_command(GemuMonitor *mon, const char *line) {
    return mon && line && enqueue_entry(mon, MON_ENTRY_TEXT, GEMU_MON_NONE, line, false) != 0;
}

int gemu_monitor_media_count(const GemuMonitor *mon) {
    return mon ? mon->n_media : 0;
}

const GemuMediaDevice *gemu_monitor_media_get(const GemuMonitor *mon, int index) {
    return mon && index >= 0 && index < mon->n_media ? &mon->media[index] : NULL;
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

void gemu_monitor_set_input_reset_cb(GemuMonitor *mon,
                                     void (*cb)(void *ud),
                                     void *ud) {
    if (!mon) return;
    mon->input_reset_cb = cb;
    mon->input_reset_ud = ud;
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

void gemu_monitor_set_memory(GemuMonitor *mon, const GemuMonMemory *mem) {
    if (!mon) return;
    mon->has_mem = mem && mem->read;
    if (mon->has_mem) {
        mon->mem = *mem;
        if (mon->mem.addr_bits == 0 || mon->mem.addr_bits > 64)
            mon->mem.addr_bits = 64;
    }
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
    pthread_mutex_lock(&mon->lock);
    mon->running = false;
    pthread_cond_broadcast(&mon->done_cond);   /* release a waiting reader */
    pthread_mutex_unlock(&mon->lock);
    for (int i = 0; i < 50 && mon->replying; i++)
        gemu_sleep_ms(10);
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

/* ── Termination signals ─────────────────────────────────────────────────── *
 *
 * Like QEMU, SIGINT/SIGTERM shut the emulator down: the first one makes
 * gemu_monitor_poll() report GEMU_MON_QUIT so the machine exits through its
 * normal path (saving battery RAM etc.); a second one exits immediately in
 * case the machine isn't polling. */

static volatile sig_atomic_t quit_signal;   /* a termination signal arrived */
static bool                  quit_reported; /* ...and QUIT was handed out */

static void on_terminate(int sig) {
    if (quit_signal) _exit(128 + sig);
    quit_signal = 1;
    static const char msg[] = "gemu: terminating on signal\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

void gemu_monitor_install_signal_handlers(void) {
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);
}

GemuMonCmd gemu_monitor_poll(GemuMonitor *mon) {
    if (!mon) return GEMU_MON_NONE;
    if (quit_signal && !quit_reported) {
        quit_reported = true;   /* once; a second signal force-exits */
        return GEMU_MON_QUIT;
    }
    for (;;) {
        pthread_mutex_lock(&mon->lock);
        if (mon->head == mon->tail) {
            pthread_mutex_unlock(&mon->lock);
            return GEMU_MON_NONE;
        }
        MonEntry entry = mon->queue[mon->head];
        mon->head = (mon->head + 1) % MON_QUEUE_SIZE;
        if (entry.capture) {
            free(mon->capture_buf);
            mon->capture_buf = NULL;
            mon->capture_len = mon->capture_size = 0;
            mon->capturing = true;
        }
        pthread_mutex_unlock(&mon->lock);

        /* Run the command without the lock: handlers may take a while and
         * the reader must stay free to queue the next line. */
        GemuMonCmd cmd = entry.type == MON_ENTRY_TEXT ? mon_dispatch(mon, entry.text)
                       : entry.type == MON_ENTRY_FN   ? entry.fn(mon, entry.fn_ud)
                       : entry.cmd;

        pthread_mutex_lock(&mon->lock);
        mon->capturing = false;
        /* Keep paused state in sync so callers can just check is_paused() */
        if (cmd == GEMU_MON_STOP || cmd == GEMU_MON_STEP) mon->paused = true;
        if (cmd == GEMU_MON_CONT) mon->paused = false;
        if (entry.seq > mon->done_seq) mon->done_seq = entry.seq;
        pthread_cond_broadcast(&mon->done_cond);
        pthread_mutex_unlock(&mon->lock);

        if (cmd != GEMU_MON_NONE)
            return cmd;
    }
}

uint32_t gemu_monitor_step_count(const GemuMonitor *mon) {
    return (mon && mon->last_step_count) ? mon->last_step_count : 1;
}

bool gemu_monitor_is_paused(const GemuMonitor *mon) {
    return mon && mon->paused;
}
