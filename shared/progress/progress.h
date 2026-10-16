#include <stddef.h>
/*
 * progress.h
 *
 * Reading progress persistence.
 *
 * Progress is stored in a central location rather than as a sidecar next to
 * the EPUB file. On Linux we use $XDG_STATE_HOME/inky/progress/ (falling back
 * to ~/.local/state/inky/progress/). Each EPUB gets a file named after a
 * simple hash of its full path. The file contents include the original EPUB
 * path so that `progress_load` can verify it is loading the right record.
 *
 * The file format is trivially parseable key=value pairs:
 *   path=<full epub path>
 *   chapter=<chapter_index>
 *   page=<page_index>
 *
 * This avoids a JSON dependency for both PC and ESP32 ports.
 */

#ifndef PROGRESS_H
#define PROGRESS_H

/*
 * Load the saved chapter and page for a given EPUB path.
 * Returns 1 if a valid progress file was found *and* its stored path matches
 * epub_path, 0 otherwise.
 */
int progress_load(const char *epub_path, int *chapter, int *page);

/*
 * Save the current chapter and page for a given EPUB path.
 * Returns 1 on success, 0 on failure.
 */
int progress_save(const char *epub_path, int chapter, int page);

/*
 * Extended variants that also read/write an ISO-8601 updated_at timestamp.
 * Pass updated_at == NULL / updated_at_size == 0 to ignore the timestamp.
 */
int progress_load_full(const char *epub_path, int *chapter, int *page,
                       char *updated_at, size_t updated_at_size);
int progress_save_full(const char *epub_path, int chapter, int page,
                       const char *updated_at);

#endif
