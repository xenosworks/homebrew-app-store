// Installing, updating and removing apps (xsdk's docs/scope/63-homebrew-store.md, step 5). An app's folder holds its
// files and app.json, the store's receipt: the catalogue entry's id, version and files with their sizes and SHA-256,
// so an update replaces and a removal deletes those files alone, and what the app made itself stays. An install's
// receipt is written first as app.json.pending and renamed app.json.part once every file is checked; a cut before that
// rename is taken back on the next start, and one after it finished.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "miniz.h"
#include "store.h"
#include "xsdk/log.h"

enum {
	MAX_RECEIPT_BYTES = 1024 * 1024,
	MAX_NAMES = 4096,
};

static const char PENDING[] = "app.json.pending";
static const char CHECKED[] = "app.json.part";
static const char RECEIPT[] = "app.json";

// dir followed by a backslash and path, its slashes made backslashes; false if it doesn't fit.
static bool join(char *out, size_t out_bytes, const char *dir, const char *path)
{
	if (snprintf(out, out_bytes, "%s\\%s", dir, path) >= (int)out_bytes)
		return false;
	for (char *c = out; *c; c++) {
		if (*c == '/')
			*c = '\\';
	}
	return true;
}

static bool app_folder(const StoreConfig *config, const StoreApp *app, char *folder, size_t folder_bytes)
{
	return snprintf(folder, folder_bytes, "%s%s", config->apps, app->id) < (int)folder_bytes;
}

// The folders above a file inside folder.
static void make_parents(const char *folder, const char *path)
{
	char parent[STORE_PATH_BYTES];
	for (const char *slash = strchr(path, '/'); slash; slash = strchr(slash + 1, '/')) {
		if (snprintf(parent, sizeof parent, "%s\\%.*s", folder, (int)(slash - path), path) < (int)sizeof parent) {
			for (char *c = parent; *c; c++) {
				if (*c == '/')
					*c = '\\';
			}
			mkdir(parent, 0777);
		}
	}
}

// The folders above a removed file inside folder, deepest first, each that's left empty: remove takes an empty folder
// as well as a file (xsdk's src/path.c, unlinkat).
static void remove_parents(const char *folder, const char *path)
{
	char parent[STORE_PATH_BYTES];
	size_t length = strlen(path);
	for (;;) {
		while (length > 0 && path[length - 1] != '/')
			length--;
		if (length == 0)
			return;
		length--;
		if (snprintf(parent, sizeof parent, "%s\\%.*s", folder, (int)length, path) < (int)sizeof parent) {
			for (char *c = parent; *c; c++) {
				if (*c == '/')
					*c = '\\';
			}
			remove(parent);
		}
	}
}

// A receipt's JSON, its files each a path an app's folder takes; NULL if it isn't there or isn't one.
static cJSON *read_receipt(const char *folder, const char *name)
{
	char path[STORE_PATH_BYTES];
	uint8_t *data;
	size_t size;
	if (!join(path, sizeof path, folder, name) || !store_read_file(path, MAX_RECEIPT_BYTES, &data, &size))
		return NULL;
	cJSON *receipt = cJSON_ParseWithLength((const char *)data, size);
	free(data);
	const cJSON *const files = cJSON_GetObjectItemCaseSensitive(receipt, "files");
	const cJSON *file;
	bool good = cJSON_IsArray(files);
	cJSON_ArrayForEach(file, files)
	{
		const cJSON *const path_item = cJSON_GetObjectItemCaseSensitive(file, "path");
		good = good && cJSON_IsString(path_item) && store_path_ok(path_item->valuestring);
	}
	if (!good) {
		cJSON_Delete(receipt);
		return NULL;
	}
	return receipt;
}

static const char *file_path(const cJSON *file)
{
	return cJSON_GetObjectItemCaseSensitive(file, "path")->valuestring;
}

static bool listed(const cJSON *files, const char *path)
{
	const cJSON *file;
	cJSON_ArrayForEach(file, files)
	{
		if (strcasecmp(file_path(file), path) == 0)
			return true;
	}
	return false;
}

static cJSON *make_receipt(const StoreCatalogue *catalogue, const StoreApp *app)
{
	char title_id[9];
	snprintf(title_id, sizeof title_id, "%08X", (unsigned)app->title_id);
	cJSON *const receipt = cJSON_CreateObject();
	if (!receipt || !cJSON_AddStringToObject(receipt, "id", app->id) ||
		!cJSON_AddStringToObject(receipt, "name", app->name) ||
		!cJSON_AddStringToObject(receipt, "author", app->author) ||
		!cJSON_AddStringToObject(receipt, "version", app->version) ||
		!cJSON_AddNumberToObject(receipt, "version_number", app->version_number) ||
		!cJSON_AddStringToObject(receipt, "title_id", title_id) ||
		!cJSON_AddStringToObject(receipt, "catalogue", catalogue->url) ||
		!cJSON_AddNumberToObject(receipt, "serial", catalogue->serial) ||
		!cJSON_AddItemToObject(receipt, "files", cJSON_Duplicate(app->files, true))) {
		cJSON_Delete(receipt);
		return NULL;
	}
	return receipt;
}

// Renames each file's checked .part into place, then app.json.part to app.json.
static bool finish(const char *folder, const cJSON *files)
{
	char target[STORE_PATH_BYTES];
	char part[STORE_PATH_BYTES + 8];
	char checked[STORE_PATH_BYTES];
	const cJSON *file;
	bool finished = true;
	cJSON_ArrayForEach(file, files)
	{
		if (!join(target, sizeof target, folder, file_path(file)))
			return false;
		snprintf(part, sizeof part, "%s.part", target);
		struct stat status;
		if (stat(part, &status) == 0 && rename(part, target) != 0)
			finished = false;
	}
	return finished && join(target, sizeof target, folder, RECEIPT) && join(checked, sizeof checked, folder, CHECKED) &&
		rename(checked, target) == 0;
}

static void remove_parts(const char *folder, const cJSON *files)
{
	char target[STORE_PATH_BYTES];
	char part[STORE_PATH_BYTES + 8];
	const cJSON *file;
	cJSON_ArrayForEach(file, files)
	{
		if (join(target, sizeof target, folder, file_path(file))) {
			snprintf(part, sizeof part, "%s.part", target);
			remove(part);
		}
	}
}

typedef struct {
	FILE *file;
	mbedtls_sha256_context sha;
	StoreProgress *progress;
	bool failed;
} Sink;

static size_t on_unpacked(void *user, mz_uint64 offset, const void *data, size_t size)
{
	Sink *const sink = user;
	(void)offset;
	if (fwrite(data, 1, size, sink->file) != size || mbedtls_sha256_update(&sink->sha, data, size) != 0) {
		sink->failed = true;
		return 0;
	}
	sink->progress->done_bytes += (uint32_t)size;
	return size;
}

// Unpacks one of the entry's files beside its place as <name>.part, and checks it's the one the catalogue signed.
static bool unpack(
	mz_zip_archive *zip, const char *folder, const cJSON *file, StoreProgress *progress, char *why, size_t why_bytes)
{
	const char *const path = file_path(file);
	const double size = cJSON_GetObjectItemCaseSensitive(file, "size")->valuedouble;
	const char *const sha256 = cJSON_GetObjectItemCaseSensitive(file, "sha256")->valuestring;
	char target[STORE_PATH_BYTES];
	char part[STORE_PATH_BYTES + 8];
	const int index = mz_zip_reader_locate_file(zip, path, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
	mz_zip_archive_file_stat status;
	if (index < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)index, &status) || status.m_is_directory ||
		(double)status.m_uncomp_size != size) {
		snprintf(why, why_bytes, "the package hasn't %s at the catalogue's size", path);
		return false;
	}
	if (!join(target, sizeof target, folder, path)) {
		snprintf(why, why_bytes, "%s is too long a path", path);
		return false;
	}
	snprintf(part, sizeof part, "%s.part", target);
	make_parents(folder, path);
	Sink sink = { .file = fopen(part, "wb"), .progress = progress };
	if (!sink.file) {
		snprintf(why, why_bytes, "%s can't be written", part);
		return false;
	}
	mbedtls_sha256_init(&sink.sha);
	uint8_t hash[32];
	const bool unpacked = mbedtls_sha256_starts(&sink.sha, 0) == 0 &&
		mz_zip_reader_extract_to_callback(zip, (mz_uint)index, on_unpacked, &sink, 0) && !sink.failed &&
		mbedtls_sha256_finish(&sink.sha, hash) == 0;
	mbedtls_sha256_free(&sink.sha);
	const bool kept = fflush(sink.file) == 0 && fsync(fileno(sink.file)) == 0;
	fclose(sink.file);
	char hex[65];
	for (int i = 0; i < 32; i++)
		snprintf(hex + 2 * i, 3, "%02x", hash[i]);
	if (!unpacked || !kept) {
		snprintf(why, why_bytes, "%s didn't unpack: %s", path, mz_zip_get_error_string(mz_zip_get_last_error(zip)));
		return false;
	}
	if (strcmp(hex, sha256) != 0) {
		snprintf(why, why_bytes, "%s isn't the file the catalogue signed", path);
		return false;
	}
	return true;
}

// Beside the kept catalogue, named after the package's hash, so a cut download is taken up only for the same package.
bool store_download_path(const StoreConfig *config, const StoreApp *app, char *path, size_t path_bytes)
{
	return snprintf(path, path_bytes, "%sdownloads\\%s-%02x%02x%02x%02x.zip.part", config->folder, app->id,
			   app->package_sha256[0], app->package_sha256[1], app->package_sha256[2],
			   app->package_sha256[3]) < (int)path_bytes;
}

bool store_install(const StoreConfig *config, const StoreCatalogue *catalogue, const StoreApp *app,
	StoreProgress *progress, char *why, size_t why_bytes)
{
	char folder[STORE_PATH_BYTES];
	char apps[STORE_PATH_BYTES];
	char downloads[STORE_PATH_BYTES];
	char part[STORE_PATH_BYTES];
	char pending[STORE_PATH_BYTES];
	char checked[STORE_PATH_BYTES];
	if (!app_folder(config, app, folder, sizeof folder) || !join(pending, sizeof pending, folder, PENDING) ||
		!join(checked, sizeof checked, folder, CHECKED) ||
		snprintf(downloads, sizeof downloads, "%sdownloads", config->folder) >= (int)sizeof downloads ||
		!store_download_path(config, app, part, sizeof part)) {
		snprintf(why, why_bytes, "%s's folders are too long", app->id);
		return false;
	}
	snprintf(apps, sizeof apps, "%.*s", (int)strlen(config->apps) - 1, config->apps);
	mkdir(apps, 0777);
	mkdir(folder, 0777);
	mkdir(downloads, 0777);

	progress->step = STORE_STEP_DOWNLOADING;
	uint8_t *package;
	if (!store_download(
			config, app->package_url, part, (size_t)app->package_bytes, &package, progress, why, why_bytes)) {
		progress->step = STORE_STEP_NONE;
		return false;
	}
	uint8_t hash[32];
	if (mbedtls_sha256(package, (size_t)app->package_bytes, hash, 0) != 0 ||
		memcmp(hash, app->package_sha256, sizeof hash) != 0) {
		// Not kept, so the next try fetches it whole.
		remove(part);
		free(package);
		progress->step = STORE_STEP_NONE;
		snprintf(why, why_bytes, "%s isn't the package the catalogue signed", app->package_url);
		return false;
	}

	progress->step = STORE_STEP_INSTALLING;
	progress->done_bytes = 0;
	progress->total_bytes = (uint32_t)app->installed_bytes;
	cJSON *const before = read_receipt(folder, RECEIPT);
	cJSON *const receipt = make_receipt(catalogue, app);
	char *const text = receipt ? cJSON_Print(receipt) : NULL;
	mz_zip_archive zip;
	memset(&zip, 0, sizeof zip);
	bool good = text && store_write_file(pending, text, strlen(text), NULL, 0);
	if (!good)
		snprintf(why, why_bytes, "%s can't be written", pending);
	else if (!(good = mz_zip_reader_init_mem(&zip, package, (size_t)app->package_bytes, 0)))
		snprintf(why, why_bytes, "the package isn't a zip: %s", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
	const cJSON *file;
	cJSON_ArrayForEach(file, app->files)
	{
		good = good && unpack(&zip, folder, file, progress, why, why_bytes);
	}
	mz_zip_reader_end(&zip);
	free(package);
	cJSON_free(text);
	if (!good) {
		remove_parts(folder, app->files);
		remove(pending);
		remove(part);
	} else if (rename(pending, checked) != 0 || !finish(folder, app->files)) {
		// Checked, so the next start finishes it.
		good = false;
		snprintf(
			why, why_bytes, "%s's files couldn't all be put in place; the store finishes when it starts next", app->id);
	} else {
		// What the version before installed and this one doesn't.
		const cJSON *files = cJSON_GetObjectItemCaseSensitive(before, "files");
		cJSON_ArrayForEach(file, files)
		{
			char target[STORE_PATH_BYTES];
			if (!listed(app->files, file_path(file)) && join(target, sizeof target, folder, file_path(file))) {
				remove(target);
				remove_parents(folder, file_path(file));
			}
		}
		remove(part);
	}
	cJSON_Delete(before);
	cJSON_Delete(receipt);
	progress->step = STORE_STEP_NONE;
	return good;
}

// The names in a folder, read whole before anything in it changes: xsdk opens a folder sharing nothing, so one held
// open can't be deleted (xsdk's docs/HARDWARE.md, "Files renamed on the console").
static size_t list_folder(const char *path, char (*names)[STORE_PATH_BYTES], size_t max_names)
{
	DIR *const dir = opendir(path);
	size_t count = 0;
	struct dirent *entry;
	while (dir && count < max_names && (entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			snprintf(names[count++], STORE_PATH_BYTES, "%s", entry->d_name);
	}
	if (dir)
		closedir(dir);
	return count;
}

static bool is_folder(const char *path)
{
	struct stat status;
	return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static void delete_tree(const char *path)
{
	char (*const names)[STORE_PATH_BYTES] = malloc(MAX_NAMES * sizeof *names);
	const size_t count = names ? list_folder(path, names, MAX_NAMES) : 0;
	for (size_t i = 0; i < count; i++) {
		char child[STORE_PATH_BYTES];
		if (snprintf(child, sizeof child, "%s\\%s", path, names[i]) >= (int)sizeof child)
			continue;
		if (is_folder(child))
			delete_tree(child);
		else
			remove(child);
	}
	free(names);
	remove(path);
}

// Counts the files under path, relative to the app's folder as prefix, that files doesn't list.
static size_t count_others(const char *path, const char *prefix, const cJSON *files)
{
	char (*const names)[STORE_PATH_BYTES] = malloc(MAX_NAMES * sizeof *names);
	const size_t count = names ? list_folder(path, names, MAX_NAMES) : 0;
	size_t others = 0;
	for (size_t i = 0; i < count; i++) {
		char child[STORE_PATH_BYTES];
		char relative[STORE_PATH_BYTES];
		if (snprintf(child, sizeof child, "%s\\%s", path, names[i]) >= (int)sizeof child ||
			snprintf(relative, sizeof relative, "%s%s", prefix, names[i]) >= (int)sizeof relative)
			continue;
		if (is_folder(child)) {
			strncat(relative, "/", sizeof relative - strlen(relative) - 1);
			others += count_others(child, relative, files);
		} else if (strcasecmp(relative, RECEIPT) != 0 && !listed(files, relative)) {
			others++;
		}
	}
	free(names);
	return others;
}

size_t store_other_files(const StoreConfig *config, const StoreApp *app)
{
	char folder[STORE_PATH_BYTES];
	if (!app_folder(config, app, folder, sizeof folder))
		return 0;
	cJSON *const receipt = read_receipt(folder, RECEIPT);
	const size_t others = count_others(folder, "", cJSON_GetObjectItemCaseSensitive(receipt, "files"));
	cJSON_Delete(receipt);
	return others;
}

bool store_remove(const StoreConfig *config, const StoreApp *app, bool everything, char *why, size_t why_bytes)
{
	char folder[STORE_PATH_BYTES];
	char path[STORE_PATH_BYTES];
	if (!app_folder(config, app, folder, sizeof folder)) {
		snprintf(why, why_bytes, "%s's folder is too long", app->id);
		return false;
	}
	cJSON *const receipt = read_receipt(folder, RECEIPT);
	if (!receipt && !everything) {
		snprintf(why, why_bytes, "%s has no app.json, so the store didn't install it", folder);
		return false;
	}
	const cJSON *file;
	cJSON_ArrayForEach(file, cJSON_GetObjectItemCaseSensitive(receipt, "files"))
	{
		if (join(path, sizeof path, folder, file_path(file))) {
			remove(path);
			remove_parents(folder, file_path(file));
		}
	}
	cJSON_Delete(receipt);
	if (join(path, sizeof path, folder, RECEIPT))
		remove(path);
	if (everything)
		delete_tree(folder);
	return true;
}

void store_recover(const StoreConfig *config)
{
	char apps[STORE_PATH_BYTES];
	snprintf(apps, sizeof apps, "%.*s", (int)strlen(config->apps) - 1, config->apps);
	char (*const names)[STORE_PATH_BYTES] = malloc(MAX_NAMES * sizeof *names);
	const size_t count = names ? list_folder(apps, names, MAX_NAMES) : 0;
	for (size_t i = 0; i < count; i++) {
		char folder[STORE_PATH_BYTES];
		char pending[STORE_PATH_BYTES];
		if (snprintf(folder, sizeof folder, "%s\\%s", apps, names[i]) >= (int)sizeof folder ||
			!join(pending, sizeof pending, folder, PENDING))
			continue;
		cJSON *receipt = read_receipt(folder, CHECKED);
		if (receipt) {
			const bool finished = finish(folder, cJSON_GetObjectItemCaseSensitive(receipt, "files"));
			remove(pending);
			xsdk_log_write("store: %s: an install cut after its files were checked %s", folder,
				finished ? "is finished" : "still couldn't be finished");
		} else if ((receipt = read_receipt(folder, PENDING))) {
			remove_parts(folder, cJSON_GetObjectItemCaseSensitive(receipt, "files"));
			remove(pending);
			xsdk_log_write("store: %s: an install cut before its files were checked is taken back", folder);
		}
		cJSON_Delete(receipt);
	}
	free(names);
}
