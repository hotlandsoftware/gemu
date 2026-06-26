#pragma once
#include "rob.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

typedef struct RobWindow RobWindow;

/* model_dir: directory containing ROB.gltf */
RobWindow *rob_window_create (const char *model_dir, bool famicom_skin);
void        rob_window_render (RobWindow *w, const RobState *rob);
/* Returns true if the user has clicked the ROB window's close button. */
bool        rob_window_close_requested(const RobWindow *w);
void        rob_window_destroy(RobWindow *w);
