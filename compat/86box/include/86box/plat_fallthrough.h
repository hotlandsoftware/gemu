/* 86Box v6.0's plat_fallthrough.h, for GEMU's 86Box layer (compat/86box):
 * the `fallthrough` statement drivers put between switch cases. */
#ifndef EMU_PLAT_FALLTHROUGH_H
#define EMU_PLAT_FALLTHROUGH_H

#ifndef fallthrough
#  if defined(__has_attribute)
#    if __has_attribute(fallthrough)
#      define fallthrough __attribute__((fallthrough))
#    endif
#  endif
#  ifndef fallthrough
#    define fallthrough do {} while (0) /* fallthrough */
#  endif
#endif

#endif /*EMU_PLAT_FALLTHROUGH_H*/
