// Screenshots, fetched as an app is shown: decoded only once their SHA-256 is the one the signed catalogue gives, then
// scaled to the app page's size and kept, so the next time they come off the disk at that size.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mbedtls/sha256.h>

#include "stb_image.h"
#include "store.h"

enum {
	// store_catalogue.py's limit.
	MAX_SHOT_BYTES = 2 * 1024 * 1024,
	MAX_SHOT_WIDTH = 3840,
	MAX_SHOT_HEIGHT = 2160,
	// The kept file: "SHOT", the width and height, the source's SHA-256, then the pixels as they are in memory.
	KEPT_HEADER_BYTES = 4 + 4 + 4 + 32,
	SHOT_PIXELS = STORE_SHOT_WIDTH * STORE_SHOT_HEIGHT,
};

static void put_be32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

static void kept_header(const StoreApp *app, uint8_t *header)
{
	memcpy(header, "SHOT", 4);
	put_be32(header + 4, STORE_SHOT_WIDTH);
	put_be32(header + 8, STORE_SHOT_HEIGHT);
	memcpy(header + 12, app->screenshot_sha256, 32);
}

// The screenshot's address: its path beside the catalogue, each byte outside a URL's unreserved ones and / escaped.
static bool shot_url(const char *catalogue_url, const char *path, char *url, size_t url_bytes)
{
	const char *const slash = strrchr(catalogue_url, '/');
	size_t length = (size_t)(slash - catalogue_url) + 1;
	if (length >= url_bytes)
		return false;
	memcpy(url, catalogue_url, length);
	for (const char *c = path; *c; c++) {
		const unsigned char byte = (unsigned char)*c;
		const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
			(byte >= '0' && byte <= '9') || strchr("-._~/", byte);
		if (length + 4 > url_bytes)
			return false;
		length +=
			plain ? (size_t)snprintf(url + length, 2, "%c", byte) : (size_t)snprintf(url + length, 4, "%%%02X", byte);
	}
	url[length] = '\0';
	return true;
}

// The image scaled into the shot, its shape kept and centred: each pixel the average of the source's under it.
static void fit(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t *out)
{
	uint32_t fit_width = STORE_SHOT_WIDTH;
	uint32_t fit_height = (uint32_t)((uint64_t)height * STORE_SHOT_WIDTH / width);
	if (fit_height > STORE_SHOT_HEIGHT || fit_height == 0) {
		fit_height = STORE_SHOT_HEIGHT;
		fit_width = (uint32_t)((uint64_t)width * STORE_SHOT_HEIGHT / height);
	}
	fit_width = fit_width ? fit_width : 1;
	const uint32_t left = (STORE_SHOT_WIDTH - fit_width) / 2;
	const uint32_t top = (STORE_SHOT_HEIGHT - fit_height) / 2;
	for (uint32_t y = 0; y < fit_height; y++) {
		const uint32_t y0 = (uint32_t)((uint64_t)y * height / fit_height);
		uint32_t y1 = (uint32_t)((uint64_t)(y + 1) * height / fit_height);
		y1 = y1 > y0 ? y1 : y0 + 1;
		for (uint32_t x = 0; x < fit_width; x++) {
			const uint32_t x0 = (uint32_t)((uint64_t)x * width / fit_width);
			uint32_t x1 = (uint32_t)((uint64_t)(x + 1) * width / fit_width);
			x1 = x1 > x0 ? x1 : x0 + 1;
			uint32_t sum[4] = { 0, 0, 0, 0 };
			for (uint32_t sy = y0; sy < y1; sy++) {
				const uint8_t *pixel = rgba + ((size_t)sy * width + x0) * 4;
				for (uint32_t sx = x0; sx < x1; sx++, pixel += 4) {
					for (int channel = 0; channel < 4; channel++)
						sum[channel] += pixel[channel];
				}
			}
			const uint32_t count = (y1 - y0) * (x1 - x0);
			// IM_COL32's order, as imgui_impl_xsdk swizzles a texture of it.
			out[(size_t)(top + y) * STORE_SHOT_WIDTH + left + x] =
				(sum[3] / count) << 24 | (sum[2] / count) << 16 | (sum[1] / count) << 8 | sum[0] / count;
		}
	}
}

static bool take_kept(const char *path, const StoreApp *app, uint32_t **pixels)
{
	uint8_t *kept;
	size_t size;
	if (!store_read_file(path, KEPT_HEADER_BYTES + SHOT_PIXELS * 4, &kept, &size))
		return false;
	uint8_t header[KEPT_HEADER_BYTES];
	kept_header(app, header);
	*pixels = size == sizeof header + SHOT_PIXELS * 4 && memcmp(kept, header, sizeof header) == 0
		? malloc(SHOT_PIXELS * 4)
		: NULL;
	if (*pixels)
		memcpy(*pixels, kept + sizeof header, SHOT_PIXELS * 4);
	free(kept);
	return *pixels != NULL;
}

bool store_shot(const StoreConfig *config, const StoreCatalogue *catalogue, const StoreApp *app, uint32_t **pixels,
	char *why, size_t why_bytes)
{
	char folder[STORE_PATH_BYTES];
	char path[STORE_PATH_BYTES];
	if (snprintf(folder, sizeof folder, "%sshots", config->folder) >= (int)sizeof folder ||
		snprintf(path, sizeof path, "%s\\%s.shot", folder, app->id) >= (int)sizeof path) {
		snprintf(why, why_bytes, "%s is too long a folder to keep screenshots in", config->folder);
		return false;
	}
	if (take_kept(path, app, pixels))
		return true;

	char url[STORE_URL_BYTES];
	uint8_t *data;
	size_t size;
	if (!shot_url(catalogue->url, app->screenshot, url, sizeof url)) {
		snprintf(why, why_bytes, "%s's screenshot's address is too long", app->id);
		return false;
	}
	if (!store_fetch(config, url, MAX_SHOT_BYTES, &data, &size, why, why_bytes))
		return false;
	uint8_t hash[32];
	int width = 0;
	int height = 0;
	int channels;
	uint8_t *rgba = NULL;
	if (mbedtls_sha256(data, size, hash, 0) != 0 || memcmp(hash, app->screenshot_sha256, sizeof hash) != 0)
		snprintf(why, why_bytes, "%s isn't the screenshot the catalogue signed", url);
	else if (!stbi_info_from_memory(data, (int)size, &width, &height, &channels) || width > MAX_SHOT_WIDTH ||
		height > MAX_SHOT_HEIGHT)
		snprintf(why, why_bytes, "%s isn't a PNG or JPEG of %ux%u at most", url, MAX_SHOT_WIDTH, MAX_SHOT_HEIGHT);
	else if (!(rgba = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4)))
		snprintf(why, why_bytes, "%s doesn't decode: %s", url, stbi_failure_reason());
	free(data);
	if (!rgba)
		return false;
	uint32_t *const out = calloc(SHOT_PIXELS, 4);
	if (out)
		fit(rgba, (uint32_t)width, (uint32_t)height, out);
	stbi_image_free(rgba);
	if (!out) {
		snprintf(why, why_bytes, "no memory for %s's screenshot", app->id);
		return false;
	}
	uint8_t header[KEPT_HEADER_BYTES];
	kept_header(app, header);
	// Shown either way; not kept, it's fetched again next time.
	mkdir(folder, 0777);
	(void)store_write_file(path, header, sizeof header, out, SHOT_PIXELS * 4);
	*pixels = out;
	return true;
}
