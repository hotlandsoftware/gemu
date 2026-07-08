#include "gemu/screendump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool gemu_screendump(const char *path, const uint8_t *rgb, int w, int h) {
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
