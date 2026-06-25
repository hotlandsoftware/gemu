#pragma once
#include "rob.h"
#include <SDL2/SDL.h>

typedef struct RobWindow RobWindow;

/* model_dir: directory containing ROB.gltf */
RobWindow *rob_window_create (const char *model_dir);
void        rob_window_render (RobWindow *w, const RobState *rob);
/* Returns 1 if the ROB window was closed. */
int         rob_window_event  (RobWindow *w, const SDL_Event *e);
void        rob_window_destroy(RobWindow *w);
