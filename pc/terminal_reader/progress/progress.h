/*
 * progress.h
 *
 * Reading progress persistence.
 *
 * For each EPUB we store a small sidecar file named:
 *   <epub_path>.inky-progress
 *
 * The file contains key=value pairs, currently:
 *   chapter=<chapter_index>
 *   page=<page_index>
 *
 * This keeps the format trivially parseable on both PC and ESP32 without
 * pulling in a JSON library.
 */

#ifndef PROGRESS_H
#define PROGRESS_H

/*
 * Load the saved chapter and page for a given EPUB path.
 * Returns 1 if a valid progress file was found, 0 otherwise.
 */
int progress_load(const char *epub_path, int *chapter, int *page);

/*
 * Save the current chapter and page for a given EPUB path.
 * Returns 1 on success, 0 on failure.
 */
int progress_save(const char *epub_path, int chapter, int page);

#endif
