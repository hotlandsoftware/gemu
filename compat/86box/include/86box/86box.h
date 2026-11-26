/*
 * The part of 86Box v6.0's 86box.h that drivers use, for GEMU's 86Box layer
 * (compat/86box): logging, fatal errors and the helper macros.  86Box's
 * global settings (gfxcard[], machine, cpu, ...) are not here: a driver that
 * reads them needs its port to decide what they mean in GEMU.
 */
#ifndef EMU_86BOX_H
#define EMU_86BOX_H
#include <stdarg.h>
#include <stdint.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(x)    ((x) > 0 ? (x) : -(x))
#define ABSD(x)   ((x) > 0.0 ? (x) : -(x))
#define BCD8(x)   ((((x) / 10) << 4) | ((x) % 10))
#define BCD16(x)  ((((x) / 1000) << 12) | (((x) / 100) << 8) | BCD8(x))
#define BCD32(x)  ((((x) / 10000000) << 28) | (((x) / 1000000) << 24) | (((x) / 100000) << 20) | (((x) / 10000) << 16) | BCD16(x))

#define AS_U8(x)     (*((uint8_t *) &(x)))
#define AS_U16(x)    (*((uint16_t *) &(x)))
#define AS_U32(x)    (*((uint32_t *) &(x)))
#define AS_U64(x)    (*((uint64_t *) &(x)))
#define AS_I8(x)     (*((int8_t *) &(x)))
#define AS_I16(x)    (*((int16_t *) &(x)))
#define AS_I32(x)    (*((int32_t *) &(x)))
#define AS_I64(x)    (*((int64_t *) &(x)))
#define AS_FLOAT(x)  (*((float *) &(x)))
#define AS_DOUBLE(x) (*((double *) &(x)))

#if defined(__GNUC__) || defined(__clang__)
#    define UNLIKELY(x) __builtin_expect(!!(x), 0)
#    define LIKELY(x)   __builtin_expect(!!(x), 1)
#else
#    define UNLIKELY(x) (x)
#    define LIKELY(x)   (x)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* To stderr, as GEMU logs. */
extern void pclog_ex(const char *fmt, va_list ap);
extern void pclog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A driver found something it can't go on from: report it and stop GEMU. */
extern void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#ifdef __cplusplus
}
#endif

#endif /*EMU_86BOX_H*/
