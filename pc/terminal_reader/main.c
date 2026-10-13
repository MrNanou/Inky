/*
 * Inky terminal EPUB reader - prototype step 4.
 *
 * Launched without arguments, shows a selectable list of EPUBs in Books/.
 * Launched with a book path, opens it directly.
 *
 * Book list:
 *   Up/Down arrows  -> move selection
 *   Enter           -> open selected book
 *   q               -> quit
 *
 * Reader:
 *   Left/Right arrows  -> previous/next page
 *   At chapter boundaries, arrows continue into the previous/next chapter
 *   q                  -> close book and return to book list
 *
 * The layout engine is intentionally abstracted in rows and columns so it can
 * be reused later with SDL2 and on the ESP32 e-ink display.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "books_dir/books_dir.h"
#include "book_list/book_list.h"
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
    int rows;
    int cols;
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
        int chapter_pct = ctx->total_pages > 0
                              ? ((ctx->page + 1) * 100) / ctx->total_pages
                              : 0;
        int book_pct = ctx->chapter_count > 0
                           ? ((ctx->chapter_index + 1) * 100) / ctx->chapter_count
                           : 0;

        /*
         * Print status lines at the bottom of the screen using explicit cursor
         * positioning. Line -1 is the second-to-last row, line -2 is the last.
         */
        int status_row = (line_number == -1) ? ctx->rows - 1 : ctx->rows;
        printf("\033[%d;1H\033[7m", status_row);

        if (line_number == -1) {
            printf(" Book %d / %d  (%d%%) ",
                   ctx->chapter_index + 1, ctx->chapter_count,
                   book_pct);
        } else {
            printf(" %s  %d / %d pages (%d%%) ",
                   ctx->chapter_name,
                   ctx->page + 1, ctx->total_pages,
                   chapter_pct);
        }

        printf("\033[0m");

        /*
         * Only the very last status line omits the newline so the cursor stays
         * on the bottom row and the terminal does not scroll past the screen.
         */
        if (line_number == -1) {
            printf("\n");
        }
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

    /* Clear and home cursor before drawing the full screen. */
    terminal_clear();

    render_ctx_t ctx = {
        .chapter_name = reader->epub->chapters[reader->current_chapter].idref,
        .chapter_index = reader->current_chapter,
        .chapter_count = reader->epub->chapter_count,
        .page = reader->current_page,
        .total_pages = layout_page_count(reader->layout, reader->rows),
        .rows = reader->rows,
        .cols = reader->cols,
    };

    layout_render_page(reader->layout, reader->current_page, reader->rows,
                       render_line, &ctx);

    fflush(stdout);
}

/*
 * Run the reading loop for one book. Returns when the user closes the book.
 */
static void run_reader(reader_t *reader, const char *path)
{
    reader->epub = epub_open(path);
    if (!reader->epub) {
        terminal_clear();
        printf("[Could not open %s]\n", path);
        terminal_read_key();
        return;
    }

    /*
     * Try to resume from saved progress. If no progress file exists, start
     * at the first entry of the book's spine.
     */
    if (!progress_load(path, &reader->current_chapter, &reader->current_page)) {
        reader->current_chapter = 0;
        reader->current_page = 0;
    }

    /* Clamp loaded values to valid ranges in case the EPUB changed. */
    if (reader->current_chapter < 0) {
        reader->current_chapter = 0;
    }
    if (reader->current_chapter >= reader->epub->chapter_count) {
        reader->current_chapter = reader->epub->chapter_count - 1;
    }

    reader_load_chapter(reader, reader->current_chapter);

    /*
     * After loading the chapter, clamp the page. This matters when resuming
     * because the saved page may no longer exist after a terminal resize.
     */
    if (reader->layout) {
        int pages = layout_page_count(reader->layout, reader->rows);
        if (reader->current_page >= pages) {
            reader->current_page = pages > 0 ? pages - 1 : 0;
        }
    }

    int reading = 1;
    while (reading) {
        /* Re-render before reading input so resizes take effect. */
        reader_render(reader);

        int key = terminal_read_key();

        switch (key) {
        case KEY_QUIT:
            reading = 0;
            break;
        case KEY_LEFT:
            if (reader->current_page > 0) {
                reader->current_page--;
            } else if (reader->current_chapter > 0) {
                /* Move to the last page of the previous chapter. */
                reader->current_chapter--;
                reader_load_chapter(reader, reader->current_chapter);
                reader->current_page = layout_page_count(reader->layout, reader->rows) - 1;
                if (reader->current_page < 0) {
                    reader->current_page = 0;
                }
            }
            break;
        case KEY_RIGHT:
            if (reader->layout &&
                reader->current_page < layout_page_count(reader->layout, reader->rows) - 1) {
                reader->current_page++;
            } else if (reader->current_chapter < reader->epub->chapter_count - 1) {
                /* Move to the first page of the next chapter. */
                reader->current_chapter++;
                reader_load_chapter(reader, reader->current_chapter);
            }
            break;
        default:
            break;
        }

        /* Handle terminal resize. */
        int new_rows, new_cols;
        if (terminal_get_size(&new_rows, &new_cols)) {
            if (new_rows != reader->rows || new_cols != reader->cols) {
                reader->rows = new_rows;
                reader->cols = reader->cols;
                reader_relayout(reader);
            }
        }
    }

    /*
     * Save progress before closing the book.
     */
    progress_save(path, reader->current_chapter, reader->current_page);

    layout_free(reader->layout);
    free(reader->chapter_text);
    epub_close(reader->epub);
    reader->layout = NULL;
    reader->chapter_text = NULL;
    reader->epub = NULL;
}

/*
 * Render the book list to the terminal.
 */
static void render_book_list(const book_entry_t *entries, int count,
                             int selected, int rows, int cols)
{
    (void)cols;

    terminal_clear();
    printf("Inky - Select a book\n");
    printf("---------------------\n\n");

    for (int i = 0; i < count; i++) {
        if (i == selected) {
            printf("\033[7m > %s\033[0m\n", entries[i].filename);
        } else {
            printf("   %s\n", entries[i].filename);
        }
    }

    if (count == 0) {
        const char *books_dir = find_books_dir();
        printf("No .epub files found in %s/\n", books_dir ? books_dir : "Books");
    }

    printf("\n");
    printf("Up/Down: select  Enter: open  q: quit\n");

    /* Pad to keep the screen stable. */
    int used = 5 + count + 2;
    for (int i = used; i < rows; i++) {
        printf("\n");
    }

    fflush(stdout);
}

/*
 * Run the book selection loop. Returns the selected book path, or NULL if the
 * user quits. The returned string must be freed by the caller.
 */
static char *run_book_list(int rows, int cols)
{
    const char *books_dir = find_books_dir();
    if (!books_dir) {
        terminal_clear();
        printf("Could not find Books/ directory\n");
        return NULL;
    }

    book_entry_t *entries = NULL;
    int count = book_list_scan(books_dir, &entries);

    if (count < 0) {
        terminal_clear();
        printf("Could not read %s/ directory\n", books_dir);
        return NULL;
    }

    int selected = 0;
    int running = 1;
    char *result = NULL;

    while (running) {
        render_book_list(entries, count, selected, rows, cols);

        int key = terminal_read_key();
        switch (key) {
        case KEY_QUIT:
            running = 0;
            break;
        case KEY_UP:
            if (selected > 0) {
                selected--;
            }
            break;
        case KEY_DOWN:
            if (selected < count - 1) {
                selected++;
            }
            break;
        case KEY_ENTER:
            if (count > 0) {
                result = strdup(entries[selected].path);
                running = 0;
            }
            break;
        default:
            break;
        }
    }

    book_list_free(entries, count);
    return result;
}

int main(int argc, char **argv)
{
    reader_t reader = {0};

    if (!terminal_get_size(&reader.rows, &reader.cols)) {
        reader.rows = 24;
        reader.cols = 80;
    }

    if (reader.rows < 4 || reader.cols < 20) {
        fprintf(stderr, "Terminal is too small (need at least 4x20)\n");
        return 1;
    }

    terminal_setup_raw();

    char *book_path = NULL;

    if (argc >= 2) {
        book_path = strdup(argv[1]);
    } else {
        book_path = run_book_list(reader.rows, reader.cols);
    }

    while (book_path != NULL) {
        run_reader(&reader, book_path);
        free(book_path);

        /* After closing a book, return to the list. */
        book_path = run_book_list(reader.rows, reader.cols);
    }

    terminal_clear();
    printf("Goodbye.\n");

    return 0;
}
