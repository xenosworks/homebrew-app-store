// Fetches over HTTPS into memory with curl-xenon, as curl_check does (curl-xenon's tests/xsdk/curl_check.c): the
// receive buffer raised, as far hosts run 5 to 8 times faster with it (xsdk's docs/HARDWARE.md, "Milestone 56 on the
// console").

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <curl/curl.h>

#include "store.h"

enum {
	RAISED_BUFFER_BYTES = 256 * 1024,
	// Public hosts' handshakes have taken over a second (curl_check).
	CONNECT_TIMEOUT_MS = 15000,
	// A transfer this long without a byte ends.
	LOW_SPEED_SECONDS = 10,
	MAX_REDIRECTS = 5,
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

bool store_fetch(const StoreConfig *config, const char *url, size_t max_bytes, uint8_t **data, size_t *size, char *why,
	size_t why_bytes)
{
	Body body = { .max_bytes = max_bytes };
	char error[CURL_ERROR_SIZE] = "";
	struct curl_slist *resolve = config->resolve[0] ? curl_slist_append(NULL, config->resolve) : NULL;
	CURL *const curl = curl_easy_init();
	CURLcode result = CURLE_FAILED_INIT;
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
		// A GitHub release's asset redirects to its storage.
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)MAX_REDIRECTS);
		curl_easy_setopt(curl, CURLOPT_CAINFO, config->ca);
		curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)LOW_SPEED_SECONDS);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
		curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, on_socket);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
		result = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
	}
	curl_slist_free_all(resolve);
	if (result != CURLE_OK) {
		free(body.data);
		if (body.too_big)
			snprintf(why, why_bytes, "%s is over %u bytes", url, (unsigned)max_bytes);
		else
			snprintf(why, why_bytes, "%s: %s%s%s", url, curl_easy_strerror(result), error[0] ? ": " : "", error);
		return false;
	}
	*data = body.data;
	*size = body.size;
	return true;
}
