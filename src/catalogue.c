// The catalogue: fetched with its signature, checked against the key built in, kept on the hard disk with its
// signature, and read into the store's structs with every field checked again, as an id names a folder on the disk.
// The format is xsdk's tools/store_catalogue.py's.

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "key.h"
#include "store.h"

enum {
	FORMAT = 1,
	MAX_CATALOGUE_BYTES = 4 * 1024 * 1024,
	// A DER P-256 signature is 72 bytes at most.
	MAX_SIGNATURE_BYTES = 80,
	MAX_CATEGORIES = 64,
	MAX_APPS = 1024,
	// A name in an app's folder, so that name.part still fits FATX's 42 (xsdk's docs/HARDWARE.md, "Shaders compiled
	// as the game asks").
	NAME_BYTES = 37,
	MAX_PATH_CHARS = 128,
	// How much of a .xex is read for its execution info: its headers come first, a few KB of them.
	XEX_HEADER_BYTES = 64 * 1024,
	// xenia:src/xenia/kernel/util/xex2_info.h:278-308, as xsdk's tools/elf2xex.py writes it.
	HEADER_EXECUTION_INFO = 0x00040006,
	MAX_RECEIPT_BYTES = 1024 * 1024,
};

// shortcut: a package is read into memory whole, so it's held to what the console's memory takes; unpack it from the
// file if apps that big ever come.
static const double MAX_PACKAGE_BYTES = 192.0 * 1024 * 1024;

static const char KEPT_NAME[] = "catalogue.kept";

static uint32_t be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

bool store_signed(const uint8_t *data, size_t size, const uint8_t *signature, size_t signature_size)
{
	static const char key_pem[] = STORE_PUBLIC_KEY_PEM;
	uint8_t hash[32];
	mbedtls_pk_context key;
	mbedtls_pk_init(&key);
	const bool good = mbedtls_sha256(data, size, hash, 0) == 0 &&
		mbedtls_pk_parse_public_key(&key, (const unsigned char *)key_pem, sizeof key_pem) == 0 &&
		mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) &&
		mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof hash, signature, signature_size) == 0;
	mbedtls_pk_free(&key);
	return good;
}

bool store_read_file(const char *path, size_t max_bytes, uint8_t **data, size_t *size)
{
	FILE *const file = fopen(path, "rb");
	if (!file)
		return false;
	long length = -1;
	if (fseek(file, 0, SEEK_END) == 0)
		length = ftell(file);
	uint8_t *const bytes = length >= 0 && (size_t)length <= max_bytes ? malloc(length ? (size_t)length : 1) : NULL;
	const bool read = bytes && fseek(file, 0, SEEK_SET) == 0 && fread(bytes, 1, (size_t)length, file) == (size_t)length;
	fclose(file);
	if (!read) {
		free(bytes);
		return false;
	}
	*data = bytes;
	*size = (size_t)length;
	return true;
}

bool store_write_file(const char *path, const void *first, size_t first_size, const void *second, size_t second_size)
{
	char part[STORE_PATH_BYTES + 8];
	if (snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part)
		return false;
	FILE *const file = fopen(part, "wb");
	if (!file)
		return false;
	bool written = fwrite(first, 1, first_size, file) == first_size &&
		(!second_size || fwrite(second, 1, second_size, file) == second_size) && fflush(file) == 0 &&
		fsync(fileno(file)) == 0;
	written = fclose(file) == 0 && written;
	// rename replaces what's at path (xsdk's docs/HARDWARE.md, "Files renamed on the console").
	if (!written || rename(part, path) != 0) {
		remove(part);
		return false;
	}
	return true;
}

bool store_xex_identity(const uint8_t *data, size_t size, uint32_t *title_id, uint32_t *version)
{
	if (size < 0x18 || memcmp(data, "XEX2", 4) != 0)
		return false;
	const uint32_t count = be32(data + 0x14);
	for (uint32_t index = 0; index < count && 0x18 + 8 * (size_t)index + 8 <= size; index++) {
		const uint8_t *const entry = data + 0x18 + 8 * (size_t)index;
		if (be32(entry) != HEADER_EXECUTION_INFO)
			continue;
		const uint32_t offset = be32(entry + 4);
		if ((size_t)offset + 16 > size)
			return false;
		*version = be32(data + offset + 4);
		*title_id = be32(data + offset + 12);
		return true;
	}
	return false;
}

static bool refuse(StoreCatalogue *catalogue, char *why, size_t why_bytes, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(why, why_bytes, format, arguments);
	va_end(arguments);
	store_catalogue_free(catalogue);
	return false;
}

void store_catalogue_free(StoreCatalogue *catalogue)
{
	cJSON_Delete(catalogue->json);
	free(catalogue->categories);
	free(catalogue->apps);
	memset(catalogue, 0, sizeof *catalogue);
}

static const char *text(const cJSON *object, const char *name)
{
	const cJSON *const item = cJSON_GetObjectItemCaseSensitive(object, name);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

// A whole number from 0 to max.
static bool whole(const cJSON *object, const char *name, double max, uint64_t *value)
{
	const cJSON *const item = cJSON_GetObjectItemCaseSensitive(object, name);
	if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max ||
		item->valuedouble != (double)(uint64_t)item->valuedouble)
		return false;
	*value = (uint64_t)item->valuedouble;
	return true;
}

static bool id_ok(const char *id)
{
	const size_t length = id ? strlen(id) : 0;
	if (length == 0 || length >= STORE_ID_BYTES || id[0] == '-')
		return false;
	for (size_t i = 0; i < length; i++) {
		if (!(islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '-'))
			return false;
	}
	return true;
}

// As store_catalogue.py checks them.
bool store_path_ok(const char *path)
{
	if (!path || !path[0] || strlen(path) > MAX_PATH_CHARS)
		return false;
	for (const char *name = path; name;) {
		const char *const slash = strchr(name, '/');
		const size_t length = slash ? (size_t)(slash - name) : strlen(name);
		if (length == 0 || length > NAME_BYTES || (length == 1 && name[0] == '.') ||
			(length == 2 && name[0] == '.' && name[1] == '.'))
			return false;
		for (size_t i = 0; i < length; i++) {
			const unsigned char c = (unsigned char)name[i];
			if (c < 0x20 || c > 0x7E || strchr("\\:*?\"<>|", c))
				return false;
		}
		name = slash ? slash + 1 : NULL;
	}
	return true;
}

static bool hex_bytes(const char *hex, uint8_t *bytes, size_t count)
{
	if (!hex || strlen(hex) != count * 2)
		return false;
	for (size_t i = 0; i < count * 2; i++) {
		if (!isxdigit((unsigned char)hex[i]))
			return false;
	}
	for (size_t i = 0; i < count; i++) {
		const char pair[3] = { hex[2 * i], hex[2 * i + 1], '\0' };
		bytes[i] = (uint8_t)strtoul(pair, NULL, 16);
	}
	return true;
}

static bool ends_with(const char *text, const char *end)
{
	const size_t length = strlen(text);
	const size_t end_length = strlen(end);
	return length >= end_length && strcasecmp(text + length - end_length, end) == 0;
}

static bool parse_app(const cJSON *item, StoreCatalogue *catalogue, StoreApp *app, char *why, size_t why_bytes)
{
	const char *const id = text(item, "id");
	if (!id_ok(id)) {
		snprintf(why, why_bytes, "an app's id isn't lower-case letters, digits and -");
		return false;
	}
	snprintf(app->id, sizeof app->id, "%s", id);
	app->name = text(item, "name");
	app->author = text(item, "author");
	app->sentence = text(item, "sentence");
	app->needs = text(item, "needs");
	app->changes = text(item, "changes");
	app->version = text(item, "version");
	app->launch = text(item, "launch");
	app->screenshot = text(item, "screenshot");
	const char *const category = text(item, "category");
	const char *const title_id = text(item, "title_id");
	uint8_t title_id_bytes[4];
	uint64_t version_number;
	if (!app->name || !app->author || !app->sentence || !app->needs || !app->changes || !app->version || !category ||
		!store_path_ok(app->launch) || !ends_with(app->launch, ".xex") || !store_path_ok(app->screenshot) ||
		!hex_bytes(text(item, "screenshot_sha256"), app->screenshot_sha256, sizeof app->screenshot_sha256) ||
		!title_id || !hex_bytes(title_id, title_id_bytes, sizeof title_id_bytes) ||
		!whole(item, "version_number", UINT32_MAX, &version_number)) {
		snprintf(why, why_bytes, "app %s lacks a field, or one isn't as store_catalogue.py writes it", id);
		return false;
	}
	app->title_id = be32(title_id_bytes);
	app->version_number = (uint32_t)version_number;
	app->category = catalogue->category_count;
	for (size_t i = 0; i < catalogue->category_count; i++) {
		if (strcmp(catalogue->categories[i].id, category) == 0)
			app->category = i;
	}
	const cJSON *const package = cJSON_GetObjectItemCaseSensitive(item, "package");
	app->files = cJSON_GetObjectItemCaseSensitive(item, "files");
	app->package_url = text(package, "url");
	if (app->category == catalogue->category_count || !whole(package, "size", MAX_PACKAGE_BYTES, &app->package_bytes) ||
		!app->package_url || strncmp(app->package_url, "https://", 8) != 0 ||
		strlen(app->package_url) >= STORE_URL_BYTES ||
		!hex_bytes(text(package, "sha256"), app->package_sha256, sizeof app->package_sha256) ||
		!cJSON_IsArray(app->files)) {
		snprintf(why, why_bytes, "app %s's category, package or files aren't there", id);
		return false;
	}
	bool launched = false;
	const cJSON *file;
	cJSON_ArrayForEach(file, app->files)
	{
		const char *const path = text(file, "path");
		uint8_t hash[32];
		uint64_t bytes;
		if (!store_path_ok(path) || !whole(file, "size", MAX_PACKAGE_BYTES, &bytes) ||
			!hex_bytes(text(file, "sha256"), hash, sizeof hash)) {
			snprintf(why, why_bytes, "app %s has a file whose path, size or SHA-256 isn't one", id);
			return false;
		}
		app->installed_bytes += bytes;
		launched = launched || strcasecmp(path, app->launch) == 0;
	}
	if (!launched) {
		snprintf(why, why_bytes, "app %s launches %s, which isn't among its files", id, app->launch);
		return false;
	}
	return true;
}

bool store_catalogue_parse(const uint8_t *data, size_t size, StoreCatalogue *catalogue, char *why, size_t why_bytes)
{
	memset(catalogue, 0, sizeof *catalogue);
	catalogue->json = cJSON_ParseWithLength((const char *)data, size);
	if (!catalogue->json)
		return refuse(catalogue, why, why_bytes, "the catalogue isn't JSON");
	const cJSON *const json = catalogue->json;
	const cJSON *const categories = cJSON_GetObjectItemCaseSensitive(json, "categories");
	const cJSON *const apps = cJSON_GetObjectItemCaseSensitive(json, "apps");
	uint64_t format;
	uint64_t serial;
	catalogue->url = text(json, "url");
	if (!whole(json, "format", UINT32_MAX, &format) || format != FORMAT ||
		!whole(json, "serial", UINT32_MAX, &serial) || !catalogue->url || strncmp(catalogue->url, "https://", 8) != 0 ||
		strlen(catalogue->url) + sizeof ".sig" > STORE_URL_BYTES || !cJSON_IsArray(categories) ||
		!cJSON_IsArray(apps) || cJSON_GetArraySize(categories) > MAX_CATEGORIES || cJSON_GetArraySize(apps) > MAX_APPS)
		return refuse(
			catalogue, why, why_bytes, "the catalogue isn't format %d with a serial, url, categories and apps", FORMAT);
	catalogue->serial = (uint32_t)serial;
	catalogue->categories = calloc((size_t)cJSON_GetArraySize(categories) + 1, sizeof *catalogue->categories);
	catalogue->apps = calloc((size_t)cJSON_GetArraySize(apps) + 1, sizeof *catalogue->apps);
	if (!catalogue->categories || !catalogue->apps)
		return refuse(catalogue, why, why_bytes, "no memory for the catalogue");

	const cJSON *item;
	cJSON_ArrayForEach(item, categories)
	{
		StoreCategory *const category = &catalogue->categories[catalogue->category_count];
		const char *const id = text(item, "id");
		category->name = text(item, "name");
		category->sentence = text(item, "sentence");
		if (!id_ok(id) || !category->name || !category->sentence)
			return refuse(catalogue, why, why_bytes, "a category hasn't an id, a name and a sentence");
		for (size_t i = 0; i < catalogue->category_count; i++) {
			if (strcmp(catalogue->categories[i].id, id) == 0)
				return refuse(catalogue, why, why_bytes, "category %s comes twice", id);
		}
		snprintf(category->id, sizeof category->id, "%s", id);
		catalogue->category_count++;
	}
	cJSON_ArrayForEach(item, apps)
	{
		StoreApp *const app = &catalogue->apps[catalogue->app_count];
		char app_why[STORE_WHY_BYTES];
		if (!parse_app(item, catalogue, app, app_why, sizeof app_why))
			return refuse(catalogue, why, why_bytes, "%s", app_why);
		for (size_t i = 0; i < catalogue->app_count; i++) {
			if (strcmp(catalogue->apps[i].id, app->id) == 0)
				return refuse(catalogue, why, why_bytes, "app %s comes twice", app->id);
		}
		catalogue->app_count++;
	}
	return true;
}

// The kept catalogue: its signature's size as 4 big-endian bytes, the signature, then the catalogue as fetched.
static bool load_kept(const StoreConfig *config, StoreCatalogue *kept, char *why, size_t why_bytes)
{
	char path[STORE_PATH_BYTES];
	uint8_t *data;
	size_t size;
	if (snprintf(path, sizeof path, "%s%s", config->folder, KEPT_NAME) >= (int)sizeof path ||
		!store_read_file(path, 4 + MAX_SIGNATURE_BYTES + MAX_CATALOGUE_BYTES, &data, &size)) {
		snprintf(why, why_bytes, "none is kept");
		return false;
	}
	const size_t signature_size = size >= 4 ? be32(data) : SIZE_MAX;
	bool loaded = false;
	if (signature_size > MAX_SIGNATURE_BYTES || 4 + signature_size > size ||
		!store_signed(data + 4 + signature_size, size - 4 - signature_size, data + 4, signature_size))
		snprintf(why, why_bytes, "the kept one isn't signed with the store's key");
	else
		loaded = store_catalogue_parse(data + 4 + signature_size, size - 4 - signature_size, kept, why, why_bytes);
	free(data);
	return loaded;
}

static bool keep(
	const StoreConfig *config, const uint8_t *data, size_t size, const uint8_t *signature, size_t signature_size)
{
	char path[STORE_PATH_BYTES];
	uint8_t start[4 + MAX_SIGNATURE_BYTES];
	if (snprintf(path, sizeof path, "%s%s", config->folder, KEPT_NAME) >= (int)sizeof path)
		return false;
	start[0] = (uint8_t)(signature_size >> 24);
	start[1] = (uint8_t)(signature_size >> 16);
	start[2] = (uint8_t)(signature_size >> 8);
	start[3] = (uint8_t)signature_size;
	memcpy(start + 4, signature, signature_size);
	return store_write_file(path, start, 4 + signature_size, data, size);
}

StoreSource store_catalogue_update(const StoreConfig *config, StoreCatalogue *catalogue, char *why, size_t why_bytes)
{
	StoreCatalogue kept;
	char kept_why[STORE_WHY_BYTES];
	const bool have_kept = load_kept(config, &kept, kept_why, sizeof kept_why);
	char url[STORE_URL_BYTES];
	char signature_url[STORE_URL_BYTES + sizeof ".sig"];
	snprintf(url, sizeof url, "%s", config->url[0] ? config->url : have_kept ? kept.url : STORE_DEFAULT_URL);
	snprintf(signature_url, sizeof signature_url, "%s.sig", url);

	uint8_t *data = NULL;
	uint8_t *signature = NULL;
	size_t size = 0;
	size_t signature_size = 0;
	const bool fetched = store_fetch(config, url, MAX_CATALOGUE_BYTES, &data, &size, why, why_bytes) &&
		store_fetch(config, signature_url, MAX_SIGNATURE_BYTES, &signature, &signature_size, why, why_bytes);
	StoreCatalogue fresh;
	bool taken = false;
	if (fetched) {
		if (!store_signed(data, size, signature, signature_size)) {
			snprintf(why, why_bytes, "%s isn't signed with the store's key", url);
		} else if (!store_catalogue_parse(data, size, &fresh, why, why_bytes)) {
		} else if (have_kept && fresh.serial < kept.serial) {
			snprintf(why, why_bytes, "%s is serial %u, older than the kept one's %u", url, (unsigned)fresh.serial,
				(unsigned)kept.serial);
			store_catalogue_free(&fresh);
		} else {
			taken = true;
			if (keep(config, data, size, signature, signature_size))
				snprintf(why, why_bytes, "serial %u from %s, kept", (unsigned)fresh.serial, url);
			else
				snprintf(why, why_bytes, "serial %u from %s, but it couldn't be kept in %s", (unsigned)fresh.serial,
					url, config->folder);
		}
	}
	free(data);
	free(signature);
	if (taken) {
		if (have_kept)
			store_catalogue_free(&kept);
		*catalogue = fresh;
		return STORE_FRESH;
	}
	if (have_kept) {
		*catalogue = kept;
		return fetched ? STORE_KEPT_REFUSED : STORE_KEPT_OFFLINE;
	}
	memset(catalogue, 0, sizeof *catalogue);
	return STORE_NONE;
}

bool store_app_folder(const StoreConfig *config, const StoreApp *app, char *folder, size_t folder_bytes)
{
	if (app->self)
		return snprintf(folder, folder_bytes, "game:") < (int)folder_bytes;
	return snprintf(folder, folder_bytes, "%s%s", config->apps, app->id) < (int)folder_bytes;
}

bool store_app_xex(const StoreConfig *config, const StoreApp *app, char *path, size_t path_bytes)
{
	char folder[STORE_PATH_BYTES];
	if (!store_app_folder(config, app, folder, sizeof folder) ||
		snprintf(path, path_bytes, "%s\\%s", folder, app->launch) >= (int)path_bytes)
		return false;
	for (char *c = path; *c; c++) {
		if (*c == '/')
			*c = '\\';
	}
	return true;
}

void store_installed_app(const StoreConfig *config, StoreApp *app)
{
	char folder[STORE_PATH_BYTES];
	char path[STORE_PATH_BYTES];
	app->installed = STORE_INSTALLED_NONE;
	app->receipt = false;
	app->receipt_author_same = false;
	app->self = config->self_title_id && app->title_id == config->self_title_id;
	if (!store_app_folder(config, app, folder, sizeof folder))
		return;
	if (snprintf(path, sizeof path, "%s\\app.json", folder) < (int)sizeof path) {
		uint8_t *data;
		size_t size;
		if (store_read_file(path, MAX_RECEIPT_BYTES, &data, &size)) {
			cJSON *const receipt = cJSON_ParseWithLength((const char *)data, size);
			const char *const author = text(receipt, "author");
			app->receipt = receipt != NULL;
			app->receipt_author_same = author && strcmp(author, app->author) == 0;
			cJSON_Delete(receipt);
			free(data);
		}
	}
	FILE *const file = store_app_xex(config, app, path, sizeof path) ? fopen(path, "rb") : NULL;
	if (!file)
		return;
	uint8_t *const header = malloc(XEX_HEADER_BYTES);
	const size_t size = header ? fread(header, 1, XEX_HEADER_BYTES, file) : 0;
	fclose(file);
	uint32_t title_id;
	uint32_t version;
	if (store_xex_identity(header, size, &title_id, &version) && title_id == app->title_id) {
		app->installed_version = version;
		app->installed = version == app->version_number ? STORE_INSTALLED_CURRENT
			: version < app->version_number             ? STORE_INSTALLED_OLDER
														: STORE_INSTALLED_NEWER;
	}
	free(header);
}

void store_installed(const StoreConfig *config, StoreCatalogue *catalogue)
{
	for (size_t i = 0; i < catalogue->app_count; i++)
		store_installed_app(config, &catalogue->apps[i]);
}
