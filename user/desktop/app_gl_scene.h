#ifndef KESTREL_DESKTOP_GL_SCENE_H
#define KESTREL_DESKTOP_GL_SCENE_H
#include "gui.h"

typedef struct gl_scene gl_scene_t;
/* GPU-only desktop scene; no software renderer is selected on failure. */
bool gl_scene_render(gl_scene_t **state, surface_t *canvas, int top, int w, int h,
                     unsigned scene, float spin_degrees, float pitch_degrees,
                     bool lit, bool textured);
void gl_scene_destroy(gl_scene_t *state);
#endif
