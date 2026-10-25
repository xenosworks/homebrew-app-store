// Fetches over HTTPS with curl-xenon, as curl_check does (curl-xenon's tests/xsdk/curl_check.c): the receive buffer
// raised, as far hosts run 5 to 8 times faster with it (xsdk's docs/HARDWARE.md, "Milestone 56 on the console").

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

#include "store.h"

enum {
	RAISED_BUFFER_BYTES = 256 * 1024,
	// Public hosts' handshakes have taken over a second (curl_check).
	CONNECT_TIMEOUT_MS = 15000,
	// A transfer this long without a byte ends.
	LOW_SPEED_SECONDS = 10,
	MAX_REDIRECTS = 5,
	// What a download gathers before its writer puts it on the disk.
	PIECE_BYTES = 1024 * 1024,
};

typedef struct {
	uint8_t *data;
	size_t size;
	size_t capacity;
	size_t max_bytes;
	bool too_big;
} Body;

static size_t on_body(char *data, size_t size, size_t count, void *user)
{
	Body *const body = user;
	const size_t length = size * count;
	if (length > body->max_bytes - body->size) {
		body->too_big = true;
		return 0;
	}
	if (body->size + length > body->capacity) {
		size_t capacity = body->capacity ? body->capacity * 2 : 64 * 1024;
		while (capacity < body->size + length)
			capacity *= 2;
		uint8_t *const grown = realloc(body->data, capacity);
		if (!grown)
			return 0;
		body->data = grown;
		body->capacity = capacity;
	}
	memcpy(body->data + body->size, data, length);
	body->size += length;
	return length;
}

static int on_socket(void *user, curl_socket_t fd, curlsocktype purpose)
{
	(void)user;
	(void)purpose;
	const int size = RAISED_BUFFER_BYTES;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	return CURL_SOCKOPT_OK;
}

// A transfer of url over HTTPS alone, redirects followed as a GitHub release's asset redirects to its storage.
static CURL *prepare(const StoreConfig *config, const char *url, struct curl_slist *resolve, char *error)
{
	CURL *const curl = curl_easy_init();
	if (!curl)
		return NULL;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)MAX_REDIRECTS);
	curl_easy_setopt(curl, CURLOPT_CAINFO, config->ca);
	curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)LOW_SPEED_SECONDS);
	curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, on_socket);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	return curl;
}

static void say_failed(const char *url, CURLcode result, const char *error, char *why, size_t why_bytes)
{
	snprintf(why, why_bytes, "%s: %s%s%s", url, curl_easy_strerror(result), error[0] ? ": " : "", error);
}

bool store_fetch(const StoreConfig *config, const char *url, size_t max_bytes, uint8_t **data, size_t *size, char *why,
	size_t why_bytes)
{
	Body body = { .max_bytes = max_bytes };
	char error[CURL_ERROR_SIZE] = "";
	struct curl_slist *resolve = config->resolve[0] ? curl_slist_append(NULL, config->resolve) : NULL;
	CURL *const curl = prepare(config, url, resolve, error);
	CURLcode result = CURLE_FAILED_INIT;
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
		result = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
	}
	curl_slist_free_all(resolve);
	if (result != CURLE_OK) {
		free(body.data);
		if (body.too_big)
			snprintf(why, why_bytes, "%s is over %u bytes", url, (unsigned)max_bytes);
		else
			say_failed(url, result, error, why, why_bytes);
		return false;
	}
	*data = body.data;
	*size = body.size;
	return true;
}

// A download: curl's thread fills data, the writer's puts each piece of it on the disk.
typedef struct {
	CURL *curl;
	uint8_t *data;
	size_t size;
	// What the part file held when the download started.
	size_t start;
	bool status_seen;
	bool too_big;
	StoreProgress *progress;
	FILE *file;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	size_t received;
	size_t written;
	bool done;
	// The server sent the whole file, not the part asked for, so the part file starts over.
	bool restart;
	bool write_failed;
} Download;

static size_t on_download(char *bytes, size_t size, size_t count, void *user)
{
	Download *const download = user;
	const size_t length = size * count;
	if (!download->status_seen) {
		download->status_seen = true;
		long code = 0;
		curl_easy_getinfo(download->curl, CURLINFO_RESPONSE_CODE, &code);
		// A 200 to a Range is the whole file again (xsdk's docs/GUIDE.md, "Downloading"). Nothing new has come, so
		// the writer has nothing of this transfer's in hand.
		if (download->start && code != 206) {
			pthread_mutex_lock(&download->lock);
			download->received = 0;
			download->written = 0;
			download->restart = true;
			pthread_mutex_unlock(&download->lock);
		}
	}
	pthread_mutex_lock(&download->lock);
	const size_t received = download->received;
	pthread_mutex_unlock(&download->lock);
	if (length > download->size - received) {
		download->too_big = true;
		return 0;
	}
	memcpy(download->data + received, bytes, length);
	pthread_mutex_lock(&download->lock);
	download->received += length;
	if (download->received - download->written >= PIECE_BYTES)
		pthread_cond_signal(&download->wake);
	pthread_mutex_unlock(&download->lock);
	download->progress->done_bytes = (uint32_t)(received + length);
	download->progress->fetched_bytes += (uint32_t)length;
	return length;
}

static void *write_pieces(void *user)
{
	Download *const download = user;
	pthread_mutex_lock(&download->lock);
	for (;;) {
		while (!download->done && !download->restart && download->received - download->written < PIECE_BYTES)
			pthread_cond_wait(&download->wake, &download->lock);
		if (download->restart) {
			download->restart = false;
			pthread_mutex_unlock(&download->lock);
			const bool truncated = fflush(download->file) == 0 && ftruncate(fileno(download->file), 0) == 0 &&
				fseek(download->file, 0, SEEK_SET) == 0;
			pthread_mutex_lock(&download->lock);
			download->write_failed = download->write_failed || !truncated;
			continue;
		}
		const size_t from = download->written;
		const size_t to = download->received;
		const bool done = download->done;
		pthread_mutex_unlock(&download->lock);
		const bool wrote = to == from ||
			(fwrite(download->data + from, 1, to - from, download->file) == to - from && fflush(download->file) == 0);
		pthread_mutex_lock(&download->lock);
		download->write_failed = download->write_failed || !wrote;
		download->written = to;
		if (done)
			break;
	}
	pthread_mutex_unlock(&download->lock);
	return NULL;
}

bool store_download(const StoreConfig *config, const char *url, const char *part_path, size_t size, uint8_t **data,
	StoreProgress *progress, char *why, size_t why_bytes)
{
	Download download = { .size = size, .progress = progress };
	download.data = malloc(size ? size : 1);
	if (!download.data) {
		snprintf(why, why_bytes, "no memory for %u bytes", (unsigned)size);
		return false;
	}
	// What a cut download left, if it isn't more than the whole.
	FILE *const kept = fopen(part_path, "rb");
	if (kept) {
		const size_t read = fread(download.data, 1, size, kept);
		download.start = fgetc(kept) == EOF ? read : 0;
		fclose(kept);
	}
	progress->total_bytes = (uint32_t)size;
	progress->done_bytes = (uint32_t)download.start;
	progress->fetched_bytes = 0;
	if (download.start == size) {
		*data = download.data;
		return true;
	}
	download.received = download.written = download.start;
	download.file = fopen(part_path, download.start ? "ab" : "wb");
	if (!download.file) {
		free(download.data);
		snprintf(why, why_bytes, "%s can't be written", part_path);
		return false;
	}
	pthread_mutex_init(&download.lock, NULL);
	pthread_cond_init(&download.wake, NULL);
	pthread_t writer;
	const bool writing = pthread_create(&writer, NULL, write_pieces, &download) == 0;

	char error[CURL_ERROR_SIZE] = "";
	struct curl_slist *resolve = config->resolve[0] ? curl_slist_append(NULL, config->resolve) : NULL;
	download.curl = writing ? prepare(config, url, resolve, error) : NULL;
	CURLcode result = CURLE_FAILED_INIT;
	if (download.curl) {
		curl_easy_setopt(download.curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)download.start);
		curl_easy_setopt(download.curl, CURLOPT_WRITEFUNCTION, on_download);
		curl_easy_setopt(download.curl, CURLOPT_WRITEDATA, &download);
		result = curl_easy_perform(download.curl);
		curl_easy_cleanup(download.curl);
	}
	curl_slist_free_all(resolve);
	// What came is put on the disk even when the transfer was cut, for the next try to take up.
	pthread_mutex_lock(&download.lock);
	download.done = true;
	pthread_cond_signal(&download.wake);
	pthread_mutex_unlock(&download.lock);
	if (writing)
		pthread_join(writer, NULL);
	const bool synced = fflush(download.file) == 0 && fsync(fileno(download.file)) == 0;
	fclose(download.file);
	pthread_cond_destroy(&download.wake);
	pthread_mutex_destroy(&download.lock);

	if (result != CURLE_OK || download.received != size || download.write_failed || !synced) {
		free(download.data);
		if (download.too_big)
			snprintf(why, why_bytes, "%s is over the catalogue's %u bytes", url, (unsigned)size);
		else if (result != CURLE_OK)
			say_failed(url, result, error, why, why_bytes);
		else
			snprintf(why, why_bytes, "%s: %u bytes of %u came or were kept", url, (unsigned)download.received,
				(unsigned)size);
		return false;
	}
	*data = download.data;
	return true;
}
