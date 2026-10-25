#include "worker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xsdk/log.h"

static struct {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	const StoreConfig *config;
	bool stop;
	bool update;
	bool catalogue_in;
	StoreCatalogue catalogue;
	StoreSource source;
	char why[STORE_WHY_BYTES];
	bool shot_wanted;
	size_t wanted;
	bool shot_done;
	size_t done;
	uint32_t *done_pixels;
	bool job_wanted;
	bool job_running;
	bool job_done;
	StoreJobKind job_kind;
	size_t job_app;
	StoreJobResult result;
	StoreProgress progress;
} worker;

static const char *const SOURCES[] = { "fetched", "kept, as fetching failed", "kept, as what came was refused",
	"none" };
static const char *const JOBS[] = { "install", "removal", "removal with what it made" };

static void update(void)
{
	StoreCatalogue catalogue;
	char why[STORE_WHY_BYTES];
	const StoreSource source = store_catalogue_update(worker.config, &catalogue, why, sizeof why);
	if (source != STORE_NONE)
		store_installed(worker.config, &catalogue);
	xsdk_log_write("store: the catalogue, %s: %s", SOURCES[source], why);
	pthread_mutex_lock(&worker.lock);
	store_catalogue_free(&worker.catalogue);
	worker.catalogue = catalogue;
	worker.source = source;
	snprintf(worker.why, sizeof worker.why, "%s", why);
	worker.catalogue_in = true;
	pthread_mutex_unlock(&worker.lock);
}

// Runs a job on a copy of the app's entry, whose installed state it then reads afresh: the screens' thread alone writes
// the catalogue's.
static void run_job(StoreJobKind kind, size_t index)
{
	StoreJobResult result = { .kind = kind, .app = index };
	StoreApp app = worker.catalogue.apps[index];
	if (kind == STORE_JOB_INSTALL)
		result.done =
			store_install(worker.config, &worker.catalogue, &app, &worker.progress, result.why, sizeof result.why);
	else
		result.done = store_remove(worker.config, &app, kind == STORE_JOB_REMOVE_ALL, result.why, sizeof result.why);
	store_installed_app(worker.config, &app);
	result.installed = app.installed;
	result.installed_version = app.installed_version;
	result.receipt = app.receipt;
	result.receipt_author_same = app.receipt_author_same;
	result.other_files = store_other_files(worker.config, &app);
	xsdk_log_write("store: %s's %s: %s%s", app.id, JOBS[kind], result.done ? "done" : "failed: ", result.why);
	pthread_mutex_lock(&worker.lock);
	worker.result = result;
	worker.job_done = true;
	worker.job_running = false;
	pthread_mutex_unlock(&worker.lock);
}

static void *run(void *unused)
{
	(void)unused;
	store_recover(worker.config);
	pthread_mutex_lock(&worker.lock);
	for (;;) {
		while (!worker.stop && !worker.update && !worker.job_wanted && !(worker.shot_wanted && !worker.shot_done))
			pthread_cond_wait(&worker.wake, &worker.lock);
		if (worker.stop)
			break;
		if (worker.update) {
			worker.update = false;
			pthread_mutex_unlock(&worker.lock);
			update();
			pthread_mutex_lock(&worker.lock);
			continue;
		}
		if (worker.job_wanted) {
			worker.job_wanted = false;
			const StoreJobKind kind = worker.job_kind;
			const size_t app = worker.job_app;
			pthread_mutex_unlock(&worker.lock);
			run_job(kind, app);
			pthread_mutex_lock(&worker.lock);
			continue;
		}
		const size_t app = worker.wanted;
		worker.shot_wanted = false;
		pthread_mutex_unlock(&worker.lock);
		uint32_t *pixels = NULL;
		char why[STORE_WHY_BYTES];
		if (!store_shot(worker.config, &worker.catalogue, &worker.catalogue.apps[app], &pixels, why, sizeof why))
			xsdk_log_write("store: %s's screenshot: %s", worker.catalogue.apps[app].id, why);
		pthread_mutex_lock(&worker.lock);
		worker.shot_done = true;
		worker.done = app;
		worker.done_pixels = pixels;
	}
	pthread_mutex_unlock(&worker.lock);
	return NULL;
}

bool store_worker_start(const StoreConfig *config)
{
	memset(&worker, 0, sizeof worker);
	worker.config = config;
	worker.update = true;
	pthread_mutex_init(&worker.lock, NULL);
	pthread_cond_init(&worker.wake, NULL);
	return pthread_create(&worker.thread, NULL, run, NULL) == 0;
}

void store_worker_stop(void)
{
	pthread_mutex_lock(&worker.lock);
	worker.stop = true;
	pthread_cond_signal(&worker.wake);
	pthread_mutex_unlock(&worker.lock);
	pthread_join(worker.thread, NULL);
	free(worker.done_pixels);
	store_catalogue_free(&worker.catalogue);
	pthread_cond_destroy(&worker.wake);
	pthread_mutex_destroy(&worker.lock);
}

bool store_worker_catalogue(StoreCatalogue **catalogue, StoreSource *source, const char **why)
{
	pthread_mutex_lock(&worker.lock);
	const bool in = worker.catalogue_in;
	*catalogue = &worker.catalogue;
	*source = worker.source;
	*why = worker.why;
	pthread_mutex_unlock(&worker.lock);
	return in;
}

void store_worker_retry(void)
{
	pthread_mutex_lock(&worker.lock);
	worker.catalogue_in = false;
	worker.update = true;
	pthread_cond_signal(&worker.wake);
	pthread_mutex_unlock(&worker.lock);
}

void store_worker_want_shot(size_t app)
{
	pthread_mutex_lock(&worker.lock);
	worker.shot_wanted = true;
	worker.wanted = app;
	pthread_cond_signal(&worker.wake);
	pthread_mutex_unlock(&worker.lock);
}

bool store_worker_take_shot(size_t *app, uint32_t **pixels)
{
	pthread_mutex_lock(&worker.lock);
	const bool done = worker.shot_done;
	if (done) {
		*app = worker.done;
		*pixels = worker.done_pixels;
		worker.done_pixels = NULL;
		worker.shot_done = false;
		pthread_cond_signal(&worker.wake);
	}
	pthread_mutex_unlock(&worker.lock);
	return done;
}

bool store_worker_job(StoreJobKind kind, size_t app)
{
	pthread_mutex_lock(&worker.lock);
	const bool free_to_start = !worker.job_wanted && !worker.job_running && !worker.job_done;
	if (free_to_start) {
		worker.job_wanted = true;
		worker.job_running = true;
		worker.job_kind = kind;
		worker.job_app = app;
		worker.progress.step = kind == STORE_JOB_INSTALL ? STORE_STEP_DOWNLOADING : STORE_STEP_REMOVING;
		worker.progress.done_bytes = 0;
		worker.progress.total_bytes = 0;
		pthread_cond_signal(&worker.wake);
	}
	pthread_mutex_unlock(&worker.lock);
	return free_to_start;
}

bool store_worker_busy(size_t *app)
{
	pthread_mutex_lock(&worker.lock);
	const bool busy = worker.job_running;
	*app = worker.job_app;
	pthread_mutex_unlock(&worker.lock);
	return busy;
}

const StoreProgress *store_worker_progress(void)
{
	return &worker.progress;
}

bool store_worker_take_job(StoreJobResult *result)
{
	pthread_mutex_lock(&worker.lock);
	const bool done = worker.job_done;
	if (done) {
		*result = worker.result;
		worker.job_done = false;
	}
	pthread_mutex_unlock(&worker.lock);
	return done;
}
