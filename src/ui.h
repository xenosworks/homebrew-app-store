// The store's screens, drawn with Dear ImGui (ui.cpp).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	UI_QUIT,
	// An installed app to start; the store itself to start again, updated.
	UI_LAUNCH,
	UI_RELAUNCH,
} UiExit;

// After xsdk_present_open(); ui_close() before xsdk_present_close().
bool ui_open(void);
void ui_close(void);
// Runs the screens until B on the categories, or until A launches an installed app or the store has updated itself,
// the .xex to start then written to xex. With shots, goes through the screens by itself, writing each as
// shot_<screen>.ppm beside the .xex, and ends: the categories, the first category's apps, its first app's page, that
// app installed, then the second category's first app's page with the word before it installs, if it isn't by
// xenosworks.
UiExit ui_run(uint32_t width, uint32_t height, const StoreConfig *config, bool shots, char *xex, size_t xex_bytes);

#ifdef __cplusplus
}
#endif
