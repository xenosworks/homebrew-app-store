// The store's network and disk work on a thread of its own, so the screens keep drawing: cut installs recovered and the
// catalogue fetched first, then installs and removals, then the screenshot asked for last.

#pragma once

#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	STORE_JOB_INSTALL,
	// app.json's files, then the rest of the app's folder too.
	STORE_JOB_REMOVE,
	STORE_JOB_REMOVE_ALL,
} StoreJobKind;

// A job that's done, and the app's state after it, for the screens to put into the catalogue.
typedef struct {
	StoreJobKind kind;
	size_t app;
	bool done;
	char why[STORE_WHY_BYTES];
	StoreInstalled installed;
	uint32_t installed_version;
	bool receipt;
	bool receipt_author_same;
	// Files in the app's folder beyond app.json's.
	size_t other_files;
} StoreJobResult;

// config stays the caller's until store_worker_stop.
bool store_worker_start(const StoreConfig *config);
void store_worker_stop(void);
// Whether the catalogue is in. From then on only the screens' thread writes it, and only from a job's result, unless
// store_worker_retry fetches again after STORE_NONE.
bool store_worker_catalogue(StoreCatalogue **catalogue, StoreSource *source, const char **why);
void store_worker_retry(void);
// Asks for an app's screenshot, in place of the last one asked for if that hasn't started.
void store_worker_want_shot(size_t app);
// A screenshot that's done: true with its app and its pixels, which are the caller's, or NULL if it couldn't be had.
bool store_worker_take_shot(size_t *app, uint32_t **pixels);
// Starts a job on an app; false if one is already under way.
bool store_worker_job(StoreJobKind kind, size_t app);
// Whether a job is under way, and on which app; its progress as it runs.
bool store_worker_busy(size_t *app);
const StoreProgress *store_worker_progress(void);
bool store_worker_take_job(StoreJobResult *result);

#ifdef __cplusplus
}
#endif
