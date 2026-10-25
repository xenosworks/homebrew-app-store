// The store's checks on the console (xsdk's docs/scope/63-homebrew-store.md, step 4 and its Checks), against the test
// site tests/make_site.py writes and xsdk's tools/net_http.py --site serves. net_http.txt beside the title holds
// "<PC address> <HTTP port>", HTTPS on the next port, and the PC's certificate is for localhost, under test-ca2.crt.
// Logs PASS or FAIL for each check, then "tests: <passed> passed, <failed> failed".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "store.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/xam.h"

enum {
	SPLASH_MS = 2000,
	// Nothing listens there on the console.
	CLOSED_PORT = 7379,
	// tests/make_site.py's serials.
	GOOD_SERIAL = 5,
	MOVED_SERIAL = 6,
	MOVED_TO_SERIAL = 7,
	// tests/make_site.py's screenshot of alpha: white on its left half and red on its right, 16:9.
	WHITE_PIXEL = 0xFFFFFFFF,
	RED_PIXEL = 0xFF0000FF,
	// The newer store_check's version, CMakeLists.txt's store_check_v2.
	NEWER_VERSION = 2,
	// A launch back comes within seconds.
	RETURN_WINDOW_S = 60,
	MAX_LINES = 40,
	LINE_BYTES = 192,
};

static const char HOST_FILE[] = "game:\\net_http.txt";
static const char FOLDER[] = "game:\\check";
static const char STATE[] = "game:\\check\\state.txt";

// Step 6's starts: the first, which launches launch_target; the one it launches back; the one after the update.
enum { STAGE_FRESH, STAGE_LAUNCHED, STAGE_UPDATED };

static StoreConfig config = {
	.ca = "game:\\test-ca2.crt",
	.folder = "game:\\check\\",
	.apps = "game:\\check\\apps\\",
};
static char site[STORE_URL_BYTES];
static unsigned passed;
static unsigned failed;

static char lines[MAX_LINES][LINE_BYTES];
static unsigned line_count;

static void check(const char *name, bool good)
{
	xsdk_log_write("%s: %s", good ? "PASS" : "FAIL", name);
	if (good)
		passed++;
	else
		failed++;
	if (line_count < MAX_LINES)
		snprintf(lines[line_count++], LINE_BYTES, "%s: %s", good ? "PASS" : "FAIL", name);
}

// A launch ends the title, so the stage, when it was written and the checks so far go in STATE, and the next start
// logs them again. One older than a launch back takes is from a run that never came back, as is one saying launched
// when no launch data came: launch_target always sends it.
static int read_state(size_t back_bytes)
{
	FILE *const file = fopen(STATE, "r");
	if (!file)
		return STAGE_FRESH;
	int stage = STAGE_FRESH;
	long long written = 0;
	char line[LINE_BYTES];
	const bool read = fscanf(file, "%d %lld\n", &stage, &written) == 2;
	const long long age_s = (long long)time(NULL) - written;
	const bool fresh = read && age_s >= 0 && age_s <= RETURN_WINDOW_S && (stage != STAGE_LAUNCHED || back_bytes != 0);
	while (fresh && fgets(line, sizeof line, file)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strlen(line) > 6)
			check(line + 6, strncmp(line, "PASS: ", 6) == 0);
	}
	fclose(file);
	remove(STATE);
	if (!fresh)
		xsdk_log_write("store check: %s is from a run %lld s ago that never came back; starting afresh", STATE, age_s);
	return fresh ? stage : STAGE_FRESH;
}

static bool write_state(int stage)
{
	FILE *const file = fopen(STATE, "w");
	if (!file)
		return false;
	fprintf(file, "%d %lld\n", stage, (long long)time(NULL));
	for (unsigned i = 0; i < line_count; i++)
		fprintf(file, "%s\n", lines[i]);
	return fclose(file) == 0;
}

static bool read_host(void)
{
	char pc[16];
	unsigned port;
	FILE *const file = fopen(HOST_FILE, "r");
	const int fields = file ? fscanf(file, "%15s %u", pc, &port) : 0;
	if (file)
		fclose(file);
	if (fields != 2) {
		xsdk_log_write("store check: %s must hold \"<PC address> <HTTP port>\"", HOST_FILE);
		return false;
	}
	snprintf(config.resolve, sizeof config.resolve, "localhost:%u:%s", port + 1, pc);
	snprintf(site, sizeof site, "https://localhost:%u/site/", port + 1);
	return true;
}

// Updates from the site's variant, or from the kept catalogue's own address if variant is NULL.
static StoreSource update(const char *variant, StoreCatalogue *catalogue)
{
	config.url[0] = '\0';
	if (variant && snprintf(config.url, sizeof config.url, "%s%s/catalogue.json", site, variant) >= STORE_URL_BYTES)
		return STORE_NONE;
	char why[STORE_WHY_BYTES];
	const StoreSource source = store_catalogue_update(&config, catalogue, why, sizeof why);
	xsdk_log_write("store check: %s: source %d, serial %u: %s", variant ? variant : "the kept address", (int)source,
		source == STORE_NONE ? 0u : (unsigned)catalogue->serial, why);
	return source;
}

// Everything kept, a run broken on purpose's too: a screenshot such a run kept unchecked would pass for checked.
static void forget_kept(void)
{
	remove("game:\\check\\catalogue.kept");
	remove("game:\\check\\shots\\alpha.shot");
	remove("game:\\check\\shots\\beta.shot");
}

static void check_catalogue(void)
{
	StoreCatalogue catalogue;
	StoreSource source = update("good", &catalogue);
	check("catalogue: a signed one fetched is taken and kept",
		source == STORE_FRESH && catalogue.serial == GOOD_SERIAL && catalogue.app_count == 2 &&
			catalogue.category_count == 2);
	store_catalogue_free(&catalogue);

	source = update("byte", &catalogue);
	check("catalogue: one with a byte changed is refused, and the kept one shown",
		source == STORE_KEPT_REFUSED && catalogue.serial == GOOD_SERIAL);
	store_catalogue_free(&catalogue);

	source = update("otherkey", &catalogue);
	check("catalogue: one signed with another key is refused",
		source == STORE_KEPT_REFUSED && catalogue.serial == GOOD_SERIAL);
	store_catalogue_free(&catalogue);

	source = update("old", &catalogue);
	check("catalogue: a signed one older than the kept one is refused",
		source == STORE_KEPT_REFUSED && catalogue.serial == GOOD_SERIAL);
	store_catalogue_free(&catalogue);

	const StoreConfig online = config;
	snprintf(config.url, sizeof config.url, "https://localhost:%u/site/good/catalogue.json", (unsigned)CLOSED_PORT);
	char why[STORE_WHY_BYTES];
	source = store_catalogue_update(&config, &catalogue, why, sizeof why);
	xsdk_log_write("store check: a closed port: source %d: %s", (int)source, why);
	check("catalogue: with nothing at its address, the kept one is shown",
		source == STORE_KEPT_OFFLINE && catalogue.serial == GOOD_SERIAL);
	store_catalogue_free(&catalogue);
	forget_kept();
	source = store_catalogue_update(&config, &catalogue, why, sizeof why);
	check("catalogue: with nothing at its address and none kept, there's none", source == STORE_NONE);
	config = online;

	update("moved", &catalogue);
	store_catalogue_free(&catalogue);
	source = update(NULL, &catalogue);
	check("catalogue: a signed new address is where the next one comes from",
		source == STORE_FRESH && catalogue.serial == MOVED_TO_SERIAL);
	store_catalogue_free(&catalogue);
	forget_kept();
}

static void check_shots(void)
{
	StoreCatalogue catalogue;
	if (update("good", &catalogue) != STORE_FRESH) {
		check("shots: the catalogue to show them from", false);
		return;
	}
	char why[STORE_WHY_BYTES] = "";
	uint32_t *pixels = NULL;
	const StoreApp *const alpha = &catalogue.apps[0];
	bool got = store_shot(&config, &catalogue, alpha, &pixels, why, sizeof why);
	xsdk_log_write("store check: alpha's screenshot: %s%s", got ? "had" : "not had: ", why);
	const uint32_t row = STORE_SHOT_HEIGHT / 2 * STORE_SHOT_WIDTH;
	check("shots: one fetched is scaled to the page's size, white on its left and red on its right",
		got && pixels[row + 100] == WHITE_PIXEL && pixels[row + STORE_SHOT_WIDTH - 100] == RED_PIXEL);
	free(pixels);
	pixels = NULL;

	// The site's name sent to the console itself, where nothing listens, so only what's kept can come.
	const StoreConfig online = config;
	const char *const address = strrchr(online.resolve, ':');
	got = snprintf(config.resolve, sizeof config.resolve, "%.*s127.0.0.1", (int)(address - online.resolve + 1),
			  online.resolve) < (int)sizeof config.resolve &&
		store_shot(&config, &catalogue, alpha, &pixels, why, sizeof why);
	check("shots: the kept one comes back with the site out of reach",
		got && pixels[row + 100] == WHITE_PIXEL && pixels[row + STORE_SHOT_WIDTH - 100] == RED_PIXEL);
	free(pixels);
	pixels = NULL;
	config = online;

	got = store_shot(&config, &catalogue, &catalogue.apps[1], &pixels, why, sizeof why);
	xsdk_log_write("store check: beta's screenshot: %s%s", got ? "had" : "not had: ", why);
	check("shots: one that isn't the signed one is refused", !got && strstr(why, "isn't the screenshot"));
	free(pixels);
	store_catalogue_free(&catalogue);
	forget_kept();
}

// A .xex's start as far as its execution info, which is all store_installed reads.
static bool write_xex(const char *app, const char *launch, uint32_t title_id, uint32_t version)
{
	char path[STORE_PATH_BYTES];
	if (snprintf(path, sizeof path, "%s%s", config.apps, app) >= (int)sizeof path)
		return false;
	mkdir(path, 0777);
	if (snprintf(path, sizeof path, "%s%s\\%s", config.apps, app, launch) >= (int)sizeof path)
		return false;
	uint8_t xex[0x38] = { 'X', 'E', 'X', '2' };
	const uint32_t words[][2] = { { 0x14, 1 }, { 0x18, 0x00040006 }, { 0x1C, 0x20 }, { 0x24, version },
		{ 0x2C, title_id } };
	for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
		xex[words[i][0]] = (uint8_t)(words[i][1] >> 24);
		xex[words[i][0] + 1] = (uint8_t)(words[i][1] >> 16);
		xex[words[i][0] + 2] = (uint8_t)(words[i][1] >> 8);
		xex[words[i][0] + 3] = (uint8_t)words[i][1];
	}
	return store_write_file(path, xex, sizeof xex, NULL, 0);
}

static void check_installed(void)
{
	StoreCatalogue catalogue;
	if (update("good", &catalogue) != STORE_FRESH) {
		check("installed: the catalogue to compare with", false);
		return;
	}
	const StoreApp *const alpha = &catalogue.apps[0];
	const StoreApp *const beta = &catalogue.apps[1];
	mkdir("game:\\check\\apps", 0777);
	const bool written = write_xex(alpha->id, alpha->launch, alpha->title_id, alpha->version_number) &&
		write_xex(beta->id, beta->launch, beta->title_id, beta->version_number - 1);
	store_installed(&config, &catalogue);
	check("installed: an app's .xex at the catalogue's version is installed, an older one's has an update ready",
		written && alpha->installed == STORE_INSTALLED_CURRENT && beta->installed == STORE_INSTALLED_OLDER);
	write_xex(beta->id, beta->launch, beta->title_id ^ 1, beta->version_number);
	store_installed(&config, &catalogue);
	check("installed: a .xex with another title ID isn't the app", beta->installed == STORE_INSTALLED_NONE);
	store_catalogue_free(&catalogue);
}

static bool exists(const char *path)
{
	struct stat status;
	return stat(path, &status) == 0;
}

static bool write_text(const char *path, const char *text)
{
	FILE *const file = fopen(path, "wb");
	const bool written = file && fputs(text, file) >= 0;
	return file && fclose(file) == 0 && written;
}

// The app's installed state afresh, its version as installed.
static StoreInstalled installed(StoreApp *app)
{
	store_installed_app(&config, app);
	return app->installed;
}

static bool install(StoreCatalogue *catalogue, size_t index, StoreProgress *progress)
{
	char why[STORE_WHY_BYTES] = "";
	memset(progress, 0, sizeof *progress);
	const bool done = store_install(&config, catalogue, &catalogue->apps[index], progress, why, sizeof why);
	xsdk_log_write("store check: %s %s installs: %s%s", catalogue->apps[index].id, catalogue->apps[index].version,
		done ? "done" : "failed: ", why);
	return done;
}

// Step 5: install, update and remove alpha, a cut download of beta's, and cut installs recovered.
static void check_install(void)
{
	StoreCatalogue catalogue;
	StoreProgress progress;
	char why[STORE_WHY_BYTES];
	StoreApp gone = { .id = "alpha" };
	store_remove(&config, &gone, true, why, sizeof why);
	snprintf(gone.id, sizeof gone.id, "beta");
	store_remove(&config, &gone, true, why, sizeof why);

	if (update("good", &catalogue) != STORE_FRESH) {
		check("install: the catalogue to install from", false);
		return;
	}
	bool done = install(&catalogue, 0, &progress);
	check("install: an app installs its files and app.json, and nothing else",
		done && exists("game:\\check\\apps\\alpha\\alpha.xex") &&
			exists("game:\\check\\apps\\alpha\\data\\readme.txt") && exists("game:\\check\\apps\\alpha\\app.json") &&
			store_other_files(&config, &catalogue.apps[0]) == 0 &&
			installed(&catalogue.apps[0]) == STORE_INSTALLED_CURRENT && catalogue.apps[0].receipt);
	store_catalogue_free(&catalogue);
	// What the app makes itself.
	write_text("game:\\check\\apps\\alpha\\save.dat", "a save\n");

	update("badupdate", &catalogue);
	char why_install[STORE_WHY_BYTES] = "";
	memset(&progress, 0, sizeof progress);
	done = store_install(&config, &catalogue, &catalogue.apps[0], &progress, why_install, sizeof why_install);
	xsdk_log_write(
		"store check: alpha %s installs: %s%s", catalogue.apps[0].version, done ? "done" : "failed: ", why_install);
	check("install: a package that isn't the signed one is refused, and the version installed stays",
		!done && strstr(why_install, "isn't the package") && installed(&catalogue.apps[0]) == STORE_INSTALLED_OLDER &&
			exists("game:\\check\\apps\\alpha\\data\\readme.txt") && exists("game:\\check\\apps\\alpha\\app.json") &&
			!exists("game:\\check\\apps\\alpha\\alpha.xex.part") &&
			!exists("game:\\check\\apps\\alpha\\app.json.pending"));
	store_catalogue_free(&catalogue);

	update("update", &catalogue);
	done = install(&catalogue, 0, &progress);
	check("update: replaces the app's files, deletes the one it dropped, and keeps what the app made",
		done && installed(&catalogue.apps[0]) == STORE_INSTALLED_CURRENT &&
			exists("game:\\check\\apps\\alpha\\data\\new.txt") &&
			!exists("game:\\check\\apps\\alpha\\data\\readme.txt") && exists("game:\\check\\apps\\alpha\\save.dat") &&
			store_other_files(&config, &catalogue.apps[0]) == 1);

	done = store_remove(&config, &catalogue.apps[0], false, why, sizeof why);
	check("remove: deletes app.json's files and app.json, and leaves what the app made",
		done && !exists("game:\\check\\apps\\alpha\\alpha.xex") && !exists("game:\\check\\apps\\alpha\\data") &&
			!exists("game:\\check\\apps\\alpha\\app.json") && exists("game:\\check\\apps\\alpha\\save.dat") &&
			store_other_files(&config, &catalogue.apps[0]) == 1);
	done = store_remove(&config, &catalogue.apps[0], true, why, sizeof why);
	check("remove: with everything, the app's folder goes too", done && !exists("game:\\check\\apps\\alpha"));
	store_catalogue_free(&catalogue);

	// badfile's, drop's and good's serial is below update's, so what's kept goes first.
	forget_kept();
	update("badfile", &catalogue);
	memset(&progress, 0, sizeof progress);
	done = store_install(&config, &catalogue, &catalogue.apps[1], &progress, why_install, sizeof why_install);
	xsdk_log_write("store check: beta from badfile installs: %s%s", done ? "done" : "failed: ", why_install);
	check("install: a file that isn't the one the catalogue signed is refused, and nothing of it is left",
		!done && strstr(why_install, "isn't the file") && !exists("game:\\check\\apps\\beta\\app.json") &&
			!exists("game:\\check\\apps\\beta\\beta.xex") && !exists("game:\\check\\apps\\beta\\beta.xex.part") &&
			!exists("game:\\check\\apps\\beta\\data\\readme.txt.part") &&
			!exists("game:\\check\\apps\\beta\\app.json.pending"));
	store_catalogue_free(&catalogue);
	forget_kept();

	update("drop", &catalogue);
	char part[STORE_PATH_BYTES];
	store_download_path(&config, &catalogue.apps[1], part, sizeof part);
	remove(part);
	const uint64_t package_bytes = catalogue.apps[1].package_bytes;
	const bool cut = !install(&catalogue, 1, &progress) && exists(part);
	store_catalogue_free(&catalogue);
	update("good", &catalogue);
	done = install(&catalogue, 1, &progress);
	xsdk_log_write("store check: beta's package, %u bytes: %u fetched after the cut", (unsigned)package_bytes,
		(unsigned)progress.fetched_bytes);
	check("download: a cut one is taken up where it stopped",
		cut && done && progress.fetched_bytes > 0 && progress.fetched_bytes < package_bytes && !exists(part) &&
			installed(&catalogue.apps[1]) == STORE_INSTALLED_CURRENT);

	// A cut before the files were checked: their .part files are taken back.
	StoreApp *const beta = &catalogue.apps[1];
	mkdir("game:\\check\\apps\\beta\\data", 0777);
	const bool laid = write_text("game:\\check\\apps\\beta\\app.json.pending",
						  "{\"files\": [{\"path\": \"beta.xex\"}, {\"path\": \"data/extra.txt\"}]}") &&
		write_text("game:\\check\\apps\\beta\\beta.xex.part", "not checked\n") &&
		write_text("game:\\check\\apps\\beta\\data\\extra.txt.part", "not checked\n");
	store_recover(&config);
	check("recover: an install cut before its files were checked is taken back, the installed one left",
		laid && !exists("game:\\check\\apps\\beta\\beta.xex.part") &&
			!exists("game:\\check\\apps\\beta\\data\\extra.txt.part") &&
			!exists("game:\\check\\apps\\beta\\app.json.pending") && installed(beta) == STORE_INSTALLED_CURRENT);

	// A cut after: the checked .part files go into place, and app.json.part becomes app.json.
	const bool checked = write_text("game:\\check\\apps\\beta\\app.json.part",
							 "{\"author\": \"Someone Else\", \"files\": [{\"path\": \"beta.xex\"}]}") &&
		write_xex("beta", "beta.xex.part", beta->title_id, beta->version_number + 1);
	store_recover(&config);
	check("recover: an install cut after its files were checked is finished",
		checked && !exists("game:\\check\\apps\\beta\\beta.xex.part") &&
			!exists("game:\\check\\apps\\beta\\app.json.part") && installed(beta) == STORE_INSTALLED_NEWER &&
			beta->receipt);
	store_remove(&config, beta, true, why, sizeof why);
	store_catalogue_free(&catalogue);
	forget_kept();
}

// Step 6: xsdk's launch_target, installed and started with store_check's path, launches it back with that data
// reversed (xsdk's tests/launch_target.c). Returns only if the launch fails.
static void launch_out(void)
{
	StoreCatalogue catalogue;
	StoreProgress progress;
	char why[STORE_WHY_BYTES];
	char xex[STORE_PATH_BYTES];
	forget_kept();
	if (update("launch", &catalogue) != STORE_FRESH) {
		check("launch: the catalogue holding launch_target", false);
		return;
	}
	const bool ready = install(&catalogue, 0, &progress) && store_app_xex(&config, &catalogue.apps[0], xex, sizeof xex);
	store_catalogue_free(&catalogue);
	if (!ready || !write_state(STAGE_LAUNCHED)) {
		check("launch: launch_target installed, to start", false);
		return;
	}
	xsdk_log_write("store check: launching %s with its own path, %s", xex, store_self_path());
	store_launch(xex, why, sizeof why);
	remove(STATE);
	xsdk_log_write("store check: %s", why);
	check("launch: launch_target, installed and started with store_check's path, came back with it", false);
}

static void launch_back(const uint8_t *back, size_t back_bytes)
{
	const char *const self = store_self_path();
	const size_t length = strlen(self);
	bool reversed = back_bytes == length + 1 && back[0] == '\0';
	for (size_t i = 0; reversed && i < length; i++)
		reversed = back[1 + i] == (uint8_t)self[length - 1 - i];
	xsdk_log_write("store check: started again with %u bytes of launch data", (unsigned)back_bytes);
	check("launch: launch_target, installed and started with store_check's path, came back with it", reversed);
}

// store_check updating itself as the store does: its own entry, newer, installed over the .xex it runs from, then
// started again. Returns only if that fails.
static void update_self(void)
{
	StoreCatalogue catalogue;
	StoreProgress progress;
	if (update("selfupdate", &catalogue) != STORE_FRESH) {
		check("update: the catalogue holding store_check, newer", false);
		return;
	}
	StoreApp *const self = &catalogue.apps[0];
	store_installed_app(&config, self);
	xsdk_log_write("store check: %s is itself %d, installed %d", self->id, (int)self->self, (int)self->installed);
	const bool done = self->self && self->installed == STORE_INSTALLED_OLDER && install(&catalogue, 0, &progress);
	store_catalogue_free(&catalogue);
	if (!done || !write_state(STAGE_UPDATED)) {
		check("update: store_check's own entry, newer, installs over the .xex it runs from", false);
		return;
	}
	(void)xsdk_launch(store_self_path(), NULL, 0);
	remove(STATE);
	check("update: store_check, updated, starts again", false);
}

static void updated(void)
{
	uint32_t title_id = 0;
	uint32_t version = 0;
	const bool known = store_self(&title_id, &version);
	check("update: store_check updated itself and started again, the .xex it runs from now the newer one",
		known && version == NEWER_VERSION && exists("game:\\app.json") && !exists("game:\\store_check.xex.part"));
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	uint32_t version = 0;
	(void)store_self(&config.self_title_id, &version);
	uint8_t back[XSDK_LAUNCH_DATA_MAX_BYTES];
	const size_t back_bytes = xsdk_launch_data(back, sizeof back);
	xsdk_log_write(
		"store check: %s, title %08X version %u", curl_version(), (unsigned)config.self_title_id, (unsigned)version);
	if (!read_host() || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		return 1;
	mkdir(FOLDER, 0777);
	// Everything else runs before the launch, as the update brings in store_check built as it should be: a run broken
	// on purpose has to break before it.
	const int stage = read_state(back_bytes);
	if (stage == STAGE_FRESH) {
		forget_kept();
		check_catalogue();
		check_shots();
		check_installed();
		check_install();
		launch_out();
	}
	if (stage == STAGE_LAUNCHED)
		launch_back(back, back_bytes);
	if (stage != STAGE_UPDATED)
		update_self();
	if (stage == STAGE_UPDATED)
		updated();
	curl_global_cleanup();
	xsdk_log_write("tests: %u passed, %u failed", passed, failed);
	return failed != 0;
}
