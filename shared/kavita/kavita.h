/*
 * kavita.h
 *
 * Kavita integration interface for Inky.
 *
 * This module handles:
 *   - Loading the Kavita configuration (URL + OPDS API key)
 *   - Loading/saving the per-book metadata sidecar (.meta.json)
 *   - OPDS catalog browsing (libraries, series, volumes)
 *   - Downloading volumes from Kavita to local storage
 *
 * The PC implementation uses libcurl for HTTP and cJSON for JSON. The ESP32
 * port will replace those with the ESP HTTP client and a lighter JSON library.
 */

#ifndef KAVITA_H
#define KAVITA_H

#include <stddef.h>
#include <time.h>

/*
 * Kavita configuration loaded from an INI/TOML-like file.
 */
typedef struct {
    char *url;
    char *api_key;
} kavita_config_t;

/*
 * Load a Kavita config file.
 *
 * The file format is simple key=value pairs. Lines starting with '#' or '['
 * are ignored. Example:
 *
 *   [kavita]
 *   url = "http://kavita.home.lan:5000"
 *   api_key = "abc123..."
 *
 * Returns 1 on success, 0 on failure.
 */
int kavita_config_load(const char *path, kavita_config_t *cfg);

/*
 * Free memory allocated by kavita_config_load().
 */
void kavita_config_free(kavita_config_t *cfg);

/*
 * Return the default Kavita config path on this platform.
 * Caller must free the returned string.
 */
char *kavita_default_config_path(void);

/*
 * Kavita-side chapter ID mapping stored alongside a downloaded EPUB.
 */
typedef struct {
    int *ids;
    int count;
} kavita_chapter_map_t;

/*
 * Per-book metadata sidecar.
 */
typedef struct {
    int kavita_library_id;
    int kavita_series_id;
    int kavita_volume_id;
    char *title;
    time_t downloaded_at;
    kavita_chapter_map_t chapters;
} kavita_meta_t;

/*
 * Build the sidecar path for a given EPUB path.
 * Returns a heap-allocated string ending in ".meta.json", or NULL.
 * Caller must free.
 */
char *kavita_meta_path(const char *epub_path);

/*
 * Load the metadata sidecar for a given EPUB.
 * Returns 1 if found and valid, 0 otherwise.
 */
int kavita_meta_load(const char *epub_path, kavita_meta_t *meta);

/*
 * Save the metadata sidecar for a given EPUB.
 * Returns 1 on success, 0 on failure.
 */
int kavita_meta_save(const char *epub_path, const kavita_meta_t *meta);

/*
 * Free memory allocated by kavita_meta_load().
 */
void kavita_meta_free(kavita_meta_t *meta);

/*
 * Opaque Kavita client.
 */
typedef struct kavita_client kavita_client_t;

/*
 * Create a new client from a loaded config.
 * Returns NULL on failure.
 */
kavita_client_t *kavita_client_new(const kavita_config_t *cfg);

/*
 * Destroy a client and free all associated resources.
 */
void kavita_client_free(kavita_client_t *client);

/*
 * Get the underlying configuration (read-only).
 */
const kavita_config_t *kavita_client_config(const kavita_client_t *client);

/*
 * Test whether the client can reach Kavita.
 * Returns 1 if reachable, 0 otherwise.
 */
int kavita_client_ping(kavita_client_t *client);

/*
 * One library returned by the OPDS catalog.
 */
typedef struct {
    int library_id;
    char *name;
} kavita_library_t;

/*
 * One series returned by the OPDS catalog.
 */
typedef struct {
    int series_id;
    char *name;
} kavita_series_t;

/*
 * One volume returned by the OPDS catalog.
 *
 * The acquisition link points to the downloadable file. kavita_volume_id and
 * kavita_chapter_id are parsed from that link.
 */
typedef struct {
    int volume_id;
    int chapter_id;
    char *title;
    char *download_url;  /* Relative URL, ready to be prefixed with cfg->url. */
} kavita_volume_t;

/*
 * Progress callback for downloads.
 *
 * `current` is the number of bytes received so far.
 * `total` is the expected total size, or 0 if the server did not send one.
 * `user_data` is the pointer passed to the download call.
 */
typedef void (*kavita_download_progress_t)(size_t current, size_t total, void *user_data);

/*
 * Fetch the list of libraries from Kavita.
 *
 * Returns the number of libraries, or -1 on error. The caller must free the
 * array with kavita_libraries_free().
 */
int kavita_client_fetch_libraries(kavita_client_t *client,
                                  kavita_library_t **libraries);

/*
 * Fetch the list of series in a library.
 *
 * Returns the number of series, or -1 on error. The caller must free the array
 * with kavita_series_free().
 */
int kavita_client_fetch_series(kavita_client_t *client, int library_id,
                               kavita_series_t **series);

/*
 * Fetch the list of volumes in a series.
 *
 * Returns the number of volumes, or -1 on error. The caller must free the
 * array with kavita_volumes_free().
 */
int kavita_client_fetch_volumes(kavita_client_t *client, int series_id,
                                kavita_volume_t **volumes);

/*
 * Download a volume to `out_path`.
 *
 * The volume must have been returned by kavita_client_fetch_volumes().
 * `progress` may be NULL.
 *
 * Returns 1 on success, 0 on failure.
 */
int kavita_client_download_volume(kavita_client_t *client,
                                  const kavita_volume_t *volume,
                                  const char *out_path,
                                  kavita_download_progress_t progress,
                                  void *user_data);

/*
 * Fetch the ordered list of Kavita chapter IDs for a volume.
 *
 * Returns the number of chapters, or -1 on error. The caller must free the
 * array with free().
 */
int kavita_client_fetch_volume_chapters(kavita_client_t *client,
                                        int series_id, int volume_id,
                                        int **chapter_ids);

/*
 * Get the stored progress for a single Kavita chapter.
 *
 * Returns 1 if the server returned progress, 0 otherwise (including network
 * errors and chapters with no progress). `page_num` is only written on success.
 */
int kavita_client_get_progress(kavita_client_t *client, int chapter_id,
                               int *page_num,
                               char *updated_at, size_t updated_at_size);

/*
 * Save progress for a single Kavita chapter.
 *
 * `updated_at` may be NULL; the current UTC time is used in that case.
 * Returns 1 on success, 0 on failure.
 */
int kavita_client_set_progress(kavita_client_t *client, int chapter_id,
                               int page_num,
                               int volume_id, int series_id, int library_id,
                               const char *updated_at);

/*
 * High-level open/close sync helpers.
 *
 * These load the local sidecar and config, talk to Kavita, and merge local
 * and remote progress using the `updated_at` timestamp. They are safe to call
 * on any book path; they return 0 silently when the book has no sidecar or
 * no config, so callers do not need to check first.
 *
 * kavita_sync_on_open loads local progress, fetches remote progress for the
 * current chapter, writes the merged result back to local storage, and fills
 * chapter/page/updated_at with the position to use.
 *
 * kavita_sync_on_close pushes the given chapter/page to Kavita immediately on
 * PC. On ESP32 this will connect Wi-Fi on demand, push, then disconnect, and
 * queue the update if the push fails.
 */
int kavita_sync_on_open(const char *epub_path,
                        int *chapter, int *page,
                        char *updated_at, size_t updated_at_size);
int kavita_sync_on_close(const char *epub_path, int chapter, int page);

/*
 * Delete a downloaded Kavita book, removing both the EPUB and its sidecar.
 * Returns 1 on success, 0 on failure.
 */
int kavita_delete_book(const char *epub_path);

/*
 * Free arrays returned by the catalog fetch functions.
 */
void kavita_libraries_free(kavita_library_t *libraries, int count);
void kavita_series_free(kavita_series_t *series, int count);
void kavita_volumes_free(kavita_volume_t *volumes, int count);

#endif
