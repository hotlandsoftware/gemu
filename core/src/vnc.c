#include "gemu/vnc.h"
#include <openssl/rand.h>
#include <openssl/des.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define INVALID_SOCK   INVALID_SOCKET
#  define sock_close(s)  closesocket(s)
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#else
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
typedef int sock_t;
#  define INVALID_SOCK   (-1)
#  define sock_close(s)  close(s)
#endif

#define GEMU_VNC_KEY_QUEUE_CAP 64u

struct GemuVncServer {
    sock_t          listen_fd;
    pthread_t       thread;
    pthread_mutex_t lock;
    uint8_t        *fb;          /* vram snapshot (1 byte/pixel) */
    int             vram_w, vram_h;
    int             fb_w, fb_h;  /* scaled output dimensions */
    bool            running;
    uint8_t         keys[16];    /* CHIP-8 key state from VNC clients */
    uint32_t        queued_keysym;
    GemuVncKeyEvent key_queue[GEMU_VNC_KEY_QUEUE_CAP];
    unsigned        key_head, key_count;
    uint32_t        held_keysyms[GEMU_VNC_KEY_QUEUE_CAP];
    unsigned        held_count;
    uint32_t        fg_rgb;
    uint32_t        bg_rgb;
    uint32_t        palette[256];
    int             n_colors;
    bool            password_set;
    char            password[9];
};

/* X11 keysyms matching the SDL/GTK CHIP-8 keypad layout:
 *   Keypad  1 2 3 C  →  1 2 3 4
 *           4 5 6 D  →  Q W E R
 *           7 8 9 E  →  A S D F
 *           A 0 B F  →  Z X C V     */
static const uint32_t chip8_keysyms[16] = {
    0x78, /* 0 → x */  0x31, /* 1 → 1 */  0x32, /* 2 → 2 */  0x33, /* 3 → 3 */
    0x71, /* 4 → q */  0x77, /* 5 → w */  0x65, /* 6 → e */  0x61, /* 7 → a */
    0x73, /* 8 → s */  0x64, /* 9 → d */  0x7a, /* A → z */  0x63, /* B → c */
    0x34, /* C → 4 */  0x72, /* D → r */  0x66, /* E → f */  0x76, /* F → v */
};

static void enqueue_key_event(GemuVncServer *vnc, uint32_t keysym, bool down) {
    if (vnc->key_count == GEMU_VNC_KEY_QUEUE_CAP) {
        vnc->key_head = (vnc->key_head + 1u) % GEMU_VNC_KEY_QUEUE_CAP;
        vnc->key_count--;
    }

    unsigned idx = (vnc->key_head + vnc->key_count) % GEMU_VNC_KEY_QUEUE_CAP;
    vnc->key_queue[idx] = (GemuVncKeyEvent){
        .keysym = keysym,
        .down = down,
    };
    vnc->key_count++;
}

static void update_held_key(GemuVncServer *vnc, uint32_t keysym, bool down) {
    for (unsigned i = 0; i < vnc->held_count; i++) {
        if (vnc->held_keysyms[i] != keysym)
            continue;
        if (!down) {
            vnc->held_count--;
            vnc->held_keysyms[i] = vnc->held_keysyms[vnc->held_count];
        }
        return;
    }

    if (down && vnc->held_count < GEMU_VNC_KEY_QUEUE_CAP)
        vnc->held_keysyms[vnc->held_count++] = keysym;
}

static void release_held_keys(GemuVncServer *vnc) {
    pthread_mutex_lock(&vnc->lock);
    for (unsigned i = 0; i < vnc->held_count; i++)
        enqueue_key_event(vnc, vnc->held_keysyms[i], false);
    vnc->held_count = 0;
    memset(vnc->keys, 0, sizeof(vnc->keys));
    pthread_mutex_unlock(&vnc->lock);
}

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static bool recvall(sock_t fd, void *buf, size_t n) {
    char *p = buf;
    while (n) { ssize_t r = recv(fd, p, (int)n, 0); if (r <= 0) return false; p += r; n -= (size_t)r; }
    return true;
}
static bool sendall(sock_t fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) { ssize_t r = send(fd, p, (int)n, MSG_NOSIGNAL); if (r <= 0) return false; p += r; n -= (size_t)r; }
    return true;
}

/* ── Per-connection pixel format ─────────────────────────────────────────── */

typedef struct {
    int bytes_pp;
    bool big_endian;
    uint16_t r_max, g_max, b_max;
    uint8_t r_shift, g_shift, b_shift;
} PixFmt;

static PixFmt default_fmt(void) {
    return (PixFmt){ 4, false, 255, 255, 255, 16, 8, 0 };
}

static PixFmt parse_pixfmt(const uint8_t *b) {
    /* b[0..15] = PixelFormat (bits-per-pixel at b[0], big-endian at b[2], ...) */
    PixFmt f;
    f.bytes_pp   = b[0] / 8; if (f.bytes_pp < 1) f.bytes_pp = 4;
    f.big_endian = (b[2] != 0);
    memcpy(&f.r_max, b + 4, 2); f.r_max = ntohs(f.r_max);
    memcpy(&f.g_max, b + 6, 2); f.g_max = ntohs(f.g_max);
    memcpy(&f.b_max, b + 8, 2); f.b_max = ntohs(f.b_max);
    f.r_shift = b[10];
    f.g_shift = b[11];
    f.b_shift = b[12];
    return f;
}

static uint32_t pack_rgb(const PixFmt *fmt, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFFu;
    uint32_t g = (rgb >> 8) & 0xFFu;
    uint32_t b = rgb & 0xFFu;
    r = (r * fmt->r_max + 127u) / 255u;
    g = (g * fmt->g_max + 127u) / 255u;
    b = (b * fmt->b_max + 127u) / 255u;
    return (r << fmt->r_shift) | (g << fmt->g_shift) | (b << fmt->b_shift);
}

static void write_pixel(uint8_t *dst, uint32_t val, int bpp, bool be) {
    if (be) { for (int i = bpp-1; i >= 0; i--) { dst[i] = val & 0xFF; val >>= 8; } }
    else    { for (int i = 0; i < bpp;  i++)   { dst[i] = val & 0xFF; val >>= 8; } }
}

/* ── RFB handshake ───────────────────────────────────────────────────────── */

static uint8_t reverse_bits(uint8_t v) {
    v = (uint8_t)(((v & 0xf0u) >> 4) | ((v & 0x0fu) << 4));
    v = (uint8_t)(((v & 0xccu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xaau) >> 1) | ((v & 0x55u) << 1));
    return v;
}

static bool vnc_authenticate(GemuVncServer *vnc, sock_t fd) {
    uint8_t challenge[16];
    if (RAND_bytes(challenge, (int)sizeof(challenge)) != 1) {
        for (size_t i = 0; i < sizeof(challenge); i++)
            challenge[i] = (uint8_t)(rand() & 0xff);
    }
    if (!sendall(fd, challenge, sizeof(challenge))) return false;

    uint8_t response[16];
    if (!recvall(fd, response, sizeof(response))) return false;

    char password[9];
    pthread_mutex_lock(&vnc->lock);
    snprintf(password, sizeof(password), "%s", vnc->password);
    pthread_mutex_unlock(&vnc->lock);

    DES_cblock key;
    memset(key, 0, sizeof(key));
    for (int i = 0; i < 8 && password[i]; i++)
        key[i] = reverse_bits((uint8_t)password[i]);
    DES_key_schedule sched;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    DES_set_key_unchecked(&key, &sched);
    DES_ecb_encrypt((const_DES_cblock *)challenge,
                    (DES_cblock *)challenge, &sched, DES_ENCRYPT);
    DES_ecb_encrypt((const_DES_cblock *)(challenge + 8),
                    (DES_cblock *)(challenge + 8), &sched, DES_ENCRYPT);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    uint32_t result = htonl(memcmp(challenge, response, sizeof(challenge)) == 0 ? 0u : 1u);
    if (!sendall(fd, &result, 4)) return false;
    return result == 0;
}

static bool vnc_handshake(GemuVncServer *vnc, sock_t fd) {
    if (!sendall(fd, "RFB 003.008\n", 12)) return false;
    char ver[13]; ver[12] = '\0';
    if (!recvall(fd, ver, 12)) return false;
    int major = 0, minor = 0;
    sscanf(ver, "RFB %d.%03d", &major, &minor);

    pthread_mutex_lock(&vnc->lock);
    bool password_set = vnc->password_set;
    pthread_mutex_unlock(&vnc->lock);

    if (major == 3 && minor == 3) {
        uint32_t sec = htonl(password_set ? 2u : 1u);
        if (!sendall(fd, &sec, 4)) return false;
        if (password_set && !vnc_authenticate(vnc, fd)) return false;
    } else {
        uint8_t offer[2] = {1, password_set ? 2u : 1u};
        if (!sendall(fd, offer, 2)) return false;
        uint8_t chosen; if (!recvall(fd, &chosen, 1)) return false;
        if (chosen != (password_set ? 2u : 1u)) return false;
        if (password_set) {
            if (!vnc_authenticate(vnc, fd)) return false;
        } else if (minor >= 8) {
            uint32_t ok = 0; if (!sendall(fd, &ok, 4)) return false;
        }
    }
    uint8_t shared; return recvall(fd, &shared, 1);
}

static bool vnc_send_server_init(GemuVncServer *vnc, sock_t fd) {
    uint8_t msg[24];
    uint16_t w = htons((uint16_t)vnc->fb_w), h = htons((uint16_t)vnc->fb_h);
    memcpy(msg, &w, 2); memcpy(msg+2, &h, 2);
    msg[4]=32; msg[5]=24; msg[6]=0; msg[7]=1;
    uint16_t mx = htons(255);
    memcpy(msg+8, &mx, 2); memcpy(msg+10, &mx, 2); memcpy(msg+12, &mx, 2);
    msg[14]=16; msg[15]=8; msg[16]=0; msg[17]=msg[18]=msg[19]=0;
    uint32_t nl = htonl(4); memcpy(msg+20, &nl, 4);
    return sendall(fd, msg, 24) && sendall(fd, "GEMU", 4);
}

/* ── FramebufferUpdate ───────────────────────────────────────────────────── */

static bool vnc_send_update(GemuVncServer *vnc, sock_t fd, const PixFmt *fmt) {
    int npix = vnc->fb_w * vnc->fb_h;

    pthread_mutex_lock(&vnc->lock);
    int vw = vnc->vram_w, vh = vnc->vram_h;
    uint8_t *vsnap = (vw > 0 && vh > 0) ? malloc((size_t)(vw * vh)) : NULL;
    if (vsnap) memcpy(vsnap, vnc->fb, (size_t)(vw * vh));
    uint32_t palette[256];
    int n_colors = vnc->n_colors;
    if (n_colors > 0)
        memcpy(palette, vnc->palette, (size_t)n_colors * sizeof(palette[0]));
    uint32_t fg_rgb = vnc->fg_rgb;
    uint32_t bg_rgb = vnc->bg_rgb;
    pthread_mutex_unlock(&vnc->lock);

    size_t out_sz = (size_t)(npix * fmt->bytes_pp);
    uint8_t *out  = malloc(out_sz);
    if (!out) { free(vsnap); return false; }

    uint32_t fg = pack_rgb(fmt, fg_rgb);
    uint32_t bg = pack_rgb(fmt, bg_rgb);
    for (int i = 0; i < npix; i++)
        write_pixel(out + i * fmt->bytes_pp, bg, fmt->bytes_pp, fmt->big_endian);

    if (vsnap) {
        for (int y = 0; y < vnc->fb_h; y++) {
            for (int x = 0; x < vnc->fb_w; x++) {
                int sx = x * vw / vnc->fb_w, sy = y * vh / vnc->fb_h;
                uint8_t idx = vsnap[sy * vw + sx];
                uint32_t px = idx ? fg : bg;
                if (n_colors > 0) {
                    if (idx >= n_colors) idx = (uint8_t)(n_colors - 1);
                    px = pack_rgb(fmt, palette[idx] & 0xFFFFFFu);
                }
                write_pixel(out + (y * vnc->fb_w + x) * fmt->bytes_pp,
                            px, fmt->bytes_pp, fmt->big_endian);
            }
        }
        free(vsnap);
    }

    uint8_t hdr[4]  = {0, 0, 0, 1};
    uint8_t rect[12]; memset(rect, 0, 4);
    uint16_t rw = htons((uint16_t)vnc->fb_w), rh = htons((uint16_t)vnc->fb_h);
    memcpy(rect+4, &rw, 2); memcpy(rect+6, &rh, 2); memset(rect+8, 0, 4);

    bool ok = sendall(fd, hdr, 4) && sendall(fd, rect, 12) && sendall(fd, out, out_sz);
    free(out);
    return ok;
}

/* ── Client message loop ─────────────────────────────────────────────────── */

static void vnc_handle_client(GemuVncServer *vnc, sock_t fd) {
    if (!vnc_handshake(vnc, fd) || !vnc_send_server_init(vnc, fd)) return;
    PixFmt fmt = default_fmt();
    while (vnc->running) {
        uint8_t type; if (!recvall(fd, &type, 1)) break;
        switch (type) {
        case 0: { /* SetPixelFormat */
            uint8_t b[19]; if (!recvall(fd, b, 19)) return;
            fmt = parse_pixfmt(b + 3); break; /* b[0..2]=pad, b[3..18]=PixelFormat */
        }
        case 2: { /* SetEncodings */
            uint8_t b[3]; if (!recvall(fd, b, 3)) return;
            uint16_t cnt; memcpy(&cnt, b+1, 2); cnt = ntohs(cnt);
            for (uint16_t i = 0; i < cnt; i++) { uint8_t e[4]; if (!recvall(fd, e, 4)) return; }
            break;
        }
        case 3: { /* FramebufferUpdateRequest */
            uint8_t b[9]; if (!recvall(fd, b, 9)) return;
            if (!vnc_send_update(vnc, fd, &fmt)) return;
            break;
        }
        case 4: { /* KeyEvent: 1 down-flag + 2 pad + 4 keysym */
            uint8_t b[7]; if (!recvall(fd, b, 7)) return;
            uint8_t down = b[0];
            uint32_t sym; memcpy(&sym, b+3, 4); sym = ntohl(sym);
            uint32_t raw_sym = sym;
            if (sym >= 0x41 && sym <= 0x5A) sym |= 0x20; /* normalize to lowercase */
            pthread_mutex_lock(&vnc->lock);
            enqueue_key_event(vnc, sym, down != 0);
            update_held_key(vnc, sym, down != 0);
            if (down) vnc->queued_keysym = raw_sym;
            for (int k = 0; k < 16; k++)
                if (chip8_keysyms[k] == sym) { vnc->keys[k] = down; break; }
            pthread_mutex_unlock(&vnc->lock);
            break;
        }
        case 5: { uint8_t b[5]; if (!recvall(fd, b, 5)) return; break; } /* PointerEvent */
        case 6: { /* ClientCutText */
            uint8_t b[7]; if (!recvall(fd, b, 7)) return;
            uint32_t len; memcpy(&len, b+3, 4); len = ntohl(len);
            while (len) { uint8_t t[256]; size_t c = len<256?len:256; if (!recvall(fd,t,c)) return; len-=(uint32_t)c; }
            break;
        }
        case 150: { /* EnableContinuousUpdates (RealVNC/Tight) */
            uint8_t b[9]; if (!recvall(fd, b, 9)) return; break;
        }
        case 248: { /* ClientFence */
            uint8_t b[8]; if (!recvall(fd, b, 8)) return; /* 3 pad + 4 flags + 1 len */
            uint8_t plen = b[7];
            while (plen--) { uint8_t t; if (!recvall(fd, &t, 1)) return; }
            break;
        }
        default: return;
        }
    }
}

static void *vnc_thread(void *arg) {
    GemuVncServer *vnc = arg;
    while (vnc->running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(vnc->listen_fd, &fds);
        struct timeval tv = {1, 0};
        if (select((int)(vnc->listen_fd) + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        sock_t cfd = accept(vnc->listen_fd, NULL, NULL);
        if (cfd == INVALID_SOCK) continue;
        printf("vnc: client connected\n");
        vnc_handle_client(vnc, cfd);
        release_held_keys(vnc);
        printf("vnc: client disconnected\n");
        sock_close(cfd);
    }
    return NULL;
}

/* ── Address parsing ─────────────────────────────────────────────────────── */

static bool parse_vnc_addr(const char *s, char host[64], int *port) {
    const char *colon = strrchr(s, ':');
    if (!colon) return false;
    *port = 5900 + atoi(colon + 1);
    size_t hlen = (size_t)(colon - s);
    if      (hlen == 0)   { memcpy(host, "0.0.0.0", 8); }
    else if (hlen < 64)   { memcpy(host, s, hlen); host[hlen] = '\0'; }
    else                  { return false; }
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

GemuVncServer *gemu_vnc_create(const char *addr, int fb_w, int fb_h) {
    char host[64]; int port;
    if (!parse_vnc_addr(addr, host, &port)) {
        fprintf(stderr, "vnc: bad address '%s' — use host:N or :N\n", addr); return NULL;
    }
#ifdef _WIN32
    { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
#endif
    sock_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == INVALID_SOCK) { perror("vnc: socket"); return NULL; }
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, (int)sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("vnc: bind"); sock_close(lfd); return NULL;
    }
    listen(lfd, 1);
    GemuVncServer *vnc = calloc(1, sizeof(*vnc));
    vnc->listen_fd = lfd;
    vnc->fb_w = fb_w; vnc->fb_h = fb_h;
    vnc->fg_rgb = 0xFFFFFFu;
    vnc->bg_rgb = 0x000000u;
    vnc->running = true;
    pthread_mutex_init(&vnc->lock, NULL);
    pthread_create(&vnc->thread, NULL, vnc_thread, vnc);
    printf("vnc: listening on %s:%d (display :%d)\n", host, port, port - 5900);
    return vnc;
}

void gemu_vnc_set_colors(GemuVncServer *vnc, uint32_t fg_rgb, uint32_t bg_rgb) {
    if (!vnc) return;
    pthread_mutex_lock(&vnc->lock);
    vnc->fg_rgb = fg_rgb & 0xFFFFFFu;
    vnc->bg_rgb = bg_rgb & 0xFFFFFFu;
    vnc->n_colors = 0;
    pthread_mutex_unlock(&vnc->lock);
}

void gemu_vnc_set_password(GemuVncServer *vnc, const char *password) {
    if (!vnc) return;
    pthread_mutex_lock(&vnc->lock);
    snprintf(vnc->password, sizeof(vnc->password), "%s", password ? password : "");
    vnc->password_set = vnc->password[0] != '\0';
    pthread_mutex_unlock(&vnc->lock);
}

void gemu_vnc_set_palette(GemuVncServer *vnc,
                          const uint32_t *palette, int n_colors) {
    if (!vnc) return;
    if (!palette || n_colors <= 0) {
        pthread_mutex_lock(&vnc->lock);
        vnc->n_colors = 0;
        pthread_mutex_unlock(&vnc->lock);
        return;
    }
    if (n_colors > 256) n_colors = 256;
    pthread_mutex_lock(&vnc->lock);
    for (int i = 0; i < n_colors; i++)
        vnc->palette[i] = palette[i] & 0xFFFFFFu;
    vnc->n_colors = n_colors;
    pthread_mutex_unlock(&vnc->lock);
}

void gemu_vnc_destroy(GemuVncServer *vnc) {
    if (!vnc) return;
    vnc->running = false;
    pthread_join(vnc->thread, NULL);
    sock_close(vnc->listen_fd);
    pthread_mutex_destroy(&vnc->lock);
    free(vnc->fb);
    free(vnc);
#ifdef _WIN32
    WSACleanup();
#endif
}

void gemu_vnc_get_keys(GemuVncServer *vnc, uint8_t keys[16]) {
    if (!vnc) { memset(keys, 0, 16); return; }
    pthread_mutex_lock(&vnc->lock);
    memcpy(keys, vnc->keys, 16);
    pthread_mutex_unlock(&vnc->lock);
}

uint32_t gemu_vnc_pop_keysym(GemuVncServer *vnc) {
    if (!vnc) return 0;
    pthread_mutex_lock(&vnc->lock);
    uint32_t sym = vnc->queued_keysym;
    vnc->queued_keysym = 0;
    pthread_mutex_unlock(&vnc->lock);
    return sym;
}

bool gemu_vnc_pop_key_event(GemuVncServer *vnc, GemuVncKeyEvent *event) {
    if (!vnc || !event) return false;
    pthread_mutex_lock(&vnc->lock);
    if (vnc->key_count == 0) {
        pthread_mutex_unlock(&vnc->lock);
        return false;
    }
    *event = vnc->key_queue[vnc->key_head];
    vnc->key_head = (vnc->key_head + 1u) % GEMU_VNC_KEY_QUEUE_CAP;
    vnc->key_count--;
    pthread_mutex_unlock(&vnc->lock);
    return true;
}

void gemu_vnc_update(GemuVncServer *vnc,
                     const uint8_t *vram, int vw, int vh) {
    if (!vnc) return;
    pthread_mutex_lock(&vnc->lock);
    if (vnc->vram_w != vw || vnc->vram_h != vh) {
        free(vnc->fb);
        vnc->fb = malloc((size_t)(vw * vh));
        vnc->vram_w = vw; vnc->vram_h = vh;
    }
    if (vnc->fb) memcpy(vnc->fb, vram, (size_t)(vw * vh));
    pthread_mutex_unlock(&vnc->lock);
}
