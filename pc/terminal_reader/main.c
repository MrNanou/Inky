/*
 * Inky terminal EPUB reader - prototype step 3.
 *
 * Goal: open an EPUB and read it paginated inside the terminal.
 *
 * Navigation:
 *   Left/Right arrows  -> previous/next page
 *   At chapter boundaries, arrows continue into the previous/next chapter
 *   q                  -> quit
 *
 * The layout engine is intentionally abstracted in rows and columns so it can
 * be reused later with SDL2 (where one cell maps to font metrics) and on the
 * ESP32 e-ink display.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epub/epub.h"
#include "html/html.h"
#include "layout/layout.h"
#include "progress/progress.h"
#include "terminal/terminal.h"

/*
 * Reader state. Groups the EPUB, the current chapter, the terminal size, and
 * the current chapter's text/layout so helper functions don't need a dozen
 * parameters.
 */
typedef struct {
    epub_t *epub;
    int current_chapter;
    int current_page;

    /* Screen dimensions. */
    int rows;
    int cols;

    /* Current chapter content. */
    char *chapter_text;
    layout_t *layout;
} reader_t;

/*
 * Load a chapter by index into the reader state. Frees the previous chapter's
 * text and layout, reads the new HTML, converts it to plain text, and builds a
 * fresh layout.
 */
static void reader_load_chapter(reader_t *reader, int index)
{
    layout_free(reader->layout);
    reader->layout = NULL;
    free(reader->chapter_text);
    reader->chapter_text = NULL;
    reader->current_page = 0;

    if (index < 0 || index >= reader->epub->chapter_count) {
        return;
    }

    const char *path = reader->epub->chapters[index].path;
    size_t chapter_len;
    char *chapter_html = epub_read_file(reader->epub, path, &chapter_len);
    if (!chapter_html) {
        terminal_clear();
        printf("[Could not read chapter]\n");
        return;
    }

    /*
     * Allocate a text buffer. Plain text after stripping tags is always
     * shorter than the original HTML, so HTML length + padding is safe.
     */
    size_t text_size = chapter_len + 4096;
    reader->chapter_text = malloc(text_size);
    if (!reader->chapter_text) {
        free(chapter_html);
        terminal_clear();
        printf("[Out of memory]\n");
        return;
    }

    html_to_text(chapter_html, reader->chapter_text, text_size);
    free(chapter_html);

    reader->layout = layout_new();
    if (!reader->layout || !layout_build(reader->layout, reader->chapter_text,
                                         reader->cols)) {
        layout_free(reader->layout);
        reader->layout = NULL;
        terminal_clear();
        printf("[Layout failed]\n");
    }
}

/*
 * Rebuild the current chapter layout after a terminal resize.
 */
static void reader_relayout(reader_t *reader)
{
    if (!reader->chapter_text) {
        return;
    }

    layout_free(reader->layout);
    reader->layout = layout_new();
    if (reader->layout) {
        layout_build(reader->layout, reader->chapter_text, reader->cols);
        int pages = layout_page_count(reader->layout, reader->rows);
        if (reader->current_page >= pages) {
            reader->current_page = pages > 0 ? pages - 1 : 0;
        }
    }
}

/*
 * User data passed to the layout render callback.
 */
typedef struct {
    const char *chapter_name;
    int chapter_index;
    int chapter_count;
    int page;
    int total_pages;
} render_ctx_t;

/*
 * Layout render callback for the terminal.
 */
static void render_line(int line_number, const char *line_text,
                        int is_status, void *user_data)
{
    (void)line_number;

    render_ctx_t *ctx = (render_ctx_t *)user_data;

    if (is_status) {
        printf("\033[7m %s  |  page %d / %d  |  chapter %d / %d \033[0m\n",
               ctx->chapter_name,
               ctx->page + 1, ctx->total_pages,
               ctx->chapter_index + 1, ctx->chapter_count);
    } else {
        printf("%s\n", line_text);
    }
}

/*
 * Render the current page to the terminal.
 */
static void reader_render(reader_t *reader)
{
    if (!reader->layout) {
        return;
    }

    terminal_clear();

    render_ctx_t ctx = {
        .chapter_name = reader->epub->chapters[reader->current_chapter].idref,
        .chapter_index = reader->current_chapter,
        .chapter_count = reader->epub->chapter_count,
        .page = reader->current_page,
        .total_pages = layout_page_count(reader->layout, reader->rows),
    };

    layout_render_page(reader->layout, reader->current_page, reader->rows,
                       render_line, &ctx);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <book.epub>\n", argv[0]);
        return 1;
    }

    reader_t reader = {0};

    if (!terminal_get_size(&reader.rows, &reader.cols)) {
        reader.rows = 24;
        reader.cols = 80;
    }

    if (reader.rows < 3 || reader.cols < 20) {
        fprintf(stderr, "Terminal is too small (need at least 3x20)\n");
        return 1;
    }

    reader.epub = epub_open(argv[1]);
    if (!reader.epub) {
        return 1;
    }

    /*
     * Try to resume from saved progress. If no progress file exists, start
     * at the first entry of the book's spine.
     */
    if (!progress_load(argv[1], &reader.current_chapter, &reader.current_page)) {
        reader.current_chapter = 0;
        reader.current_page = 0;
    }

    /* Clamp loaded values to valid ranges in case the EPUB changed. */
    if (reader.current_chapter < 0) {
        reader.current_chapter = 0;
    }
    if (reader.current_chapter >= reader.epub->chapter_count) {
        reader.current_chapter = reader.epub->chapter_count - 1;
    }

    terminal_setup_raw();
    reader_load_chapter(&reader, reader.current_chapter);

    /*
     * After loading the chapter, clamp the page. This matters when resuming
     * because the saved page may no longer exist after a terminal resize.
     */
    if (reader.layout) {
        int pages = layout_page_count(reader.layout, reader.rows);
        if (reader.current_page >= pages) {
            reader.current_page = pages > 0 ? pages - 1 : 0;
        }
    }

    int running = 1;
    while (running) {
        /* Re-render before reading input so resizes take effect. */
        reader_render(&reader);

        int key = terminal_read_key();

        switch (key) {
        case KEY_QUIT:
            running = 0;
            break;
        case KEY_LEFT:
            if (reader.current_page > 0) {
                reader.current_page--;
            } else if (reader.current_chapter > 0) {
                /* Move to the last page of the previous chapter. */
                reader.current_chapter--;
                reader_load_chapter(&reader, reader.current_chapter);
                reader.current_page = layout_page_count(reader.layout, reader.rows) - 1;
                if (reader.current_page < 0) {
                    reader.current_page = 0;
                }
            }
            break;
        case KEY_RIGHT:
            if (reader.layout &&
                reader.current_page < layout_page_count(reader.layout, reader.rows) - 1) {
                reader.current_page++;
            } else if (reader.current_chapter < reader.epub->chapter_count - 1) {
                /* Move to the first page of the next chapter. */
                reader.current_chapter++;
                reader_load_chapter(&reader, reader.current_chapter);
            }
            break;
        default:
            break;
        }

        /* Handle terminal resize. */
        int new_rows, new_cols;
        if (terminal_get_size(&new_rows, &new_cols)) {
            if (new_rows != reader.rows || new_cols != reader.cols) {
                reader.rows = new_rows;
                reader.cols = new_cols;
                reader_relayout(&reader);
            }
        }
    }

    /*
     * Save progress before exiting. We write the current chapter and page so
     * the next time this EPUB is opened we can resume reading here.
     */
    progress_save(argv[1], reader.current_chapter, reader.current_page);

    terminal_clear();
    printf("Goodbye.\n");

    layout_free(reader.layout);
    free(reader.chapter_text);
    epub_close(reader.epub);

    return 0;
}
