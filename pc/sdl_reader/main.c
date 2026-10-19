/*
 * Inky SDL2 reader - step 6.
 *
 * Launched without arguments, shows a selectable list of EPUBs in Books/ plus
 * a [Kavita] entry for downloading books from a self-hosted Kavita server.
 * Launched with a book path, opens it directly.
 *
 * Book list:
 *   Up/Down arrows  -> move selection
 *   Enter           -> open selected book
 *   q / Escape      -> quit
 *
 * Reader:
 *   Right arrow / Page Down / Space -> next page
 *   Left arrow  / Page Up           -> previous page
 *   q / Escape                      -> close book and return to book list
 *
 * Reuses the shared engine and reader session from the terminal reader.
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../shared/books_dir/books_dir.h"
#include "../../shared/book_list/book_list.h"
#include "../../shared/kavita/kavita.h"
#include "../../shared/reader/reader.h"
#include "../../shared/battery/battery.h"

/* Target e-paper resolution for the LilyGo T5 4.7" S3. */
#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 540

/* Font and margins. */
#define FONT_SIZE     24
#define MARGIN_X      20
#define MARGIN_Y      20
#define LINE_SPACING  8
#define STATUS_HEIGHT 40

/*
 * Application state.
 */
typedef enum {
    STATE_LIST,
    STATE_READER,
    STATE_QUIT,
} app_state_t;

typedef struct {
    app_state_t state;

    /* Book list. */
    book_entry_t *books;
    int book_count;
    int selected_book;
    int show_kavita;
    int pending_kavita;

    /* Reader session. */
    reader_session_t reader;

    /* SDL. */
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font;
    SDL_Color fg;
    SDL_Color bg;

    int cell_w;
    int cell_h;
} app_t;

/*
 * User data passed to the layout render callback.
 */
typedef struct {
    app_t *app;
    int total_pages;
} sdl_render_ctx_t;

/*
 * Try to load a usable monospace TTF font from common Arch Linux paths.
 */
static TTF_Font *load_font(int size)
{
    const char *font_paths[] = {
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        NULL,
    };

    for (int i = 0; font_paths[i] != NULL; i++) {
        TTF_Font *font = TTF_OpenFont(font_paths[i], size);
        if (font) {
            return font;
        }
    }

    return NULL;
}

/*
 * Measure the average glyph size for cell-based layout.
 */
static void measure_font(TTF_Font *font, int *cell_w, int *cell_h)
{
    int advance;
    TTF_GlyphMetrics(font, 'M', NULL, NULL, NULL, NULL, &advance);
    if (advance <= 0) {
        advance = FONT_SIZE / 2;
    }

    int max_h;
    TTF_SizeUTF8(font, "Ay", NULL, &max_h);

    *cell_w = advance;
    *cell_h = max_h + LINE_SPACING;
}

/*
 * Render a line of text at the given pixel coordinates.
 */
static void draw_text_line(app_t *app, const char *text, int x, int y)
{
    if (!text || text[0] == '\0') {
        return;
    }

    SDL_Surface *surface = TTF_RenderUTF8_Shaded(app->font, text, app->fg, app->bg);
    if (!surface) {
        return;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(app->renderer, surface);
    SDL_Rect dst = {x, y, surface->w, surface->h};
    SDL_RenderCopy(app->renderer, texture, NULL, &dst);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

/*
 * Layout render callback for the SDL reader.
 */
static void sdl_render_line(int line_number, const char *line_text,
                            int is_status, void *user_data)
{
    sdl_render_ctx_t *ctx = (sdl_render_ctx_t *)user_data;
    app_t *app = ctx->app;
    const reader_session_t *reader = &app->reader;

    if (is_status) {
        int chapter_pct = ctx->total_pages > 0
                              ? ((reader->current_page + 1) * 100) / ctx->total_pages
                              : 0;
        int book_pct = reader->epub->chapter_count > 0
                           ? ((reader->current_chapter + 1) * 100) / reader->epub->chapter_count
                           : 0;

        const char *battery = battery_string(reader->battery_pct);

        char status[256];
        snprintf(status, sizeof(status),
                 "%s  |  page %d / %d (%d%%)  |  Book %d / %d (%d%%)  |  Bat: %s",
                 reader->epub->chapters[reader->current_chapter].idref,
                 reader->current_page + 1, ctx->total_pages, chapter_pct,
                 reader->current_chapter + 1, reader->epub->chapter_count, book_pct,
                 battery);

        draw_text_line(app, status, MARGIN_X, SCREEN_HEIGHT - app->cell_h - 4);
    } else {
        int content_rows = reader->rows - reader->status_rows;
        int line_in_page = line_number - (reader->current_page * content_rows);
        int y = MARGIN_Y + line_in_page * app->cell_h;
        draw_text_line(app, line_text, MARGIN_X, y);
    }
}

/*
 * Open the currently selected book.
 */
static void reader_open_selected(app_t *app)
{
    if (app->book_count <= 0 || app->selected_book < 0 ||
        app->selected_book >= app->book_count) {
        return;
    }

    app->reader.cols = (SCREEN_WIDTH - 2 * MARGIN_X) / app->cell_w;
    app->reader.rows = (SCREEN_HEIGHT - 2 * MARGIN_Y) / app->cell_h;
    app->reader.status_rows = (STATUS_HEIGHT + app->cell_h - 1) / app->cell_h;

    if (reader_open(&app->reader, app->books[app->selected_book].path)) {
        app->state = STATE_READER;
    }
}

/*
 * Open a book by path.
 */
static void reader_open_path(app_t *app, const char *path)
{
    if (!path) {
        return;
    }

    app->reader.cols = (SCREEN_WIDTH - 2 * MARGIN_X) / app->cell_w;
    app->reader.rows = (SCREEN_HEIGHT - 2 * MARGIN_Y) / app->cell_h;
    app->reader.status_rows = (STATUS_HEIGHT + app->cell_h - 1) / app->cell_h;

    if (reader_open(&app->reader, path)) {
        app->state = STATE_READER;
    }
}

/*
 * Close the current book and return to the book list.
 */
static void reader_close_book(app_t *app)
{
    char *previous_path = NULL;
    if (app->reader.book_path) {
        previous_path = strdup(app->reader.book_path);
    }

    reader_close(&app->reader);
    app->state = STATE_LIST;

    /* Rescan the book list so progress % is up to date. */
    book_list_free(app->books, app->book_count);
    const char *books_dir = find_books_dir();
    app->book_count = book_list_scan(books_dir, &app->books);
    if (app->book_count < 0) {
        app->book_count = 0;
        app->books = NULL;
    }

    /* Try to reselect the book that was just closed. */
    if (previous_path) {
        for (int i = 0; i < app->book_count; i++) {
            if (strcmp(app->books[i].path, previous_path) == 0) {
                app->selected_book = i;
                break;
            }
        }
        free(previous_path);
    }
}

/*
 * Render the book list screen.
 */
static void render_list(app_t *app)
{
    SDL_SetRenderDrawColor(app->renderer, app->bg.r, app->bg.g, app->bg.b, app->bg.a);
    SDL_RenderClear(app->renderer);

    int y = MARGIN_Y;
    int total_items = app->book_count + (app->show_kavita ? 1 : 0);

    draw_text_line(app, "Inky - Select a book", MARGIN_X, y);
    y += app->cell_h * 2;

    if (total_items == 0) {
        const char *books_dir = find_books_dir();
        char msg[256];
        snprintf(msg, sizeof(msg), "No .epub files found in %s/", books_dir ? books_dir : "Books");
        draw_text_line(app, msg, MARGIN_X, y);
    } else {
        for (int i = 0; i < app->book_count; i++) {
            char line[512];
            if (app->books[i].has_progress) {
                snprintf(line, sizeof(line), "%s %s - %d%%",
                         i == app->selected_book ? ">" : " ",
                         app->books[i].filename,
                         app->books[i].progress_pct);
            } else {
                snprintf(line, sizeof(line), "%s %s",
                         i == app->selected_book ? ">" : " ",
                         app->books[i].filename);
            }
            draw_text_line(app, line, MARGIN_X, y);
            y += app->cell_h;
        }

        if (app->show_kavita) {
            char line[64];
            snprintf(line, sizeof(line), "%s [Kavita]",
                     app->selected_book == app->book_count ? ">" : " ");
            draw_text_line(app, line, MARGIN_X, y);
        }
    }

    y = SCREEN_HEIGHT - app->cell_h - 4;
    draw_text_line(app, "Up/Down: select  Enter: open  Esc/q: quit", MARGIN_X, y);

    SDL_RenderPresent(app->renderer);
}

/*
 * Render the current page and the bottom status bar.
 */
static void render_reader(app_t *app)
{
    const reader_session_t *reader = &app->reader;

    SDL_SetRenderDrawColor(app->renderer, app->bg.r, app->bg.g, app->bg.b, app->bg.a);
    SDL_RenderClear(app->renderer);

    if (!reader->layout) {
        char msg[256];
        if (reader->chapter_load_failed) {
            snprintf(msg, sizeof(msg), "Could not load chapter %d",
                     reader->current_chapter + 1);
        } else {
            snprintf(msg, sizeof(msg), "No content to display");
        }
        draw_text_line(app, msg, MARGIN_X, MARGIN_Y);
        SDL_RenderPresent(app->renderer);
        return;
    }

    int content_rows = reader->rows - reader->status_rows;
    int total_pages = reader_page_count(reader);

    sdl_render_ctx_t ctx = {
        .app = app,
        .total_pages = total_pages,
    };

    layout_render_page(reader->layout, reader->current_page, content_rows,
                       sdl_render_line, &ctx);

    /* Status line at the bottom. */
    sdl_render_line(-1, "", 1, &ctx);

    SDL_RenderPresent(app->renderer);
}

/*
 * Render the current screen based on application state.
 */
static void app_render(app_t *app)
{
    switch (app->state) {
    case STATE_LIST:
        render_list(app);
        break;
    case STATE_READER:
        render_reader(app);
        break;
    default:
        break;
    }
}

/*
 * Handle a key press based on application state.
 */
static void app_handle_key(app_t *app, SDL_Keycode key)
{
    int total_items = app->book_count + (app->show_kavita ? 1 : 0);

    if (app->state == STATE_LIST) {
        switch (key) {
        case SDLK_q:
        case SDLK_ESCAPE:
            app->state = STATE_QUIT;
            break;
        case SDLK_UP:
            if (app->selected_book > 0) {
                app->selected_book--;
            }
            break;
        case SDLK_DOWN:
            if (app->selected_book < total_items - 1) {
                app->selected_book++;
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (app->show_kavita && app->selected_book == app->book_count) {
                app->pending_kavita = 1;
            } else {
                reader_open_selected(app);
            }
            break;
        default:
            break;
        }
    } else if (app->state == STATE_READER) {
        switch (key) {
        case SDLK_q:
        case SDLK_ESCAPE:
            reader_close_book(app);
            break;
        case SDLK_RIGHT:
        case SDLK_PAGEDOWN:
        case SDLK_SPACE:
            reader_navigate(&app->reader, 1);
            break;
        case SDLK_LEFT:
        case SDLK_PAGEUP:
            reader_navigate(&app->reader, -1);
            break;
        default:
            break;
        }
    }
}

/*
 * Handle a mouse click or touch event based on application state.
 *
 * Reader touch zones:
 *   Left half of the screen  -> previous page
 *   Right half of the screen -> next page
 *   Top edge (y < 60)        -> close book / return to list
 *
 * Book-list touch zones:
 *   Left half  -> select previous book
 *   Right half -> select next book
 *   Center     -> open selected book
 *   Top edge   -> quit
 */
static void app_handle_click(app_t *app, int x, int y)
{
    if (app->state == STATE_LIST) {
        if (y < 60) {
            app->state = STATE_QUIT;
            return;
        }

        int third = SCREEN_WIDTH / 3;
        if (x < third) {
            if (app->selected_book > 0) {
                app->selected_book--;
            }
        } else if (x >= 2 * third) {
            int total_items = app->book_count + (app->show_kavita ? 1 : 0);
            if (app->selected_book < total_items - 1) {
                app->selected_book++;
            }
        } else {
            if (app->show_kavita && app->selected_book == app->book_count) {
                app->pending_kavita = 1;
            } else {
                reader_open_selected(app);
            }
        }
    } else if (app->state == STATE_READER) {
        if (y < 60) {
            reader_close_book(app);
            return;
        }

        int half = SCREEN_WIDTH / 2;
        if (x < half) {
            reader_navigate(&app->reader, -1);
        } else {
            reader_navigate(&app->reader, 1);
        }
    }
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
 * Render a generic Kavita selection list with pagination.
 */
static void render_kavita_list(app_t *app, const char *title,
                               const char *const *items, int count,
                               int selected, const char *status)
{
    SDL_SetRenderDrawColor(app->renderer, app->bg.r, app->bg.g, app->bg.b, app->bg.a);
    SDL_RenderClear(app->renderer);

    int y = MARGIN_Y;

    draw_text_line(app, title, MARGIN_X, y);
    y += app->cell_h * 2;

    int per_page = (SCREEN_HEIGHT - y - app->cell_h * 2 - 4) / app->cell_h;
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

    if (status) {
        draw_text_line(app, status, MARGIN_X, y);
        y += app->cell_h * 2;
    } else if (items) {
        for (int i = page_start; i < page_end; i++) {
            char line[512];
            snprintf(line, sizeof(line), "%s %s",
                     i == selected ? ">" : " ", items[i]);
            draw_text_line(app, line, MARGIN_X, y);
            y += app->cell_h;
        }

        if (count == 0) {
            draw_text_line(app, "No entries.", MARGIN_X, y);
        }
    }

    char footer[256];
    if (count > 0) {
        snprintf(footer, sizeof(footer),
                 "Page %d / %d  |  Up/Down: select  Left/Right: page  Enter: open  Esc/q: back",
                 page + 1, total_pages);
    } else {
        snprintf(footer, sizeof(footer),
                 "Up/Down: select  Left/Right: page  Enter: open  Esc/q: back");
    }
    y = SCREEN_HEIGHT - app->cell_h - 4;
    draw_text_line(app, footer, MARGIN_X, y);

    SDL_RenderPresent(app->renderer);
}

enum {
    KAVITA_KEY_QUIT,
    KAVITA_KEY_UP,
    KAVITA_KEY_DOWN,
    KAVITA_KEY_PAGE_UP,
    KAVITA_KEY_PAGE_DOWN,
    KAVITA_KEY_ENTER,
};

/*
 * Poll SDL events until the user presses Up, Down, Enter, Escape or q.
 * Returns the key code that ended the wait.
 */
static int kavita_wait_key(void)
{
    SDL_Event event;

    while (1) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                return KAVITA_KEY_QUIT;
            }
            if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE:
                    return KAVITA_KEY_QUIT;
                case SDLK_UP:
                    return KAVITA_KEY_UP;
                case SDLK_DOWN:
                    return KAVITA_KEY_DOWN;
                case SDLK_LEFT:
                case SDLK_PAGEUP:
                    return KAVITA_KEY_PAGE_UP;
                case SDLK_RIGHT:
                case SDLK_PAGEDOWN:
                    return KAVITA_KEY_PAGE_DOWN;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    return KAVITA_KEY_ENTER;
                default:
                    break;
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                int x = event.button.x;
                int y = event.button.y;
                if (y < 60) {
                    return KAVITA_KEY_QUIT;
                }
                if (y >= SCREEN_HEIGHT - 60) {
                    /* Bottom strip: left/right third for paging, center enter. */
                    int third = SCREEN_WIDTH / 3;
                    if (x < third) {
                        return KAVITA_KEY_PAGE_UP;
                    } else if (x >= 2 * third) {
                        return KAVITA_KEY_PAGE_DOWN;
                    } else {
                        return KAVITA_KEY_ENTER;
                    }
                }
                int third = SCREEN_WIDTH / 3;
                if (x < third) {
                    return KAVITA_KEY_UP;
                } else if (x >= 2 * third) {
                    return KAVITA_KEY_DOWN;
                } else {
                    return KAVITA_KEY_ENTER;
                }
            }
        }

        SDL_Delay(16);
    }
}

/*
 * Run a generic selectable list for the Kavita catalog.
 *
 * Returns the selected index, or -1 if the user cancels.
 */
static int run_kavita_select(app_t *app, const char *title,
                             const char *const *items, int count)
{
    int selected = 0;

    while (1) {
        render_kavita_list(app, title, items, count, selected, NULL);

        int key = kavita_wait_key();
        switch (key) {
        case KAVITA_KEY_QUIT:
            return -1;
        case KAVITA_KEY_UP:
            if (selected > 0) {
                selected--;
            }
            break;
        case KAVITA_KEY_DOWN:
            if (selected < count - 1) {
                selected++;
            }
            break;
        case KAVITA_KEY_PAGE_UP:
            if (selected > 0) {
                int per_page = (SCREEN_HEIGHT - MARGIN_Y - app->cell_h * 4 - 4) / app->cell_h;
                if (per_page < 5) {
                    per_page = 5;
                }
                selected -= per_page;
                if (selected < 0) {
                    selected = 0;
                }
            }
            break;
        case KAVITA_KEY_PAGE_DOWN:
            if (selected < count - 1) {
                int per_page = (SCREEN_HEIGHT - MARGIN_Y - app->cell_h * 4 - 4) / app->cell_h;
                if (per_page < 5) {
                    per_page = 5;
                }
                selected += per_page;
                if (selected >= count) {
                    selected = count - 1;
                }
            }
            break;
        case KAVITA_KEY_ENTER:
            if (count > 0) {
                return selected;
            }
            break;
        default:
            break;
        }
    }
}

/*
 * Download progress callback for SDL.
 */
static void sdl_download_progress(size_t current, size_t total, void *user_data)
{
    app_t *app = (app_t *)user_data;

    char status[256];
    if (total > 0) {
        int pct = (int)((current * 100) / total);
        snprintf(status, sizeof(status), "Downloading... %d%% (%zu / %zu bytes)",
                 pct, current, total);
    } else {
        snprintf(status, sizeof(status), "Downloading... %zu bytes", current);
    }

    SDL_SetRenderDrawColor(app->renderer, app->bg.r, app->bg.g, app->bg.b, app->bg.a);
    SDL_RenderClear(app->renderer);
    draw_text_line(app, status, MARGIN_X, MARGIN_Y);
    SDL_RenderPresent(app->renderer);
}

/*
 * Run the Kavita catalog browser and download a selected volume.
 *
 * Returns a heap-allocated path to the downloaded EPUB, or NULL if the user
 * cancels or an error occurs.
 */
static char *run_kavita_catalog(app_t *app)
{
    char *config_path = kavita_config_path();
    if (!config_path) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Could not determine config path.");
        kavita_wait_key();
        return NULL;
    }

    kavita_config_t cfg = {0};
    if (!kavita_config_load(config_path, &cfg)) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Could not load ~/.config/inky/kavita.toml");
        kavita_wait_key();
        free(config_path);
        return NULL;
    }
    free(config_path);

    kavita_client_t *client = kavita_client_new(&cfg);
    kavita_config_free(&cfg);
    if (!client) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Could not create Kavita client.");
        kavita_wait_key();
        return NULL;
    }

    render_kavita_list(app, "Kavita", NULL, 0, 0, "Connecting to Kavita...");

    if (!kavita_client_ping(client)) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Could not reach Kavita. Check URL and API key.");
        kavita_wait_key();
        kavita_client_free(client);
        return NULL;
    }

    /* --- Libraries --- */
    kavita_library_t *libraries = NULL;
    int library_count = kavita_client_fetch_libraries(client, &libraries);
    const char **names = NULL;
    if (library_count < 0) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Failed to fetch libraries.");
        kavita_wait_key();
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

        int library_idx = run_kavita_select(app, "Kavita - Libraries", names, library_count);
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
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Selected library has no valid ID.");
        kavita_wait_key();
        kavita_client_free(client);
        return NULL;
    }

    /* --- Series --- */
    kavita_series_t *series = NULL;
    int series_count = kavita_client_fetch_series(client, library_id, &series);
    if (series_count < 0) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Failed to fetch series.");
        kavita_wait_key();
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

    int series_idx = run_kavita_select(app, "Kavita - Series", names, series_count);
    free(names);

    if (series_idx < 0) {
        kavita_series_free(series, series_count);
        kavita_client_free(client);
        return NULL;
    }

    int series_id = series[series_idx].series_id;
    kavita_series_free(series, series_count);

    if (series_id <= 0) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Selected series has no valid ID.");
        kavita_wait_key();
        kavita_client_free(client);
        return NULL;
    }

    /* --- Volumes --- */
    kavita_volume_t *volumes = NULL;
    int volume_count = kavita_client_fetch_volumes(client, series_id, &volumes);
    if (volume_count < 0) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Failed to fetch volumes.");
        kavita_wait_key();
        kavita_client_free(client);
        return NULL;
    }

    names = calloc((size_t)volume_count + 1, sizeof(char *));
    if (!names) {
        kavita_volumes_free(volumes, volume_count);
        kavita_client_free(client);
        return NULL;
    }
    for (int i = 0; i < volume_count; i++) {
        names[i] = volumes[i].title;
    }

    int volume_idx = run_kavita_select(app, "Kavita - Volumes", names, volume_count);
    free(names);

    if (volume_idx < 0) {
        kavita_volumes_free(volumes, volume_count);
        kavita_client_free(client);
        return NULL;
    }

    /* --- Download --- */
    const char *books_dir = find_books_dir();
    if (!books_dir) {
        render_kavita_list(app, "Kavita", NULL, 0, 0,
                           "Could not find Books/ directory.");
        kavita_wait_key();
        kavita_volumes_free(volumes, volume_count);
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
        kavita_client_free(client);
        return NULL;
    }
    snprintf(out_path, path_len, "%s/%s.epub", books_dir, filename);

    int ok = kavita_client_download_volume(client, &volumes[volume_idx],
                                           out_path, sdl_download_progress, app);

    if (!ok) {
        render_kavita_list(app, "Kavita", NULL, 0, 0, "Download failed.");
        kavita_wait_key();
        free(out_path);
        free(filename);
        kavita_volumes_free(volumes, volume_count);
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
    free(filename);
    kavita_client_free(client);

    render_kavita_list(app, "Kavita", NULL, 0, 0,
                       "Download complete. Press Enter to open, Esc/q to return.");

    int key = kavita_wait_key();
    if (key == KAVITA_KEY_QUIT) {
        free(out_path);
        return NULL;
    }

    return out_path;
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    app_t app = {0};
    reader_init(&app.reader);
    app.show_kavita = 1;

    app.window = SDL_CreateWindow(
        "Inky SDL Reader",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    app.renderer = SDL_CreateRenderer(
        app.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    app.font = load_font(FONT_SIZE);
    if (!app.font) {
        fprintf(stderr, "TTF_OpenFont failed: %s\n", TTF_GetError());
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    app.fg = (SDL_Color){0, 0, 0, 255};
    app.bg = (SDL_Color){255, 255, 255, 255};

    measure_font(app.font, &app.cell_w, &app.cell_h);

    /* Load book list. */
    const char *books_dir = find_books_dir();
    app.book_count = book_list_scan(books_dir, &app.books);
    if (app.book_count < 0) {
        app.book_count = 0;
        app.books = NULL;
    }

    /* If a path was given on the command line, open it directly. */
    if (argc >= 2) {
        app.reader.cols = (SCREEN_WIDTH - 2 * MARGIN_X) / app.cell_w;
        app.reader.rows = (SCREEN_HEIGHT - 2 * MARGIN_Y) / app.cell_h;
        app.reader.status_rows = 3;
        if (reader_open(&app.reader, argv[1])) {
            app.state = STATE_READER;
        } else {
            app.state = STATE_LIST;
        }
    } else {
        app.state = STATE_LIST;
    }

    SDL_Event event;

    while (app.state != STATE_QUIT) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                app.state = STATE_QUIT;
            }
            if (event.type == SDL_KEYDOWN) {
                app_handle_key(&app, event.key.keysym.sym);
            }
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    app_handle_click(&app, event.button.x, event.button.y);
                }
            }
            if (event.type == SDL_FINGERDOWN) {
                int x = (int)(event.tfinger.x * SCREEN_WIDTH);
                int y = (int)(event.tfinger.y * SCREEN_HEIGHT);
                app_handle_click(&app, x, y);
            }
        }

        /*
         * If the user selected the Kavita entry, run the catalog browser as a
         * modal and open any downloaded book on return.
         */
        if (app.pending_kavita) {
            app.pending_kavita = 0;
            char *kavita_path = run_kavita_catalog(&app);
            if (kavita_path) {
                reader_open_path(&app, kavita_path);
                free(kavita_path);
            }
        }

        app_render(&app);
        SDL_Delay(16);
    }

    /*
     * If a book is open when the window closes (SDL_QUIT), save its progress
     * before freeing resources. reader_free does not save; reader_close does.
     */
    if (app.reader.epub) {
        reader_close(&app.reader);
    } else {
        reader_free(&app.reader);
    }
    book_list_free(app.books, app.book_count);

    TTF_CloseFont(app.font);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
