#include "gemu/screendump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

/* ── PPM ─────────────────────────────────────────────────────────────────── */

static bool gemu_screendump_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "screendump: cannot open '%s'\n", path); return false; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * (size_t)h * 3, f);
    fclose(f);
    printf("screendump: saved '%s'\n", path);
    return true;
}

/* ── PNG (uncompressed deflate, no external dependencies) ────────────────── */

/* CRC32 (IEEE 802.3 / PNG) */
static const uint32_t *crc32_tbl(void) {
    static uint32_t tbl[256];
    static bool     done = false;
    if (!done) {
        for (unsigned n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[n] = c;
        }
        done = true;
    }
    return tbl;
}

/* Adler-32 (zlib checksum) */
static uint32_t adler32(const uint8_t *d, size_t n) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        s1 = (s1 + d[i]) % 65521u;
        s2 = (s2 + s1)   % 65521u;
    }
    return (s2 << 16) | s1;
}

static void w32be(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16);
    b[2]=(uint8_t)(v>>8);  b[3]=(uint8_t)v;
}

static void png_chunk(FILE *f, const char type[4],
                      const uint8_t *data, uint32_t len) {
    uint8_t tmp[4];
    const uint32_t *t = crc32_tbl();

    w32be(tmp, len);  fwrite(tmp,  1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);

    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) c = t[(c ^ (uint8_t)type[i]) & 0xFF] ^ (c >> 8);
    for (uint32_t i = 0; i < len; i++) c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    w32be(tmp, c ^ 0xFFFFFFFFu); fwrite(tmp, 1, 4, f);
}

static bool gemu_screendump_png(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "screendump: cannot open '%s'\n", path); return false; }

    /* PNG signature */
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    fwrite(sig, 1, 8, f);

    /* IHDR */
    {
        uint8_t ihdr[13];
        w32be(ihdr+0, (uint32_t)w);
        w32be(ihdr+4, (uint32_t)h);
        ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
        png_chunk(f, "IHDR", ihdr, 13);
    }

    /* Build raw scanline data: 1 filter byte (None=0) + RGB per row */
    size_t row_bytes = 1 + (size_t)w * 3;
    size_t raw_size  = row_bytes * (size_t)h;
    uint8_t *raw = malloc(raw_size);
    if (!raw) { fclose(f); return false; }
    for (int y = 0; y < h; y++) {
        raw[y * row_bytes] = 0;
        memcpy(raw + y * row_bytes + 1, rgb + (size_t)y * (size_t)w * 3,
               (size_t)w * 3);
    }

    /* Build zlib stream: header + uncompressed deflate blocks + Adler-32 */
    int    n_blk     = (int)((raw_size + 65534) / 65535);
    size_t zlib_size = 2 + (size_t)n_blk * 5 + raw_size + 4;
    uint8_t *zlib = malloc(zlib_size);
    if (!zlib) { free(raw); fclose(f); return false; }

    size_t zi = 0;
    zlib[zi++] = 0x78;   /* CMF: deflate, 32 K window  */
    zlib[zi++] = 0x01;   /* FLG: (0x7801 % 31 == 0) ✓ */

    size_t pos = 0;
    for (int blk = 0; blk < n_blk; blk++) {
        size_t   avail = raw_size - pos;
        uint16_t blen  = (uint16_t)(avail > 65535 ? 65535 : avail);
        uint16_t nlen  = (uint16_t)(~blen);
        zlib[zi++] = (blk == n_blk - 1) ? 0x01 : 0x00; /* BFINAL | BTYPE=00 */
        zlib[zi++] = (uint8_t)(blen);
        zlib[zi++] = (uint8_t)(blen >> 8);
        zlib[zi++] = (uint8_t)(nlen);
        zlib[zi++] = (uint8_t)(nlen >> 8);
        memcpy(zlib + zi, raw + pos, blen);
        zi  += blen;
        pos += blen;
    }

    uint8_t cksum[4];
    w32be(cksum, adler32(raw, raw_size));
    memcpy(zlib + zi, cksum, 4);
    zi += 4;

    png_chunk(f, "IDAT", zlib, (uint32_t)zi);
    png_chunk(f, "IEND", NULL, 0);

    free(raw);
    free(zlib);
    fclose(f);
    printf("screendump: saved '%s'\n", path);
    return true;
}

/* ── Dispatcher ──────────────────────────────────────────────────────────── */

/* ── Live preview (see screendump.h) ─────────────────────────────────────── */

#ifndef _WIN32
static int             preview_fd = -2;          /* -2 = not looked yet, -1 = none */
static pthread_t       preview_thread;
static pthread_mutex_t preview_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  preview_cond = PTHREAD_COND_INITIALIZER;
static uint8_t        *preview_frame;            /* the next frame to send, NULL = none */
static size_t          preview_len;
static struct timespec preview_next;

static void *preview_writer(void *arg) {
    (void)arg;
    /* a closed pipe fails the write (EPIPE) instead of killing the machine */
    sigset_t pipe_sig;
    sigemptyset(&pipe_sig);
    sigaddset(&pipe_sig, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_sig, NULL);
    for (;;) {
        pthread_mutex_lock(&preview_lock);
        while (!preview_frame) pthread_cond_wait(&preview_cond, &preview_lock);
        uint8_t *f = preview_frame;
        size_t len = preview_len;
        preview_frame = NULL;
        pthread_mutex_unlock(&preview_lock);
        for (size_t off = 0; off < len; ) {
            ssize_t n = write(preview_fd, f + off, len - off);
            if (n <= 0) { free(f); close(preview_fd); return NULL; }   /* manager gone */
            off += (size_t)n;
        }
        free(f);
    }
}

bool gemu_preview_enabled(void) {
    if (preview_fd == -2) {
        const char *v = getenv("GEMU_PREVIEW_FD");
        char *end;
        long fd = v ? strtol(v, &end, 10) : -1;
        preview_fd = v && *v && !*end && fd >= 0 ? (int)fd : -1;
        /* don't hand the pipe to anything this process starts */
        unsetenv("GEMU_PREVIEW_FD");
        if (preview_fd >= 0 && pthread_create(&preview_thread, NULL, preview_writer, NULL) != 0)
            preview_fd = -1;
        if (preview_fd >= 0) {
            pthread_detach(preview_thread);
            clock_gettime(CLOCK_MONOTONIC, &preview_next);
            preview_next.tv_sec += 1;                 /* the first one soon after start */
        }
    }
    return preview_fd >= 0;
}

bool gemu_preview_due(void) {
    if (!gemu_preview_enabled()) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec < preview_next.tv_sec ||
        (now.tv_sec == preview_next.tv_sec && now.tv_nsec < preview_next.tv_nsec))
        return false;
    preview_next = now;
    preview_next.tv_sec += GEMU_PREVIEW_INTERVAL_MS / 1000;
    preview_next.tv_nsec += (long)(GEMU_PREVIEW_INTERVAL_MS % 1000) * 1000000L;
    if (preview_next.tv_nsec >= 1000000000L) { preview_next.tv_sec++; preview_next.tv_nsec -= 1000000000L; }
    return true;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Scale to fit the preview box (averaging the source pixels each covers)
 * and queue it for the writer, replacing one it hasn't sent yet. */
static bool preview_send(const uint8_t *rgb, int w, int h) {
    if (!gemu_preview_enabled() || w <= 0 || h <= 0) return false;
    double s = 1.0;
    if (w > GEMU_PREVIEW_MAX_W) s = (double)GEMU_PREVIEW_MAX_W / w;
    if (h * s > GEMU_PREVIEW_MAX_H) s = (double)GEMU_PREVIEW_MAX_H / h;
    int dw = (int)(w * s + 0.5), dh = (int)(h * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    size_t len = 16 + (size_t)dw * dh * 3;
    uint8_t *f = malloc(len);
    if (!f) return false;
    memcpy(f, GEMU_PREVIEW_MAGIC, 8);
    put32(f + 8, (uint32_t)dw);
    put32(f + 12, (uint32_t)dh);
    uint8_t *o = f + 16;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sum[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = rgb + ((size_t)yy * w + xx) * 3;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    n++;
                }
            for (int c = 0; c < 3; c++) *o++ = (uint8_t)(sum[c] / n);
        }
    }
    pthread_mutex_lock(&preview_lock);
    free(preview_frame);
    preview_frame = f;
    preview_len = len;
    pthread_cond_signal(&preview_cond);
    pthread_mutex_unlock(&preview_lock);
    return true;
}
#else
bool gemu_preview_enabled(void) { return false; }
bool gemu_preview_due(void) { return false; }
static bool preview_send(const uint8_t *rgb, int w, int h) { (void)rgb; (void)w; (void)h; return false; }
#endif

bool gemu_screendump(const char *path, const uint8_t *rgb, int w, int h) {
    if (strcmp(path, GEMU_PREVIEW_PATH) == 0) return preview_send(rgb, w, h);
    const char *dot = strrchr(path, '.');
    if (dot && strcmp(dot, ".png") == 0)
        return gemu_screendump_png(path, rgb, w, h);
    return gemu_screendump_ppm(path, rgb, w, h);
}

/* ── Framebuffer-format frontends ────────────────────────────────────────── */

bool gemu_screendump_argb(const char *path, const uint32_t *argb, int w, int h) {
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = argb[i];
        rgb[i * 3 + 0] = (uint8_t)(c >> 16);
        rgb[i * 3 + 1] = (uint8_t)(c >> 8);
        rgb[i * 3 + 2] = (uint8_t)(c);
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

bool gemu_screendump_mono(const char *path, const uint8_t *pixels, int w, int h) {
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint8_t v = pixels[i] ? 0xFF : 0x00;
        rgb[i*3+0] = v; rgb[i*3+1] = v; rgb[i*3+2] = v;
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}
