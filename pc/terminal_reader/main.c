/*
 * Inky terminal EPUB reader - prototype step 6.
 *
 * Launched without arguments, shows a selectable list of EPUBs in Books/ plus
 * a [Kavita] entry for downloading books from a self-hosted Kavita server.
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
#include "battery/battery.h"
#include "kavita/kavita.h"
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

        char status[256];
        if (line_number == -2) {
            const char *battery = battery_string(session->battery_pct);
            snprintf(status, sizeof(status),
                     "Book %d / %d  (%d%%)  Bat: %s",
                     session->current_chapter + 1, session->epub->chapter_count,
                     book_pct, battery);
        } else {
            snprintf(status, sizeof(status),
                     "%s  %d / %d pages (%d%%)",
                     session->epub->chapters[session->current_chapter].idref,
                     session->current_page + 1, ctx->total_pages,
                     chapter_pct);
        }

        /*
         * Keep the status bar from overflowing and overwriting the content
         * area when the terminal is narrow.
         */
        int max_len = session->cols > 2 ? session->cols - 2 : 1;
        size_t status_len = strlen(status);
        if ((int)status_len > max_len) {
            status[max_len] = '\0';
        }

        printf(" %s ", status);

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

    session->status_rows = 3;
    session->battery_pct = 100;  /* Placeholder on PC; ESP32 reads ADC. */
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
 * Return the path to the Kavita config file.
 * Caller must free the returned string.
 */
static char *kavita_config_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        size_t len = strlen(xdg) + strlen("/inky/kavita.toml") + 1;
        char *path = malloc(len);
        if (path) {
            snprintf(path, len, "%s/inky/kavita.toml", xdg);
        }
        return path;
    }

    const char *home = getenv("HOME");
    if (!home) {
        return NULL;
    }

    size_t len = strlen(home) + strlen("/.config/inky/kavita.toml") + 1;
    char *path = malloc(len);
    if (path) {
        snprintf(path, len, "%s/.config/inky/kavita.toml", home);
    }
    return path;
}

/*
 * Replace characters that are unsafe in a filename with underscores.
 */
static void sanitize_filename(char *name)
{
    if (!name) {
        return;
    }

    for (size_t i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            name[i] = '_';
        }
    }
}

/*
 * Render a generic selectable list with pagination. `items` may be NULL while
 * loading.
 */
static void render_select_list(const char *title, const char *const *items,
                               int count, int selected, int rows,
                               const char *status)
{
    int per_page = rows - 8;
    if (per_page < 5) {
        per_page = 5;
    }

    int page = count > 0 ? selected / per_page : 0;
    int page_start = page * per_page;
    int page_end = page_start + per_page;
    if (page_end > count) {
        page_end = count;
    }
    int total_pages = count > 0 ? (count + per_page - 1) / per_page : 1;

    terminal_clear();
    printf("Inky - %s\n", title);
    printf("---------------------\n\n");

    if (status) {
        printf("%s\n\n", status);
    } else if (items) {
        for (int i = page_start; i < page_end; i++) {
            if (i == selected) {
                printf("\033[7m > %s\033[0m\n", items[i]);
            } else {
                printf("   %s\n", items[i]);
            }
        }

        if (count == 0) {
            printf("No entries.\n");
        }
    }

    printf("\n");
    if (count > 0) {
        printf("Page %d / %d  |  Up/Down: select  Left/Right: page  Enter: open  q: back\n",
               page + 1, total_pages);
    } else {
        printf("Up/Down: select  Left/Right: page  Enter: open  q: back\n");
    }

    int used = 5 + (status ? 2 : (page_end - page_start)) + 2;
    for (int i = used; i < rows; i++) {
        printf("\n");
    }

    fflush(stdout);
}

/*
 * Run a generic selectable list and return the selected index, or -1 if the
 * user presses q / backspace.
 */
static int run_select_list(const char *title, const char *const *items,
                           int count, int rows)
{
    int selected = 0;
    int running = 1;
    int result = -1;

    while (running) {
        render_select_list(title, items, count, selected, rows, NULL);

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
        case KEY_LEFT:
            if (selected > 0) {
                int per_page = rows - 8;
                if (per_page < 5) {
                    per_page = 5;
                }
                selected -= per_page;
                if (selected < 0) {
                    selected = 0;
                }
            }
            break;
        case KEY_RIGHT:
            if (selected < count - 1) {
                int per_page = rows - 8;
                if (per_page < 5) {
                    per_page = 5;
                }
                selected += per_page;
                if (selected >= count) {
                    selected = count - 1;
                }
            }
            break;
        case KEY_ENTER:
            if (count > 0) {
                result = selected;
                running = 0;
            }
            break;
        default:
            break;
        }
    }

    return result;
}

/*
 * Download progress callback for the terminal.
 */
static void download_progress(size_t current, size_t total, void *user_data)
{
    (void)user_data;

    if (total > 0) {
        int pct = (int)((current * 100) / total);
        printf("\rDownloading... %d%% (%zu / %zu bytes)", pct, current, total);
    } else {
        printf("\rDownloading... %zu bytes", current);
    }
    fflush(stdout);
}

/*
 * Run the Kavita catalog browser and download a selected volume.
 *
 * Returns a heap-allocated path to the downloaded EPUB, or NULL if the user
 * cancels or an error occurs.
 */
static char *run_kavita_catalog(int rows)
{
    char *config_path = kavita_config_path();
    if (!config_path) {
        terminal_clear();
        printf("Could not determine config path.\n");
        terminal_read_key();
        return NULL;
    }

    kavita_config_t cfg = {0};
    if (!kavita_config_load(config_path, &cfg)) {
        terminal_clear();
        printf("Could not load Kavita config from %s\n", config_path);
        printf("Create it with:\n");
        printf("  [kavita]\n");
        printf("  url = \"http://kavita.home.lan:5000\"\n");
        printf("  api_key = \"...\"\n");
        terminal_read_key();
        free(config_path);
        return NULL;
    }
    free(config_path);

    kavita_client_t *client = kavita_client_new(&cfg);
    kavita_config_free(&cfg);
    if (!client) {
        terminal_clear();
        printf("Could not create Kavita client.\n");
        terminal_read_key();
        return NULL;
    }

    terminal_clear();
    printf("Connecting to Kavita...\n");
    fflush(stdout);

    if (!kavita_client_ping(client)) {
        terminal_clear();
        printf("Could not reach Kavita. Check URL and API key.\n");
        terminal_read_key();
        kavita_client_free(client);
        return NULL;
    }

    /* --- Libraries --- */
    kavita_library_t *libraries = NULL;
    int library_count = kavita_client_fetch_libraries(client, &libraries);
    const char **names = NULL;
    if (library_count < 0) {
        terminal_clear();
        printf("Failed to fetch libraries.\n");
        terminal_read_key();
        kavita_client_free(client);
        return NULL;
    }

    int library_id;
    if (library_count == 1) {
        library_id = libraries[0].library_id;
        kavita_libraries_free(libraries, library_count);
    } else {
        names = calloc((size_t)library_count + 1, sizeof(char *));
        if (!names) {
            kavita_libraries_free(libraries, library_count);
            kavita_client_free(client);
            return NULL;
        }
        for (int i = 0; i < library_count; i++) {
            names[i] = libraries[i].name;
        }

        int library_idx = run_select_list("Kavita - Libraries", names, library_count, rows);
        free(names);

        if (library_idx < 0) {
            kavita_libraries_free(libraries, library_count);
            kavita_client_free(client);
            return NULL;
        }

        library_id = libraries[library_idx].library_id;
        kavita_libraries_free(libraries, library_count);
    }

    if (library_id <= 0) {
        terminal_clear();
        printf("Selected library has no valid ID.\n");
        terminal_read_key();
        kavita_client_free(client);
        return NULL;
    }

    /* --- Series --- */
    kavita_series_t *series = NULL;
    int series_count = kavita_client_fetch_series(client, library_id, &series);
    if (series_count < 0) {
        terminal_clear();
        printf("Failed to fetch series.\n");
        terminal_read_key();
        kavita_client_free(client);
        return NULL;
    }

    names = calloc((size_t)series_count + 1, sizeof(char *));
    if (!names) {
        kavita_series_free(series, series_count);
        kavita_client_free(client);
        return NULL;
    }
    for (int i = 0; i < series_count; i++) {
        names[i] = series[i].name;
    }

    int series_idx = run_select_list("Kavita - Series", names, series_count, rows);
    free(names);

    if (series_idx < 0) {
        kavita_series_free(series, series_count);
        kavita_client_free(client);
        return NULL;
    }

    int series_id = series[series_idx].series_id;
    char *series_name = strdup(series[series_idx].name);
    kavita_series_free(series, series_count);

    if (series_id <= 0) {
        terminal_clear();
        printf("Selected series has no valid ID.\n");
        terminal_read_key();
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }

    /* --- Volumes --- */
    kavita_volume_t *volumes = NULL;
    int volume_count = kavita_client_fetch_volumes(client, series_id, &volumes);
    if (volume_count < 0) {
        terminal_clear();
        printf("Failed to fetch volumes.\n");
        terminal_read_key();
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }

    names = calloc((size_t)volume_count + 1, sizeof(char *));
    if (!names) {
        kavita_volumes_free(volumes, volume_count);
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }
    for (int i = 0; i < volume_count; i++) {
        names[i] = volumes[i].title;
    }

    int volume_idx = run_select_list("Kavita - Volumes", names, volume_count, rows);
    free(names);

    if (volume_idx < 0) {
        kavita_volumes_free(volumes, volume_count);
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }

    /* --- Download --- */
    const char *books_dir = find_books_dir();
    if (!books_dir) {
        terminal_clear();
        printf("Could not find Books/ directory.\n");
        terminal_read_key();
        kavita_volumes_free(volumes, volume_count);
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }

    char *filename = strdup(volumes[volume_idx].title);
    sanitize_filename(filename);

    size_t path_len = strlen(books_dir) + 1 + strlen(filename) + strlen(".epub") + 1;
    char *out_path = malloc(path_len);
    if (!out_path) {
        free(filename);
        kavita_volumes_free(volumes, volume_count);
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }
    snprintf(out_path, path_len, "%s/%s.epub", books_dir, filename);

    terminal_clear();
    printf("Downloading %s...\n", volumes[volume_idx].title);
    fflush(stdout);

    int ok = kavita_client_download_volume(client, &volumes[volume_idx],
                                           out_path, download_progress, NULL);

    printf("\n");

    if (!ok) {
        terminal_clear();
        printf("Download failed.\n");
        terminal_read_key();
        free(out_path);
        free(filename);
        kavita_volumes_free(volumes, volume_count);
        free(series_name);
        kavita_client_free(client);
        return NULL;
    }

    /* Write the metadata sidecar. */
    kavita_meta_t meta = {
        .kavita_volume_id = volumes[volume_idx].volume_id,
        .kavita_series_id = series_id,
        .title = volumes[volume_idx].title,
    };
    kavita_meta_save(out_path, &meta);

    kavita_volumes_free(volumes, volume_count);
    free(series_name);
    free(filename);
    kavita_client_free(client);

    terminal_clear();
    printf("Downloaded to %s\n", out_path);
    printf("Press any key to open it, or q to return to the list.\n");
    int key = terminal_read_key();
    if (key == KEY_QUIT) {
        free(out_path);
        return NULL;
    }

    return out_path;
}

/*
 * Render the book list to the terminal.
 */
static void render_book_list(const book_entry_t *entries, int count,
                             int selected, int rows, int show_kavita)
{
    terminal_clear();
    printf("Inky - Select a book\n");
    printf("---------------------\n\n");

    int total_items = count + (show_kavita ? 1 : 0);

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

    if (show_kavita) {
        if (selected == count) {
            printf("\033[7m > [Kavita]\033[0m\n");
        } else {
            printf("   [Kavita]\n");
        }
    }

    if (total_items == 0) {
        const char *books_dir = find_books_dir();
        printf("No .epub files found in %s/\n", books_dir ? books_dir : "Books");
    }

    printf("\n");
    printf("Up/Down: select  Enter: open  q: quit\n");

    /* Pad to keep the screen stable. */
    int used = 5 + total_items + 2;
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

    int show_kavita = 1;
    int total_items = count + (show_kavita ? 1 : 0);
    int selected = 0;
    int running = 1;
    char *result = NULL;

    while (running) {
        render_book_list(entries, count, selected, rows, show_kavita);

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
            if (selected < total_items - 1) {
                selected++;
            }
            break;
        case KEY_ENTER:
            if (selected == count && show_kavita) {
                result = run_kavita_catalog(rows);
                if (result) {
                    running = 0;
                }
            } else if (count > 0) {
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
