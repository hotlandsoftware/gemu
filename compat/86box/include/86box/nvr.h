/*
 * The part of 86Box v6.0's nvr.h devices use for their own NVRAM files,
 * for GEMU's 86Box layer (compat/86box): nvr_fopen() opens one in GEMU's
 * settings directory, under 86box/ (~/.gemu/86box/NAME on Unix).  86Box's
 * RTC/CMOS NVRAM itself isn't here.
 *
 * From 86Box (src/include/86box/nvr.h), GPLv2 or later.
 */
#ifndef EMU_NVR_H
#define EMU_NVR_H
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern FILE *nvr_fopen(char *str, char *mode);

#ifdef __cplusplus
}
#endif

#endif /*EMU_NVR_H*/
