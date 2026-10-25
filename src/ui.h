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
// Runs the screens until B on the categories. With shots, opens the first category and its first app by itself,
// writes each screen as shot_<screen>.ppm beside the .xex, and ends.
void ui_run(uint32_t width, uint32_t height, bool shots);

#ifdef __cplusplus
}
#endif
