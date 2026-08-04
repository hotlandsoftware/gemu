#pragma once

#include "gemu/display.h"
#include <stdbool.h>

typedef enum {
    MITSU_MACHINE_TVGAME6,
} MitsuMachineType;

typedef struct {
    MitsuMachineType machine;
    GemuDisplayType  display_type;
    int              display_scale;
    bool             no_shutdown;
} MitsuConfig;

typedef struct MitsuTvGame6State MitsuTvGame6State;

MitsuTvGame6State *mitsu_tvgame6_create(const MitsuConfig *cfg);
void               mitsu_tvgame6_run(MitsuTvGame6State *s, const MitsuConfig *cfg);
void               mitsu_tvgame6_destroy(MitsuTvGame6State *s);
