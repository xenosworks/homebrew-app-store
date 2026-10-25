// The store's checks on the console (xsdk's docs/scope/63-homebrew-store.md, step 4 and its Checks), against the test
// site tests/make_site.py writes and xsdk's tools/net_http.py --site serves. net_http.txt beside the title holds
// "<PC address> <HTTP port>", HTTPS on the next port, and the PC's certificate is for localhost, under test-ca2.crt.
// Logs PASS or FAIL for each check, then "tests: <passed> passed, <failed> failed".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include "store.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

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
};

static const char HOST_FILE[] = "game:\\net_http.txt";
static const char FOLDER[] = "game:\\check";

static StoreConfig config = {
	.ca = "game:\\test-ca2.crt",
	.folder = "game:\\check\\",
	.apps = "game:\\check\\apps\\",
};
static char site[STORE_URL_BYTES];
static unsigned passed;
static unsigned failed;

static void check(const char *name, bool good)
{
	xsdk_log_write("%s: %s", good ? "PASS" : "FAIL", name);
	if (good)
		passed++;
	else
		failed++;
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

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("store check: %s", curl_version());
	if (!read_host() || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		return 1;
	mkdir(FOLDER, 0777);
	forget_kept();
	check_catalogue();
	check_shots();
	check_installed();
	curl_global_cleanup();
	xsdk_log_write("tests: %u passed, %u failed", passed, failed);
	return failed != 0;
}
