// Launching an installed app, and the running title's own identity, which picks out its own entry in the catalogue
// (xsdk's docs/scope/63-homebrew-store.md, step 6).

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "store.h"
#include "xsdk/kernel.h"
#include "xsdk/xam.h"

enum {
	// Its headers come first, a few KB of them.
	XEX_HEADER_BYTES = 64 * 1024,
};

const char *store_self_path(void)
{
	return ExLoadedImageName ? ExLoadedImageName : "";
}

bool store_self(uint32_t *title_id, uint32_t *version)
{
	const char *const slash = strrchr(store_self_path(), '\\');
	char path[STORE_PATH_BYTES];
	if (!slash || snprintf(path, sizeof path, "game:\\%s", slash + 1) >= (int)sizeof path)
		return false;
	FILE *const file = fopen(path, "rb");
	if (!file)
		return false;
	uint8_t *const header = malloc(XEX_HEADER_BYTES);
	const size_t size = header ? fread(header, 1, XEX_HEADER_BYTES, file) : 0;
	fclose(file);
	const bool known = store_xex_identity(header, size, title_id, version);
	free(header);
	return known;
}

bool store_launch(const char *xex, char *why, size_t why_bytes)
{
	struct stat status;
	if (stat(xex, &status) != 0) {
		snprintf(why, why_bytes, "%s isn't there to start", xex);
		return false;
	}
	const char *const self = store_self_path();
	(void)xsdk_launch(xex, self, strlen(self) + 1);
	snprintf(why, why_bytes, "xsdk_launch(%s) failed: errno %d", xex, errno);
	return false;
}
