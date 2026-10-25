// The store's network and disk work on a thread of its own, so the screens keep drawing: the catalogue first, then the
// screenshot asked for last.

#pragma once

#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

// config stays the caller's until store_worker_stop.
bool store_worker_start(const StoreConfig *config);
void store_worker_stop(void);
// Whether the catalogue is in. From then it doesn't change, unless store_worker_retry fetches again after STORE_NONE.
bool store_worker_catalogue(const StoreCatalogue **catalogue, StoreSource *source, const char **why);
void store_worker_retry(void);
// Asks for an app's screenshot, in place of the last one asked for if that hasn't started.
void store_worker_want_shot(size_t app);
// A screenshot that's done: true with its app and its pixels, which are the caller's, or NULL if it couldn't be had.
bool store_worker_take_shot(size_t *app, uint32_t **pixels);

#ifdef __cplusplus
}
#endif
