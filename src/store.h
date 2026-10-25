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
	// The running title's ID, from its own .xex: the catalogue's entry with it is the running title itself, which
	// installs in its own folder, game:, and updates by replacing its own .xex.
	uint32_t self_title_id;
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
	const char *package_url;
	uint8_t package_sha256[32];
	uint64_t package_bytes;
	uint64_t installed_bytes;
	// The entry's files, each {"path", "size", "sha256"}, every path checked.
	const cJSON *files;
	size_t category;
	StoreInstalled installed;
	uint32_t installed_version;
	// Whether app.json is in the app's folder, the store having installed it, and whether it names the same author.
	bool receipt;
	bool receipt_author_same;
	// The running title itself.
	bool self;
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

// What an install or a removal has done so far, for the screens to show as it runs.
typedef enum {
	STORE_STEP_NONE,
	STORE_STEP_DOWNLOADING,
	STORE_STEP_INSTALLING,
	STORE_STEP_REMOVING,
} StoreStep;

typedef struct {
	volatile uint32_t step;
	volatile uint32_t done_bytes;
	volatile uint32_t total_bytes;
	// What the download fetched this time, less than the package when it took up a cut one.
	volatile uint32_t fetched_bytes;
} StoreProgress;

// net.c: fetches url into memory, up to max_bytes, over HTTPS only. Returns false with why filled when it can't.
bool store_fetch(const StoreConfig *config, const char *url, size_t max_bytes, uint8_t **data, size_t *size, char *why,
	size_t why_bytes);
// Downloads url's size bytes into memory, taking up from where a cut download left part_path, and writes each piece to
// part_path from a thread of its own as it comes, as writing to the disk as it arrives halves the rate (xsdk's
// docs/HARDWARE.md, "Milestone 56 on the console"). data is the caller's once all size bytes have come.
bool store_download(const StoreConfig *config, const char *url, const char *part_path, size_t size, uint8_t **data,
	StoreProgress *progress, char *why, size_t why_bytes);

// catalogue.c
// Whether signature is the built-in key's over data's SHA-256.
bool store_signed(const uint8_t *data, size_t size, const uint8_t *signature, size_t signature_size);
// Reads a signed catalogue's JSON into catalogue, every field checked; false with why filled if it's malformed.
bool store_catalogue_parse(const uint8_t *data, size_t size, StoreCatalogue *catalogue, char *why, size_t why_bytes);
void store_catalogue_free(StoreCatalogue *catalogue);
// Fetches the catalogue and its signature, checks them and keeps them in config->folder; or takes the one kept there
// when the fetch fails or what came is refused, unsigned, malformed or older than the kept one.
StoreSource store_catalogue_update(const StoreConfig *config, StoreCatalogue *catalogue, char *why, size_t why_bytes);
// Fills each app's installed state from the .xex it launches in config->apps, and its receipt from app.json there.
void store_installed(const StoreConfig *config, StoreCatalogue *catalogue);
void store_installed_app(const StoreConfig *config, StoreApp *app);
// Whether path is one inside an app's folder, with / between its names, each a name FATX takes with .part after it.
bool store_path_ok(const char *path);
// The folder an app installs in, config->apps and its id or, for the running title itself, game:; and its .xex there.
bool store_app_folder(const StoreConfig *config, const StoreApp *app, char *folder, size_t folder_bytes);
bool store_app_xex(const StoreConfig *config, const StoreApp *app, char *path, size_t path_bytes);
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

// install.c (xsdk's docs/scope/63-homebrew-store.md, step 5). An app's folder is config->apps followed by its id.
// Installs or updates the app: its package downloaded and its SHA-256 checked; app.json.pending written, naming the
// files; each file unpacked beside its final name as <name>.part and checked against the catalogue; app.json.pending
// renamed app.json.part; the files renamed into place and app.json.part renamed app.json. Then the files the version
// before listed and this one doesn't are deleted. False with why filled leaves the installed version as it was.
bool store_install(const StoreConfig *config, const StoreCatalogue *catalogue, const StoreApp *app,
	StoreProgress *progress, char *why, size_t why_bytes);
// Deletes the files app.json lists, then app.json; with everything, the rest of the app's folder too, what the app
// made.
bool store_remove(const StoreConfig *config, const StoreApp *app, bool everything, char *why, size_t why_bytes);
// How many files are in the app's folder beyond what app.json lists.
size_t store_other_files(const StoreConfig *config, const StoreApp *app);
// Where the app's package downloads, which a cut download leaves for the next try to take up.
bool store_download_path(const StoreConfig *config, const StoreApp *app, char *path, size_t path_bytes);
// As the store starts: in each app's folder, finishes an install app.json.part says was checked, and takes back the
// .part files of one app.json.pending says wasn't.
void store_recover(const StoreConfig *config);

// launch.c (step 6).
// The running title's ID and version, from its own .xex beside it; false if that can't be read.
bool store_self(uint32_t *title_id, uint32_t *version);
// The path the loader started the running title from, as xsdk_launch takes it back.
const char *store_self_path(void);
// Starts an installed app's .xex, from store_app_xex, with the store's own path, ended by its 0, as its launch data,
// so the app can come back with xsdk_launch_data() and xsdk_launch(); ends the store, and returns only if it can't,
// why filled. Nothing else may be running then: the store's worker stopped first.
bool store_launch(const char *xex, char *why, size_t why_bytes);

#ifdef __cplusplus
}
#endif
