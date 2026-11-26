/*
 * The part of 86Box v6.0's plat.h that drivers use, for GEMU's 86Box layer
 * (compat/86box).  The host-platform functions (plat_*, files, threads,
 * the UI) are not here: a driver that needs one doesn't compile until the
 * layer provides it.
 */
#ifndef EMU_PLAT_H
#define EMU_PLAT_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <86box/plat_unused.h>

#ifdef _WIN32
#    define wcscasecmp _wcsicmp
#    define strcasecmp _stricmp
#endif

/* Return the size (in wchar's) of a wchar_t array. */
#define sizeof_w(x) (sizeof((x)) / sizeof(wchar_t))

#endif /*EMU_PLAT_H*/
