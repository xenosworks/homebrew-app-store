// The Homebrew App Store (xsdk's docs/scope/63-homebrew-store.md): the splash, then the catalogue's screens until B on
// the first. store.txt beside the .xex, for checks against a PC, may hold url=, ca= and resolve= for the catalogue's
// address, the roots TLS checks against and a host:port:address for curl, and shots=1 to write each screen and end.

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>

#include "store.h"
#include "ui.h"
#include "worker.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"

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
		else if (!strcmp(line, "shots"))
			shots = value[1] == '1';
	}
	fclose(file);
	xsdk_log_write("store: store.txt: url %s, ca %s, resolve %s%s", config.url[0] ? config.url : "the kept one's",
		config.ca, config.resolve[0] ? config.resolve : "none", shots ? ", writing each screen" : "");
}

int main(void)
{
	read_config();
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
				ui_run(width, height, shots);
				store_worker_stop();
				result = 0;
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
