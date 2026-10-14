/*
 * Inky terminal EPUB reader - prototype step 5.
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

#include "book_list/book_list.h"
#include "books_dir/books_dir.h"
#include "reader/reader.h"
#include "terminal/terminal.h"

/*
 * User data passed to the layout render callback.
 */
typedef struct {
    const reader_session_t *session;
    int total_pages;
} render_ctx_t;

/*
 * Layout render callback for the terminal.
 */
static void render_line(int line_number, const char *line_text,
                        int is_status, void *user_data)
{
    render_ctx_t *ctx = (render_ctx_t *)user_data;
    const reader_session_t *session = ctx->session;

    if (is_status) {
        int chapter_pct = ctx->total_pages > 0
                              ? ((session->current_page + 1) * 100) / ctx->total_pages
                              : 0;
        int book_pct = session->epub->chapter_count > 0
                           ? ((session->current_chapter + 1) * 100) / session->epub->chapter_count
                           : 0;

        /*
         * Two status lines at the very bottom. line_number == -2 is the
         * first status line (book progress), -1 is the last line (chapter
         * progress). Use 1-based ANSI row coordinates.
         */
        int status_row;
        if (line_number == -2) {
            status_row = session->rows - 1;
        } else {
            status_row = session->rows;
        }

        printf("\033[%d;1H\033[7m", status_row);

        if (line_number == -2) {
            printf(" Book %d / %d  (%d%%) ",
                   session->current_chapter + 1, session->epub->chapter_count,
                   book_pct);
        } else {
            printf(" %s  %d / %d pages (%d%%) ",
                   session->epub->chapters[session->current_chapter].idref,
                   session->current_page + 1, ctx->total_pages,
                   chapter_pct);
        }

        printf("\033[0m");

        /*
         * The first status line (line_number == -2) is followed by a newline
         * so the second status line is drawn on the next row. The second
         * status line (line_number == -1) is positioned at the bottom row
         * with an absolute ANSI cursor command, so it must not emit a newline
         * (that would scroll the screen).
         */
        if (line_number == -2) {
            printf("\n");
        }
    } else {
        printf("%s\n", line_text);
    }
}

/*
 * Render the current page to the terminal.
 */
static void reader_render(reader_session_t *session)
{
    if (!session->layout) {
        return;
    }

    /* Clear and home cursor before drawing the full screen. */
    terminal_clear();

    int content_rows = session->rows - session->status_rows;
    int total_pages = reader_page_count(session);

    render_ctx_t ctx = {
        .session = session,
        .total_pages = total_pages,
    };

    layout_render_page(session->layout, session->current_page, content_rows,
                       render_line, &ctx);

    /* Status lines at the bottom. */
    render_line(-2, "", 1, &ctx);
    render_line(-1, "", 1, &ctx);

    fflush(stdout);
}

/*
 * Run the reading loop for one book. Returns when the user closes the book.
 */
static void run_reader(reader_session_t *session, const char *path)
{
    if (!reader_open(session, path)) {
        terminal_clear();
        printf("[Could not open %s]\n", path);
        terminal_read_key();
        return;
    }

    session->status_rows = 2;
    reader_relayout(session);

    int reading = 1;
    while (reading) {
        /* Re-render before reading input so resizes take effect. */
        reader_render(session);

        int key = terminal_read_key();

        switch (key) {
        case KEY_QUIT:
            reading = 0;
            break;
        case KEY_LEFT:
            reader_navigate(session, -1);
            break;
        case KEY_RIGHT:
            reader_navigate(session, 1);
            break;
        default:
            break;
        }

        /* Handle terminal resize. */
        int new_rows, new_cols;
        if (terminal_get_size(&new_rows, &new_cols)) {
            if (new_rows != session->rows || new_cols != session->cols) {
                session->rows = new_rows;
                session->cols = new_cols;
                reader_relayout(session);
            }
        }
    }

    reader_close(session);
}

/*
 * Render the book list to the terminal.
 */
static void render_book_list(const book_entry_t *entries, int count,
                             int selected, int rows)
{
    terminal_clear();
    printf("Inky - Select a book\n");
    printf("---------------------\n\n");

    for (int i = 0; i < count; i++) {
        char line[512];
        if (entries[i].has_progress) {
            snprintf(line, sizeof(line), "%s - %d%%",
                     entries[i].filename, entries[i].progress_pct);
        } else {
            snprintf(line, sizeof(line), "%s", entries[i].filename);
        }

        if (i == selected) {
            printf("\033[7m > %s\033[0m\n", line);
        } else {
            printf("   %s\n", line);
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
    (void)cols;

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
        render_book_list(entries, count, selected, rows);

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
    reader_session_t session = {0};

    reader_init(&session);

    if (!terminal_get_size(&session.rows, &session.cols)) {
        session.rows = 24;
        session.cols = 80;
    }

    if (session.rows < 4 || session.cols < 20) {
        fprintf(stderr, "Terminal is too small (need at least 4x20)\n");
        return 1;
    }

    session.status_rows = 2;
    terminal_setup_raw();

    char *book_path = NULL;

    if (argc >= 2) {
        book_path = strdup(argv[1]);
    } else {
        book_path = run_book_list(session.rows, session.cols);
    }

    while (book_path != NULL) {
        run_reader(&session, book_path);
        free(book_path);

        /* After closing a book, return to the list. */
        book_path = run_book_list(session.rows, session.cols);
    }

    reader_free(&session);

    terminal_clear();
    printf("Goodbye.\n");

    return 0;
}
