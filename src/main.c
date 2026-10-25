// The Homebrew App Store (xsdk's docs/scope/63-homebrew-store.md): the splash, then the catalogue's screens until B on
// the first. store.txt beside the .xex, for checks against a PC, may hold url=, ca= and resolve= for the catalogue's
// address, the roots TLS checks against and a host:port:address for curl, apps= for a folder beside the .xex to install
// apps in, and shots=1 to go through the screens and write each (ui.h).

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "store.h"
#include "ui.h"
#include "worker.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/xam.h"

enum {
	SPLASH_MS = 2000,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	LINE_BYTES = 640,
};

static StoreConfig config = {
	.ca = "game:\\cacert.pem",
	.folder = "game:\\",
	.apps = "hdd:\\apps\\",
};
static bool shots;

static void read_config(void)
{
	FILE *const file = fopen("game:\\store.txt", "r");
	if (!file)
		return;
	char line[LINE_BYTES];
	while (fgets(line, sizeof line, file)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *const value = strchr(line, '=');
		if (!value)
			continue;
		*value = '\0';
		if (!strcmp(line, "url"))
			snprintf(config.url, sizeof config.url, "%s", value + 1);
		else if (!strcmp(line, "ca"))
			snprintf(config.ca, sizeof config.ca, "game:\\%s", value + 1);
		else if (!strcmp(line, "resolve"))
			snprintf(config.resolve, sizeof config.resolve, "%s", value + 1);
		else if (!strcmp(line, "apps")) {
			char folder[STORE_PATH_BYTES];
			snprintf(folder, sizeof folder, "game:\\%.200s", value + 1);
			mkdir(folder, 0777);
			snprintf(config.apps, sizeof config.apps, "%.250s\\", folder);
		} else if (!strcmp(line, "shots"))
			shots = value[1] == '1';
	}
	fclose(file);
	xsdk_log_write("store: store.txt: url %s, ca %s, resolve %s, apps in %s%s",
		config.url[0] ? config.url : "the kept one's", config.ca, config.resolve[0] ? config.resolve : "none",
		config.apps, shots ? ", writing each screen" : "");
}

int main(void)
{
	read_config();
	uint32_t version = 0;
	if (!store_self(&config.self_title_id, &version))
		xsdk_log_write("store: its own .xex can't be read, so its entry in the catalogue isn't known");
	char back[XSDK_LAUNCH_DATA_MAX_BYTES];
	const size_t back_bytes = xsdk_launch_data(back, sizeof back);
	xsdk_log_write("store: %s, title %08X version %u.%u.%u.%u%s", store_self_path(), (unsigned)config.self_title_id,
		(unsigned)(version >> 28), (unsigned)(version >> 24 & 0xF), (unsigned)(version >> 8 & 0xFFFF),
		(unsigned)(version & 0xFF), back_bytes ? ", started again with launch data, as an app came back" : "");
	// shots goes through the screens from a fresh start; started back by an app it launched, it would launch it again.
	if (shots && back_bytes) {
		shots = false;
		xsdk_log_write("store: started back by an app, so not going through the screens by itself");
	}
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("store: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		xsdk_log_write("store: curl_global_init failed");
		return 1;
	}
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("store: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	int result = 1;
	uint32_t width;
	uint32_t height;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("store: xsdk_present_open failed: 0x%08X", (unsigned)status);
	} else {
		if (ui_open()) {
			if (store_worker_start(&config)) {
				char xex[STORE_PATH_BYTES];
				const UiExit exit = ui_run(width, height, &config, shots, xex, sizeof xex);
				// Nothing else runs as a launch ends the store.
				store_worker_stop();
				result = 0;
				char why[STORE_WHY_BYTES];
				if (exit == UI_LAUNCH) {
					xsdk_log_write("store: launching %s", xex);
					store_launch(xex, why, sizeof why);
					xsdk_log_write("store: %s", why);
					result = 1;
				} else if (exit == UI_RELAUNCH) {
					xsdk_log_write("store: updated; starting %s again", xex);
					(void)xsdk_launch(xex, NULL, 0);
					xsdk_log_write("store: xsdk_launch(%s) failed: errno %d", xex, errno);
					result = 1;
				}
			} else {
				xsdk_log_write("store: the worker's thread didn't start");
			}
			ui_close();
		}
		xsdk_present_close();
	}
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("store: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	curl_global_cleanup();
	xsdk_log_write("store: done");
	return result;
}
