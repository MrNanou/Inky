/*
 * reader.h
 *
 * Platform-independent reading session state. Manages an open EPUB, the
 * current chapter/page, and navigation across chapter boundaries.
 *
 * Frontends (terminal, SDL, ESP32) create a reader_session_t, open a book,
 * and call reader_navigate() to move forward/backward. They provide their own
 * rendering.
 */

#ifndef READER_H
#define READER_H

#include "../epub/epub.h"
#include "../layout/layout.h"

typedef struct {
    epub_t *epub;
    char *book_path;

    int current_chapter;
    int current_page;

    char *chapter_text;
    layout_t *layout;

    int cols;  /* Layout columns, set by the frontend. */
} reader_session_t;

/*
 * Initialize a reader session. Does not allocate an EPUB.
 */
void reader_init(reader_session_t *session);

/*
 * Free all resources held by a session.
 */
void reader_free(reader_session_t *session);

/*
 * Open a book by path. If a progress file exists, resume from it; otherwise
 * start at the first spine entry. Returns 1 on success, 0 on failure.
 */
int reader_open(reader_session_t *session, const char *path);

/*
 * Close the current book and save progress.
 */
void reader_close(reader_session_t *session);

/*
 * Rebuild the layout for the current chapter. Call after the frontend's
 * column count changes (e.g. terminal resize, font change).
 */
void reader_relayout(reader_session_t *session);

/*
 * Navigate one page. delta > 0 moves forward, delta < 0 moves backward.
 * Automatically crosses chapter boundaries.
 */
void reader_navigate(reader_session_t *session, int delta);

/*
 * Return the number of pages in the current chapter, given the number of
 * content rows available. The frontend decides how many rows to reserve for
 * status/chrome.
 */
int reader_page_count(const reader_session_t *session, int content_rows);

#endif
