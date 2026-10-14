/*
 * reader.c
 *
 * Platform-independent reading session implementation.
 */

#include "reader/reader.h"

#include "../epub/epub.h"
#include "../html/html.h"
#include "../layout/layout.h"
#include "../progress/progress.h"

#include <stdlib.h>
#include <string.h>

void reader_init(reader_session_t *session)
{
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->status_rows = 2;
}

void reader_free(reader_session_t *session)
{
    if (!session) {
        return;
    }

    layout_free(session->layout);
    free(session->chapter_text);
    free(session->book_path);
    epub_close(session->epub);

    session->layout = NULL;
    session->chapter_text = NULL;
    session->book_path = NULL;
    session->epub = NULL;
}

/*
 * Load a chapter by index into the session. Frees the previous chapter's
 * text/layout, reads the new HTML, converts it to plain text, and builds a
 * fresh layout.
 */
static int reader_load_chapter(reader_session_t *session, int index)
{
    layout_free(session->layout);
    session->layout = NULL;
    free(session->chapter_text);
    session->chapter_text = NULL;
    session->current_page = 0;

    if (!session->epub || index < 0 || index >= session->epub->chapter_count) {
        return 0;
    }

    const char *path = session->epub->chapters[index].path;
    size_t chapter_len;
    char *chapter_html = epub_read_file(session->epub, path, &chapter_len);
    if (!chapter_html) {
        return 0;
    }

    /*
     * Plain text after stripping tags is always shorter than the original
     * HTML, so HTML length + padding is safe.
     */
    size_t text_size = chapter_len + 4096;
    session->chapter_text = malloc(text_size);
    if (!session->chapter_text) {
        free(chapter_html);
        return 0;
    }

    html_to_text(chapter_html, session->chapter_text, text_size);
    free(chapter_html);

    session->layout = layout_new();
    if (session->layout) {
        layout_build(session->layout, session->chapter_text, session->cols);
    }

    return session->layout != NULL && session->layout->line_count > 0;
}

int reader_open(reader_session_t *session, const char *path)
{
    if (!session || !path) {
        return 0;
    }

    reader_free(session);

    /*
     * Normalize the book path so progress is stable regardless of how the
     * book is opened (CLI absolute path, relative path, or Books/ selection).
     */
    char *normalized = realpath(path, NULL);
    const char *book_path = normalized ? normalized : path;

    session->epub = epub_open(book_path);
    if (!session->epub) {
        free(normalized);
        return 0;
    }

    session->book_path = strdup(book_path);
    free(normalized);

    if (!progress_load(path, &session->current_chapter, &session->current_page)) {
        session->current_chapter = 0;
        session->current_page = 0;
    }

    if (session->current_chapter < 0) {
        session->current_chapter = 0;
    }
    if (session->current_chapter >= session->epub->chapter_count) {
        session->current_chapter = session->epub->chapter_count - 1;
    }

    reader_load_chapter(session, session->current_chapter);
    if (session->layout) {
        int pages = reader_page_count(session);
        if (session->current_page >= pages) {
            session->current_page = pages > 0 ? pages - 1 : 0;
        }
    }

    return 1;
}

void reader_close(reader_session_t *session)
{
    if (!session) {
        return;
    }

    if (session->epub && session->book_path) {
        progress_save(session->book_path, session->current_chapter, session->current_page);
    }

    layout_free(session->layout);
    free(session->chapter_text);
    free(session->book_path);
    epub_close(session->epub);

    session->layout = NULL;
    session->chapter_text = NULL;
    session->book_path = NULL;
    session->epub = NULL;
    session->current_chapter = 0;
    session->current_page = 0;
}

void reader_relayout(reader_session_t *session)
{
    if (!session || !session->chapter_text) {
        return;
    }

    layout_free(session->layout);
    session->layout = layout_new();
    if (session->layout) {
        layout_build(session->layout, session->chapter_text, session->cols);
        int pages = reader_page_count(session);
        if (session->current_page >= pages) {
            session->current_page = pages > 0 ? pages - 1 : 0;
        }
    }
}

void reader_navigate(reader_session_t *session, int delta)
{
    if (!session || !session->epub || !session->layout || delta == 0) {
        return;
    }

    int pages = reader_page_count(session);

    if (delta > 0) {
        if (session->current_page < pages - 1) {
            session->current_page++;
        } else if (session->current_chapter < session->epub->chapter_count - 1) {
            session->current_chapter++;
            if (!reader_load_chapter(session, session->current_chapter)) {
                session->chapter_load_failed = 1;
            }
        }
    } else {
        if (session->current_page > 0) {
            session->current_page--;
        } else if (session->current_chapter > 0) {
            session->current_chapter--;
            if (!reader_load_chapter(session, session->current_chapter)) {
                session->chapter_load_failed = 1;
            }
            session->current_page = reader_page_count(session) - 1;
            if (session->current_page < 0) {
                session->current_page = 0;
            }
        }
    }
}

int reader_page_count(const reader_session_t *session)
{
    if (!session || !session->layout) {
        return 0;
    }
    int content_rows = session->rows - session->status_rows;
    if (content_rows <= 0) {
        content_rows = 1;
    }
    return layout_page_count(session->layout, content_rows);
}
