/*
 * kavita.h
 *
 * Kavita integration interface for Inky.
 *
 * This module handles:
 *   - Loading the Kavita configuration (URL + OPDS API key)
 *   - Loading/saving the per-book metadata sidecar (.meta.json)
 *   - A client skeleton for OPDS catalog browsing and progress sync
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
    int kavita_volume_id;
    int kavita_series_id;
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

#endif
