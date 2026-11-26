/*
 * GEMU's 86Box layer: interrupt requests (<86box/pic.h>) on the current
 * device's irq0..irq15 lines.
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/pic.h>

void picint_common(uint16_t num, int level, int set, uint8_t *irq_state) {
    Gemu86BoxDev *b = gemu_86box_need("picint");
    for (unsigned i = 0; i < 16; i++) {
        if (!(num & (1u << i)))
            continue;
        GemuLine *l = b->irq[i];
        if (level) {
            gemu_line_set(l, set);
        } else if (set) {
            if (gemu_line_get(l))         /* each picint() is a request */
                gemu_line_set(l, false);
            gemu_line_set(l, true);
        } else {
            gemu_line_set(l, false);
        }
    }
    if (level && irq_state)
        *irq_state = (uint8_t)!!set;
}
