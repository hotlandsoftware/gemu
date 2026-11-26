/* 86Box v6.0's plat_unused.h, for GEMU's 86Box layer (compat/86box). */
#ifndef EMU_PLAT_UNUSED_H
#define EMU_PLAT_UNUSED_H

#if defined(__GNUC__) || defined(__clang__)
#    define UNUSED(arg) __attribute__((unused)) arg
#else
#    define UNUSED(arg) arg
#endif

#endif /*EMU_PLAT_UNUSED_H*/
