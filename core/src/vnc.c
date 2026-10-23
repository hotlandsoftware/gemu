/*
 * GEMU VNC server - RFB 3.3/3.7/3.8, configured like QEMU's -vnc.
 *
 * One server per process.  The display layer publishes every frame with
 * gemu_vnc_update_argb() and drains input with gemu_vnc_pop_key_event() /
 * gemu_vnc_get_pointer(), so machines need no VNC code of their own.
 *
 * Threads: one accept thread, and two per client - a reader that handles
 * client messages (input, update requests) and a writer that waits until a
 * requested update has something new and sends only the 16x16 tiles that
 * changed since that client's last update.  Clients that support the
 * DesktopSize pseudo-encoding are told when the frame size changes; others
 * get the frame scaled to the size they connected with.
 */
#include "gemu/vnc.h"
#include "gemu/gemu_display.h"
#ifdef HAVE_CRYPTO
#  include <openssl/rand.h>
#  include <openssl/des.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define INVALID_SOCK    INVALID_SOCKET
#  define sock_close(s)   closesocket(s)
#  define SHUT_RDWR       SD_BOTH
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#else
#  include <unistd.h>
#  include <netdb.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <errno.h>
typedef int sock_t;
#  define INVALID_SOCK    (-1)
#  define sock_close(s)   close(s)
#endif

#define VNC_PORT_BASE    5900
#define VNC_MAX_CLIENTS  16
#define VNC_KEY_QUEUE    256
#define VNC_TILE         16
#define VNC_MAX_LISTEN   4
#define ENC_RAW          0
#define ENC_DESKTOP_SIZE (-223)

/* ── Configuration (-vnc) ────────────────────────────────────────────────── */

typedef enum { SHARE_ALLOW_EXCLUSIVE, SHARE_FORCE_SHARED, SHARE_IGNORE } ShareMode;

typedef struct {
    bool      enabled;
    bool      unix_socket;
    char      host[256];       /* "" = all interfaces */
    int       display, display_max;
    char      path[108];
    bool      password;
    ShareMode share;
} VncConfig;

static VncConfig       g_cfg;
static GemuVncServer  *g_server;
static bool            g_server_tried;

/* Parse "host:N[,opt=val...]" (QEMU syntax) into cfg. */
static bool parse_spec(const char *spec, VncConfig *cfg, char *err, size_t err_len) {
    memset(cfg, 0, sizeof(*cfg));
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *opts = strchr(buf, ',');
    if (opts) *opts++ = '\0';

    if (strcmp(buf, "none") == 0) return true;      /* disabled */

    if (strncmp(buf, "unix:", 5) == 0) {
#ifdef _WIN32
        snprintf(err, err_len, "unix sockets aren't supported on Windows");
        return false;
#else
        if (!buf[5] || strlen(buf + 5) >= sizeof(cfg->path)) {
            snprintf(err, err_len, "bad unix socket path");
            return false;
        }
        cfg->unix_socket = true;
        snprintf(cfg->path, sizeof(cfg->path), "%s", buf + 5);
#endif
    } else {
        char *colon = strrchr(buf, ':');
        if (!colon) {
            snprintf(err, err_len, "expected host:N, :N or unix:/path");
            return false;
        }
        *colon = '\0';
        char *end;
        long n = strtol(colon + 1, &end, 10);
        if (*end || colon[1] == '\0' || n < 0 || n > 65535 - VNC_PORT_BASE) {
            snprintf(err, err_len, "bad display number '%s'", colon + 1);
            return false;
        }
        cfg->display = cfg->display_max = (int)n;
        char *host = buf;
        size_t hl = strlen(host);
        if (hl >= 2 && host[0] == '[' && host[hl - 1] == ']') {   /* [::1] */
            host[hl - 1] = '\0';
            host++;
        }
        snprintf(cfg->host, sizeof(cfg->host), "%s", host);
    }

    for (char *o = opts; o && *o; ) {
        char *next = strchr(o, ',');
        if (next) *next++ = '\0';
        char *val = strchr(o, '=');
        if (val) *val++ = '\0';
        if (strcmp(o, "password") == 0) {
            cfg->password = !val || strcmp(val, "on") == 0 || strcmp(val, "true") == 0;
        } else if (strcmp(o, "to") == 0 && val) {
            cfg->display_max = atoi(val);
            if (cfg->display_max < cfg->display) cfg->display_max = cfg->display;
        } else if (strcmp(o, "share") == 0 && val) {
            if      (strcmp(val, "allow-exclusive") == 0) cfg->share = SHARE_ALLOW_EXCLUSIVE;
            else if (strcmp(val, "force-shared") == 0)    cfg->share = SHARE_FORCE_SHARED;
            else if (strcmp(val, "ignore") == 0)          cfg->share = SHARE_IGNORE;
            else { snprintf(err, err_len, "bad share mode '%s'", val); return false; }
        } else if (strcmp(o, "ipv4") == 0 || strcmp(o, "ipv6") == 0 ||
                   strcmp(o, "lossy") == 0 || strcmp(o, "non-adaptive") == 0) {
            /* accepted for QEMU compatibility; no effect */
        } else {
            snprintf(err, err_len, "unsupported option '%s'", o);
            return false;
        }
        o = next;
    }
    cfg->enabled = true;
    return true;
}

bool gemu_vnc_configure(const char *spec) {
    char err[160] = "";
    VncConfig cfg;
    if (!parse_spec(spec ? spec : "none", &cfg, err, sizeof(err))) {
        fprintf(stderr, "vnc: %s\n", err);
        return false;
    }
    g_cfg = cfg;
    return true;
}

bool gemu_vnc_configured(void) {
    return g_cfg.enabled;
}

/* ── Server state ────────────────────────────────────────────────────────── */

typedef struct {
    int      bytes_pp;
    bool     big_endian;
    uint16_t r_max, g_max, b_max;
    uint8_t  r_shift, g_shift, b_shift;
} PixFmt;

typedef struct VncClient VncClient;

struct GemuVncServer {
    sock_t          listen_fds[VNC_MAX_LISTEN];  /* one per address (v4/v6) */
    int             n_listen;
    pthread_t       accept_thread;
    volatile bool   running;
    char            where[300];          /* listening address, for info vnc */

    pthread_mutex_t lock;
    pthread_cond_t  cond;                /* new frame, or a client state change */

    uint32_t       *fb;                  /* published frame */
    int             w, h;
    uint64_t        frame_seq;

    GemuVncKeyEvent keys[VNC_KEY_QUEUE];
    unsigned        key_head, key_count;
    GemuVncPointerState pointer;
    bool            have_pointer;

    bool            password_required;
    char            password[9];
    bool            password_valid;      /* set and not expired */
    ShareMode       share;

    VncClient      *clients[VNC_MAX_CLIENTS];
    int             n_clients;
};

struct VncClient {
    GemuVncServer *s;
    sock_t         fd;
    char           peer[64];
    pthread_t      reader, writer;
    bool           writer_started;
    volatile bool  alive;

    /* Guarded by s->lock: */
    PixFmt         fmt;
    bool           desktop_size;         /* client handles DesktopSize */
    bool           req;                  /* an update request is pending */
    bool           req_full;             /* ...and it's non-incremental */
    uint64_t       sent_seq;
    int            cw, ch;               /* size the client believes the screen is */

    /* Writer-only: */
    uint32_t      *last;                 /* frame as last sent (cw x ch, scaled) */
    uint32_t       held[VNC_KEY_QUEUE];  /* keysyms held by this client */
    unsigned       n_held;
};

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static bool recvall(sock_t fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        int r = (int)recv(fd, p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool sendall(sock_t fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        int r = (int)send(fd, p, (int)n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ── Pixel formats ───────────────────────────────────────────────────────── */

static PixFmt default_fmt(void) {
    return (PixFmt){ 4, false, 255, 255, 255, 16, 8, 0 };
}

static PixFmt parse_pixfmt(const uint8_t *b) {
    PixFmt f;
    f.bytes_pp   = b[0] / 8;
    if (f.bytes_pp != 1 && f.bytes_pp != 2 && f.bytes_pp != 4) f.bytes_pp = 4;
    f.big_endian = b[2] != 0;
    f.r_max = get16(b + 4); f.g_max = get16(b + 6); f.b_max = get16(b + 8);
    f.r_shift = b[10]; f.g_shift = b[11]; f.b_shift = b[12];
    return f;
}

static void put_pixel(uint8_t *dst, const PixFmt *f, uint32_t argb) {
    uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    uint32_t v = ((r * f->r_max + 127) / 255) << f->r_shift |
                 ((g * f->g_max + 127) / 255) << f->g_shift |
                 ((b * f->b_max + 127) / 255) << f->b_shift;
    for (int i = 0; i < f->bytes_pp; i++) {
        int shift = f->big_endian ? (f->bytes_pp - 1 - i) * 8 : i * 8;
        dst[i] = (uint8_t)(v >> shift);
    }
}

/* ── Input ───────────────────────────────────────────────────────────────── */

/* Caller holds s->lock. */
static void push_key(GemuVncServer *s, uint32_t keysym, bool down) {
    if (s->key_count == VNC_KEY_QUEUE) {               /* drop the oldest */
        s->key_head = (s->key_head + 1) % VNC_KEY_QUEUE;
        s->key_count--;
    }
    s->keys[(s->key_head + s->key_count) % VNC_KEY_QUEUE] =
        (GemuVncKeyEvent){ .keysym = keysym, .down = down };
    s->key_count++;
}

static void client_key(VncClient *c, uint32_t keysym, bool down) {
    GemuVncServer *s = c->s;
    pthread_mutex_lock(&s->lock);
    unsigned i;
    for (i = 0; i < c->n_held && c->held[i] != keysym; i++) {}
    if (down && i == c->n_held && c->n_held < VNC_KEY_QUEUE) c->held[c->n_held++] = keysym;
    if (!down && i < c->n_held) c->held[i] = c->held[--c->n_held];
    push_key(s, keysym, down);
    pthread_mutex_unlock(&s->lock);
}

static void client_pointer(VncClient *c, uint8_t mask, int x, int y) {
    GemuVncServer *s = c->s;
    pthread_mutex_lock(&s->lock);
    /* Map from the client's (possibly stale-size) view to the frame. */
    if (c->cw > 0 && c->ch > 0 && (c->cw != s->w || c->ch != s->h)) {
        x = x * s->w / c->cw;
        y = y * s->h / c->ch;
    }
    GemuVncPointerState *p = &s->pointer;
    if (s->have_pointer) {
        p->rel_x += x - p->x;
        p->rel_y += y - p->y;
    }
    p->x = x;
    p->y = y;
    s->have_pointer = true;
    bool left = mask & 0x01, right = mask & 0x04;
    if (left && !p->button) p->pressed = true;
    if (right && !p->right_button) p->right_pressed = true;
    p->button = left;
    p->right_button = right;
    pthread_mutex_unlock(&s->lock);
}

/* ── Handshake ───────────────────────────────────────────────────────────── */

#ifdef HAVE_CRYPTO
static uint8_t reverse_bits(uint8_t v) {
    v = (uint8_t)(((v & 0xf0u) >> 4) | ((v & 0x0fu) << 4));
    v = (uint8_t)(((v & 0xccu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xaau) >> 1) | ((v & 0x55u) << 1));
    return v;
}
#endif

/* VNC authentication: DES-encrypt a random challenge with the password. */
static bool vnc_auth(VncClient *c) {
    GemuVncServer *s = c->s;
    uint8_t challenge[16], response[16];
#ifdef HAVE_CRYPTO
    if (RAND_bytes(challenge, (int)sizeof(challenge)) != 1)
#endif
        for (size_t i = 0; i < sizeof(challenge); i++) challenge[i] = (uint8_t)rand();
    if (!sendall(c->fd, challenge, 16) || !recvall(c->fd, response, 16)) return false;

    pthread_mutex_lock(&s->lock);
    bool valid = s->password_valid;
    char password[9];
    memcpy(password, s->password, sizeof(password));
    pthread_mutex_unlock(&s->lock);

    bool ok = false;
#ifdef HAVE_CRYPTO
    if (valid) {
        DES_cblock key = {0};
        for (int i = 0; i < 8 && password[i]; i++) key[i] = reverse_bits((uint8_t)password[i]);
        DES_key_schedule sched;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        DES_set_key_unchecked(&key, &sched);
        DES_ecb_encrypt((const_DES_cblock *)challenge, (DES_cblock *)challenge, &sched, DES_ENCRYPT);
        DES_ecb_encrypt((const_DES_cblock *)(challenge + 8), (DES_cblock *)(challenge + 8), &sched, DES_ENCRYPT);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        ok = memcmp(challenge, response, 16) == 0;
    }
#else
    (void)valid; (void)password; (void)response;
#endif
    uint8_t result[4];
    put32(result, ok ? 0 : 1);
    sendall(c->fd, result, 4);
    return ok;
}

static bool handshake(VncClient *c, bool *shared) {
    GemuVncServer *s = c->s;
    char ver[13] = {0};
    if (!sendall(c->fd, "RFB 003.008\n", 12) || !recvall(c->fd, ver, 12)) return false;
    int major = 0, minor = 0;
    if (sscanf(ver, "RFB %d.%d", &major, &minor) != 2 || major != 3) return false;

    uint8_t sec = s->password_required ? 2 : 1;
    if (minor < 7) {                                   /* 3.3: server picks */
        uint8_t b[4];
        put32(b, sec);
        if (!sendall(c->fd, b, 4)) return false;
    } else {
        uint8_t offer[2] = { 1, sec }, chosen;
        if (!sendall(c->fd, offer, 2) || !recvall(c->fd, &chosen, 1) || chosen != sec)
            return false;
    }
    if (sec == 2) {
        if (!vnc_auth(c)) return false;
    } else if (minor >= 8) {
        uint8_t ok[4] = {0};
        if (!sendall(c->fd, ok, 4)) return false;
    }

    uint8_t shared_flag;
    if (!recvall(c->fd, &shared_flag, 1)) return false;
    *shared = shared_flag != 0;

    /* ServerInit: size, our native pixel format (32bpp xRGB), name */
    char name[200];
    gemu_window_title(name, sizeof(name), NULL, false);
    pthread_mutex_lock(&s->lock);
    c->cw = s->w > 0 ? s->w : 640;
    c->ch = s->h > 0 ? s->h : 480;
    pthread_mutex_unlock(&s->lock);
    uint8_t init[24] = {0};
    put16(init, (uint16_t)c->cw);
    put16(init + 2, (uint16_t)c->ch);
    init[4] = 32; init[5] = 24; init[6] = 0; init[7] = 1;
    put16(init + 8, 255); put16(init + 10, 255); put16(init + 12, 255);
    init[14] = 16; init[15] = 8; init[16] = 0;
    put32(init + 20, (uint32_t)strlen(name));
    return sendall(c->fd, init, 24) && sendall(c->fd, name, strlen(name));
}

/* ── Updates ─────────────────────────────────────────────────────────────── */

/* Send one FramebufferUpdate to c from frame (fw x fh).  full = resend
 * everything; resize = announce a new size first. */
static bool send_update(VncClient *c, const uint32_t *frame, int fw, int fh,
                        PixFmt fmt, bool full, bool resize) {
    int cw = c->cw, ch = c->ch;
    size_t npix = (size_t)cw * ch;

    /* What the client should now show, at its size. */
    uint32_t *want = malloc(npix * sizeof(uint32_t));
    if (!want) return false;
    if (fw == cw && fh == ch) {
        memcpy(want, frame, npix * sizeof(uint32_t));
    } else {
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                want[(size_t)y * cw + x] = frame[(size_t)(y * fh / ch) * fw + x * fw / cw];
    }
    if (!c->last || full || resize) {
        free(c->last);
        c->last = calloc(npix, sizeof(uint32_t));
        if (!c->last) { free(want); return false; }
        full = true;
    }

    /* Dirty tiles, merged into horizontal runs per tile row. */
    int tx_n = (cw + VNC_TILE - 1) / VNC_TILE, ty_n = (ch + VNC_TILE - 1) / VNC_TILE;
    int max_rects = tx_n * ty_n + 1;
    int (*rects)[4] = malloc((size_t)max_rects * sizeof(*rects));
    if (!rects) { free(want); return false; }
    int n = 0;
    for (int ty = 0; ty < ty_n; ty++) {
        int y0 = ty * VNC_TILE, th = (y0 + VNC_TILE > ch) ? ch - y0 : VNC_TILE;
        int run_start = -1;
        for (int tx = 0; tx <= tx_n; tx++) {
            bool dirty = false;
            if (tx < tx_n) {
                int x0 = tx * VNC_TILE, tw = (x0 + VNC_TILE > cw) ? cw - x0 : VNC_TILE;
                dirty = full;
                for (int y = y0; !dirty && y < y0 + th; y++)
                    dirty = memcmp(want + (size_t)y * cw + x0, c->last + (size_t)y * cw + x0,
                                   (size_t)tw * sizeof(uint32_t)) != 0;
            }
            if (dirty && run_start < 0) run_start = tx;
            if (!dirty && run_start >= 0) {
                int x0 = run_start * VNC_TILE, x1 = tx * VNC_TILE;
                if (x1 > cw) x1 = cw;
                rects[n][0] = x0; rects[n][1] = y0; rects[n][2] = x1 - x0; rects[n][3] = th;
                n++;
                run_start = -1;
            }
        }
    }

    uint8_t hdr[4] = { 0, 0 };
    put16(hdr + 2, (uint16_t)(n + (resize ? 1 : 0)));
    bool ok = sendall(c->fd, hdr, 4);
    if (ok && resize) {
        uint8_t r[12] = {0};
        put16(r + 4, (uint16_t)cw); put16(r + 6, (uint16_t)ch);
        put32(r + 8, (uint32_t)ENC_DESKTOP_SIZE);
        ok = sendall(c->fd, r, 12);
    }
    uint8_t *row = malloc((size_t)cw * 4);
    if (!row) ok = false;
    for (int i = 0; ok && i < n; i++) {
        int x0 = rects[i][0], y0 = rects[i][1], w = rects[i][2], h = rects[i][3];
        uint8_t r[12];
        put16(r, (uint16_t)x0); put16(r + 2, (uint16_t)y0);
        put16(r + 4, (uint16_t)w); put16(r + 6, (uint16_t)h);
        put32(r + 8, ENC_RAW);
        ok = sendall(c->fd, r, 12);
        for (int y = y0; ok && y < y0 + h; y++) {
            const uint32_t *src = want + (size_t)y * cw + x0;
            for (int x = 0; x < w; x++) put_pixel(row + x * fmt.bytes_pp, &fmt, src[x]);
            ok = sendall(c->fd, row, (size_t)w * fmt.bytes_pp);
        }
    }
    free(row);
    free(rects);
    if (ok) memcpy(c->last, want, npix * sizeof(uint32_t));
    free(want);
    return ok;
}

static void client_kill(VncClient *c) {
    c->alive = false;
    shutdown(c->fd, SHUT_RDWR);   /* wakes a blocked recv/send */
}

static void *writer_main(void *arg) {
    VncClient *c = arg;
    GemuVncServer *s = c->s;
    uint32_t *frame = NULL;
    size_t frame_cap = 0;
    pthread_mutex_lock(&s->lock);
    while (c->alive && s->running) {
        bool size_changed = s->w > 0 && (s->w != c->cw || s->h != c->ch) && c->desktop_size;
        bool ready = c->req && s->fb &&
                     (c->req_full || size_changed || s->frame_seq != c->sent_seq);
        if (!ready) {
            pthread_cond_wait(&s->cond, &s->lock);
            continue;
        }
        size_t need = (size_t)s->w * s->h;
        if (need > frame_cap) {
            free(frame);
            frame = malloc(need * sizeof(uint32_t));
            frame_cap = frame ? need : 0;
            if (!frame) break;
        }
        memcpy(frame, s->fb, need * sizeof(uint32_t));
        int fw = s->w, fh = s->h;
        bool full = c->req_full;
        if (size_changed) { c->cw = fw; c->ch = fh; }
        PixFmt fmt = c->fmt;
        c->req = c->req_full = false;
        c->sent_seq = s->frame_seq;
        pthread_mutex_unlock(&s->lock);

        bool ok = send_update(c, frame, fw, fh, fmt, full, size_changed);

        pthread_mutex_lock(&s->lock);
        if (!ok) client_kill(c);
    }
    pthread_mutex_unlock(&s->lock);
    free(frame);
    return NULL;
}

/* ── Client lifecycle ────────────────────────────────────────────────────── */

static void client_remove(VncClient *c) {
    GemuVncServer *s = c->s;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n_clients; i++) {
        if (s->clients[i] == c) {
            s->clients[i] = s->clients[--s->n_clients];
            break;
        }
    }
    /* Release anything this client was holding so keys don't stick. */
    for (unsigned i = 0; i < c->n_held; i++) push_key(s, c->held[i], false);
    if (s->have_pointer) { s->pointer.button = s->pointer.right_button = false; }
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void *reader_main(void *arg) {
    VncClient *c = arg;
    GemuVncServer *s = c->s;
    bool shared = true;
    if (!handshake(c, &shared)) goto done;

    pthread_mutex_lock(&s->lock);
    /* An exclusive request disconnects everyone else (QEMU's default). */
    if (!shared && s->share == SHARE_ALLOW_EXCLUSIVE)
        for (int i = 0; i < s->n_clients; i++)
            if (s->clients[i] != c) client_kill(s->clients[i]);
    pthread_mutex_unlock(&s->lock);

    fprintf(stderr, "vnc: client %s connected\n", c->peer);
    if (pthread_create(&c->writer, NULL, writer_main, c) == 0) c->writer_started = true;
    else goto done;

    while (c->alive && s->running) {
        uint8_t type;
        if (!recvall(c->fd, &type, 1)) break;
        switch (type) {
        case 0: {                                          /* SetPixelFormat */
            uint8_t b[19];
            if (!recvall(c->fd, b, 19)) goto done;
            pthread_mutex_lock(&s->lock);
            c->fmt = parse_pixfmt(b + 3);
            c->req_full = true;                          /* old pixels are stale */
            pthread_mutex_unlock(&s->lock);
            break;
        }
        case 2: {                                          /* SetEncodings */
            uint8_t b[3];
            if (!recvall(c->fd, b, 3)) goto done;
            bool desk = false;
            for (uint16_t i = 0, n = get16(b + 1); i < n; i++) {
                uint8_t e[4];
                if (!recvall(c->fd, e, 4)) goto done;
                if ((int32_t)get32(e) == ENC_DESKTOP_SIZE) desk = true;
            }
            pthread_mutex_lock(&s->lock);
            c->desktop_size = desk;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->lock);
            break;
        }
        case 3: {                                          /* FramebufferUpdateRequest */
            uint8_t b[9];
            if (!recvall(c->fd, b, 9)) goto done;
            pthread_mutex_lock(&s->lock);
            c->req = true;
            if (!b[0]) c->req_full = true;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->lock);
            break;
        }
        case 4: {                                          /* KeyEvent */
            uint8_t b[7];
            if (!recvall(c->fd, b, 7)) goto done;
            client_key(c, get32(b + 3), b[0] != 0);
            break;
        }
        case 5: {                                          /* PointerEvent */
            uint8_t b[5];
            if (!recvall(c->fd, b, 5)) goto done;
            client_pointer(c, b[0], get16(b + 1), get16(b + 3));
            break;
        }
        case 6: {                                          /* ClientCutText */
            uint8_t b[7];
            if (!recvall(c->fd, b, 7)) goto done;
            for (uint32_t len = get32(b + 3); len; ) {
                uint8_t t[256];
                size_t n = len < sizeof(t) ? len : sizeof(t);
                if (!recvall(c->fd, t, n)) goto done;
                len -= (uint32_t)n;
            }
            break;
        }
        case 150: {                                        /* EnableContinuousUpdates */
            uint8_t b[9];
            if (!recvall(c->fd, b, 9)) goto done;
            break;
        }
        case 248: {                                        /* ClientFence */
            uint8_t b[8];
            if (!recvall(c->fd, b, 8)) goto done;
            for (uint8_t n = b[7]; n; n--) {
                uint8_t t;
                if (!recvall(c->fd, &t, 1)) goto done;
            }
            break;
        }
        default:
            goto done;                                     /* unknown: can't resync */
        }
    }

done:
    pthread_mutex_lock(&s->lock);
    client_kill(c);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    if (c->writer_started) {
        pthread_join(c->writer, NULL);
        fprintf(stderr, "vnc: client %s disconnected\n", c->peer);
    }
    client_remove(c);
    sock_close(c->fd);
    free(c->last);
    free(c);
    return NULL;
}

static void *accept_main(void *arg) {
    GemuVncServer *s = arg;
    while (s->running) {
        fd_set fds;
        FD_ZERO(&fds);
        sock_t maxfd = 0;
        for (int i = 0; i < s->n_listen; i++) {
            FD_SET(s->listen_fds[i], &fds);
            if (s->listen_fds[i] > maxfd) maxfd = s->listen_fds[i];
        }
        struct timeval tv = { 0, 200000 };
        if (select((int)maxfd + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        sock_t lfd = INVALID_SOCK;
        for (int i = 0; i < s->n_listen && lfd == INVALID_SOCK; i++)
            if (FD_ISSET(s->listen_fds[i], &fds)) lfd = s->listen_fds[i];
        if (lfd == INVALID_SOCK) continue;
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        sock_t fd = accept(lfd, (struct sockaddr *)&ss, &sl);
        if (fd == INVALID_SOCK) continue;

        VncClient *c = calloc(1, sizeof(*c));
        if (!c) { sock_close(fd); continue; }
        c->s = s;
        c->fd = fd;
        c->alive = true;
        c->fmt = default_fmt();
        snprintf(c->peer, sizeof(c->peer), "local");
        char host[NI_MAXHOST], serv[NI_MAXSERV];
        if (ss.ss_family != AF_UNSPEC &&
#ifndef _WIN32
            ss.ss_family != AF_UNIX &&
#endif
            getnameinfo((struct sockaddr *)&ss, sl, host, sizeof(host), serv, sizeof(serv),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            snprintf(c->peer, sizeof(c->peer), "%s:%s", host, serv);

        pthread_mutex_lock(&s->lock);
        bool room = s->n_clients < VNC_MAX_CLIENTS;
        if (room) s->clients[s->n_clients++] = c;
        pthread_mutex_unlock(&s->lock);
        pthread_t t;
        if (!room || pthread_create(&t, NULL, reader_main, c) != 0) {
            if (room) client_remove(c);
            sock_close(fd);
            free(c);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

/* ── Listening ───────────────────────────────────────────────────────────── */

static bool addr_in_use(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEADDRINUSE;
#else
    return errno == EADDRINUSE;
#endif
}

/* Listen on every address host resolves to (all interfaces when host is
 * empty), like QEMU: "localhost" gets both ::1 and 127.0.0.1.  Returns the
 * number of sockets added to fds - 0 if the port is taken on any of them
 * (so to= moves on), skipping addresses the host can't use at all. */
static int listen_tcp(const char *host, int port, sock_t *fds, int max_fds,
                      char *where, size_t where_len) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host[0] ? host : NULL, portstr, &hints, &res) != 0) return 0;

    int n = 0;
    where[0] = '\0';
    for (struct addrinfo *ai = res; ai && n < max_fds; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) continue;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
        if (ai->ai_family == AF_INET6)   /* v4 gets its own socket */
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&one, sizeof(one));
        if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0 || listen(s, 4) != 0) {
            bool busy = addr_in_use();
            sock_close(s);
            if (busy) {
                while (n > 0) sock_close(fds[--n]);
                break;
            }
            continue;
        }
        char h[NI_MAXHOST] = "?";
        getnameinfo(ai->ai_addr, (socklen_t)ai->ai_addrlen, h, sizeof(h), NULL, 0, NI_NUMERICHOST);
        size_t l = strlen(where);
        snprintf(where + l, where_len - l, ai->ai_family == AF_INET6 ? "%s[%s]:%d" : "%s%s:%d",
                 n ? ", " : "", h, port);
        fds[n++] = s;
    }
    freeaddrinfo(res);
    return n;
}

static GemuVncServer *server_start(const VncConfig *cfg) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
    GemuVncServer *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    if (cfg->unix_socket) {
#ifndef _WIN32
        sock_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa = {0};
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", cfg->path);
        unlink(cfg->path);
        if (fd != INVALID_SOCK && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
            listen(fd, 4) == 0) {
            s->listen_fds[s->n_listen++] = fd;
            snprintf(s->where, sizeof(s->where), "unix:%s", cfg->path);
        } else if (fd != INVALID_SOCK) {
            sock_close(fd);
        }
#endif
    } else {
        for (int d = cfg->display; d <= cfg->display_max && s->n_listen == 0; d++) {
            s->n_listen = listen_tcp(cfg->host, VNC_PORT_BASE + d, s->listen_fds,
                                     VNC_MAX_LISTEN, s->where, sizeof(s->where));
            if (s->n_listen > 0) {
                size_t l = strlen(s->where);
                snprintf(s->where + l, sizeof(s->where) - l, " (display :%d)", d);
            }
        }
    }
    if (s->n_listen == 0) {
        if (cfg->unix_socket)
            fprintf(stderr, "vnc: cannot listen on unix:%s\n", cfg->path);
        else if (cfg->display == cfg->display_max)
            fprintf(stderr, "vnc: cannot listen on %s:%d (port %d)\n",
                    cfg->host[0] ? cfg->host : "*", cfg->display, VNC_PORT_BASE + cfg->display);
        else
            fprintf(stderr, "vnc: cannot listen on %s, displays :%d-:%d\n",
                    cfg->host[0] ? cfg->host : "*", cfg->display, cfg->display_max);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->password_required = cfg->password;
    s->share = cfg->share;
    s->pointer.x = s->pointer.y = -1;
    s->running = true;
    if (pthread_create(&s->accept_thread, NULL, accept_main, s) != 0) {
        for (int i = 0; i < s->n_listen; i++) sock_close(s->listen_fds[i]);
        free(s);
        return NULL;
    }
    fprintf(stderr, "vnc: listening on %s%s\n", s->where,
            cfg->password ? " (password required)" : "");
#ifndef HAVE_CRYPTO
    if (cfg->password)
        fprintf(stderr, "vnc: built without OpenSSL - VNC authentication can't succeed\n");
#endif
    return s;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

GemuVncServer *gemu_vnc_get(void) {
    if (!g_server && !g_server_tried && g_cfg.enabled) {
        g_server_tried = true;
        g_server = server_start(&g_cfg);
        if (g_server) atexit(gemu_vnc_shutdown);   /* close sockets, remove unix: path */
    }
    return g_server;
}

void gemu_vnc_shutdown(void) {
    GemuVncServer *s = g_server;
    if (!s) return;
    g_server = NULL;
    pthread_mutex_lock(&s->lock);
    s->running = false;
    for (int i = 0; i < s->n_clients; i++) client_kill(s->clients[i]);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->accept_thread, NULL);
    for (int i = 0; i < s->n_listen; i++) sock_close(s->listen_fds[i]);
#ifndef _WIN32
    if (g_cfg.unix_socket) unlink(g_cfg.path);
#endif
    /* Client threads are detached and exit on their own once their sockets
     * close; the server struct is left allocated for them (process exit). */
}

void gemu_vnc_update_argb(GemuVncServer *s, const uint32_t *argb, int w, int h) {
    if (!s || !argb || w <= 0 || h <= 0) return;
    size_t n = (size_t)w * h;
    pthread_mutex_lock(&s->lock);
    if (w != s->w || h != s->h || !s->fb) {
        uint32_t *fb = realloc(s->fb, n * sizeof(uint32_t));
        if (!fb) { pthread_mutex_unlock(&s->lock); return; }
        s->fb = fb;
        s->w = w;
        s->h = h;
    } else if (memcmp(s->fb, argb, n * sizeof(uint32_t)) == 0) {
        pthread_mutex_unlock(&s->lock);            /* unchanged: nothing to send */
        return;
    }
    memcpy(s->fb, argb, n * sizeof(uint32_t));
    s->frame_seq++;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

bool gemu_vnc_pop_key_event(GemuVncServer *s, GemuVncKeyEvent *ev) {
    if (!s || !ev) return false;
    pthread_mutex_lock(&s->lock);
    bool have = s->key_count > 0;
    if (have) {
        *ev = s->keys[s->key_head];
        s->key_head = (s->key_head + 1) % VNC_KEY_QUEUE;
        s->key_count--;
    }
    pthread_mutex_unlock(&s->lock);
    return have;
}

GemuVncPointerState gemu_vnc_get_pointer(GemuVncServer *s) {
    if (!s) return (GemuVncPointerState){ .x = -1, .y = -1 };
    pthread_mutex_lock(&s->lock);
    GemuVncPointerState p = s->pointer;
    s->pointer.rel_x = s->pointer.rel_y = 0;
    s->pointer.pressed = s->pointer.right_pressed = false;
    pthread_mutex_unlock(&s->lock);
    return p;
}

bool gemu_vnc_set_password(GemuVncServer *s, const char *password, char *err, size_t err_len) {
    if (!s) { snprintf(err, err_len, "VNC is not active"); return false; }
    if (!s->password_required) {
        /* QEMU refuses too: authentication must be enabled at startup. */
        snprintf(err, err_len, "password authentication is not enabled (start with -vnc ...,password=on)");
        return false;
    }
#ifndef HAVE_CRYPTO
    snprintf(err, err_len, "built without OpenSSL - VNC authentication is unavailable");
    return false;
#else
    pthread_mutex_lock(&s->lock);
    snprintf(s->password, sizeof(s->password), "%s", password ? password : "");
    s->password_valid = true;
    pthread_mutex_unlock(&s->lock);
    return true;
#endif
}

void gemu_vnc_expire_password(GemuVncServer *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->password_valid = false;
    pthread_mutex_unlock(&s->lock);
}

void gemu_vnc_info(GemuVncServer *s, char *buf, size_t len) {
    if (!s) {
        snprintf(buf, len, "Server: disabled\n");
        return;
    }
    pthread_mutex_lock(&s->lock);
    int n = snprintf(buf, len, "Server: %s\n     auth: %s\n",
                     s->where, s->password_required ? "vnc" : "none");
    if (s->n_clients == 0 && n >= 0 && (size_t)n < len)
        n += snprintf(buf + n, len - (size_t)n, "Client: none\n");
    for (int i = 0; i < s->n_clients && n >= 0 && (size_t)n < len; i++)
        n += snprintf(buf + n, len - (size_t)n, "Client: %s\n", s->clients[i]->peer);
    pthread_mutex_unlock(&s->lock);
}
