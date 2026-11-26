/* 86Box's Novell NetWare 2.x Key Card (third_party/86box/v6.0), unchanged,
 * as GEMU device type "novellkeycard".  Its serial number comes from its
 * option and nothing changes after init: no state to save. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"     /* upstream's loop over sizeof */
#include "../../../third_party/86box/v6.0/src/device/novell_cardkey.c"
#pragma GCC diagnostic pop
#include "../gemu_86box.h"

GEMU_86BOX_PORT(gemu_86box_novellkeycard_type, novell_keycard_device, "novellkeycard",
                NULL, NULL, true);
