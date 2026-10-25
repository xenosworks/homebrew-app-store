// The Homebrew App Store's catalogue, screenshots and installed apps, shared by the store and store_check (xsdk's
// docs/scope/63-homebrew-store.md, step 4). The catalogue is the one xsdk's tools/store_catalogue.py writes and signs.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	STORE_URL_BYTES = 512,
	STORE_PATH_BYTES = 260,
	STORE_HOST_BYTES = 96,
	STORE_WHY_BYTES = 256,
	// An id and its 0: lower-case letters, digits and -, 32 at most, as the folder hdd:\apps\<id> is named.
	STORE_ID_BYTES = 33,
	// The screenshots' size, kept as the app's page shows them on a 720-line display.
	STORE_SHOT_WIDTH = 768,
	STORE_SHOT_HEIGHT = 432,
};

typedef struct {
	// The catalogue's address; empty for the kept catalogue's own, or the built-in one when none is kept.
	char url[STORE_URL_BYTES];
	// The roots TLS checks against.
	char ca[STORE_PATH_BYTES];
	// host:port:address for curl to resolve, as checks reach the PC under the name its certificate has; or empty.
	char resolve[STORE_HOST_BYTES];
	// Where the store keeps the catalogue and screenshots, and where apps install, each ending in a backslash.
	char folder[STORE_PATH_BYTES];
	char apps[STORE_PATH_BYTES];
} StoreConfig;

typedef enum {
	STORE_INSTALLED_NONE,
	STORE_INSTALLED_CURRENT,
	// Older than the catalogue's, an update ready; newer, copied over by hand.
	STORE_INSTALLED_OLDER,
	STORE_INSTALLED_NEWER,
} StoreInstalled;

typedef struct {
	char id[STORE_ID_BYTES];
	const char *name;
	const char *sentence;
} StoreCategory;

// The text points into the catalogue's JSON, which owns it.
typedef struct {
	char id[STORE_ID_BYTES];
	const char *name;
	const char *author;
	const char *sentence;
	const char *needs;
	const char *changes;
	const char *version;
	const char *launch;
	const char *screenshot;
	uint8_t screenshot_sha256[32];
	uint32_t title_id;
	uint32_t version_number;
	uint64_t package_bytes;
	uint64_t installed_bytes;
	size_t category;
	StoreInstalled installed;
	uint32_t installed_version;
} StoreApp;

typedef struct {
	cJSON *json;
	uint32_t serial;
	const char *url;
	StoreCategory *categories;
	size_t category_count;
	StoreApp *apps;
	size_t app_count;
} StoreCatalogue;

typedef enum {
	// Fetched, its signature checked, and kept.
	STORE_FRESH,
	// The kept one, as fetching failed or what came was refused; why says which.
	STORE_KEPT_OFFLINE,
	STORE_KEPT_REFUSED,
	// None to show: nothing came that was signed, and none was kept.
	STORE_NONE,
} StoreSource;

// net.c: fetches url into memory, up to max_bytes, over HTTPS only. Returns false with why filled when it can't.
bool store_fetch(const StoreConfig *config, const char *url, size_t max_bytes, uint8_t **data, size_t *size, char *why,
	size_t why_bytes);

// catalogue.c
// Whether signature is the built-in key's over data's SHA-256.
bool store_signed(const uint8_t *data, size_t size, const uint8_t *signature, size_t signature_size);
// Reads a signed catalogue's JSON into catalogue, every field checked; false with why filled if it's malformed.
bool store_catalogue_parse(const uint8_t *data, size_t size, StoreCatalogue *catalogue, char *why, size_t why_bytes);
void store_catalogue_free(StoreCatalogue *catalogue);
// Fetches the catalogue and its signature, checks them and keeps them in config->folder; or takes the one kept there
// when the fetch fails or what came is refused, unsigned, malformed or older than the kept one.
StoreSource store_catalogue_update(const StoreConfig *config, StoreCatalogue *catalogue, char *why, size_t why_bytes);
// Fills each app's installed state from the .xex it launches in config->apps.
void store_installed(const StoreConfig *config, StoreCatalogue *catalogue);
// The title ID and version in a .xex's execution info; false if it hasn't one.
bool store_xex_identity(const uint8_t *data, size_t size, uint32_t *title_id, uint32_t *version);
// Writes data to path through path.part, fsync and a rename over path.
bool store_write_file(const char *path, const void *first, size_t first_size, const void *second, size_t second_size);
// Reads a whole file, up to max_bytes; false if it isn't there or is bigger.
bool store_read_file(const char *path, size_t max_bytes, uint8_t **data, size_t *size);

// shots.c: the app's screenshot as STORE_SHOT_WIDTH by STORE_SHOT_HEIGHT IM_COL32 pixels, fitted inside and centred,
// transparent around it: the one kept in config->folder if it's the signed one, or else fetched, its SHA-256 checked
// against the catalogue's, decoded, scaled and kept. pixels is the caller's to free.
bool store_shot(const StoreConfig *config, const StoreCatalogue *catalogue, const StoreApp *app, uint32_t **pixels,
	char *why, size_t why_bytes);

#ifdef __cplusplus
}
#endif
