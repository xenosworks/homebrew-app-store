// The store's screens, drawn with Dear ImGui (ui.cpp).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// After xsdk_present_open(); ui_close() before xsdk_present_close().
bool ui_open(void);
void ui_close(void);
// Runs the screens until B on the categories, apps installing in the folder apps names, its backslash after it. With
// shots, goes through the screens by itself, writing each as shot_<screen>.ppm beside the .xex, and ends: the
// categories, the first category's apps, its first app's page, that app installed, then the second category's first
// app's page with the word before it installs, if it isn't by xenosworks.
void ui_run(uint32_t width, uint32_t height, const char *apps, bool shots);

#ifdef __cplusplus
}
#endif
