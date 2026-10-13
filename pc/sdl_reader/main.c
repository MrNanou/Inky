/*
 * Inky SDL2 reader - step 4.
 *
 * Launched without arguments, shows a selectable list of EPUBs in Books/.
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
#include "../../shared/reader/reader.h"

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
 * Close the current book and return to the book list.
 */
static void reader_close_book(app_t *app)
{
    reader_close(&app->reader);
    app->state = STATE_LIST;
}

/*
 * Render the book list screen.
 */
static void render_list(app_t *app)
{
    SDL_SetRenderDrawColor(app->renderer, app->bg.r, app->bg.g, app->bg.b, app->bg.a);
    SDL_RenderClear(app->renderer);

    int y = MARGIN_Y;

    draw_text_line(app, "Inky - Select a book", MARGIN_X, y);
    y += app->cell_h * 2;

    if (app->book_count == 0) {
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

    if (!reader->layout) {
        return;
    }

    SDL_SetRenderDrawColor(app->renderer, app->bg.r, app->bg.g, app->bg.b, app->bg.a);
    SDL_RenderClear(app->renderer);

    int y = MARGIN_Y;

    int content_rows = reader->rows - reader->status_rows;
    int total_pages = reader_page_count(reader);
    int first_line = reader->current_page * content_rows;
    int last_line = first_line + content_rows;
    if (last_line > reader->layout->line_count) {
        last_line = reader->layout->line_count;
    }

    for (int i = first_line; i < last_line; i++) {
        draw_text_line(app, reader->layout->text + reader->layout->line_offsets[i], MARGIN_X, y);
        y += app->cell_h;
    }

    int chapter_pct = total_pages > 0 ? ((reader->current_page + 1) * 100) / total_pages : 0;
    int book_pct = reader->epub->chapter_count > 0
                       ? ((reader->current_chapter + 1) * 100) / reader->epub->chapter_count
                       : 0;

    char status[256];
    snprintf(status, sizeof(status),
             "%s  |  page %d / %d (%d%%)  |  Book %d / %d (%d%%)",
             reader->epub->chapters[reader->current_chapter].idref,
             reader->current_page + 1, total_pages, chapter_pct,
             reader->current_chapter + 1, reader->epub->chapter_count, book_pct);

    draw_text_line(app, status, MARGIN_X, SCREEN_HEIGHT - app->cell_h - 4);

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
            if (app->selected_book < app->book_count - 1) {
                app->selected_book++;
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            reader_open_selected(app);
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
        app.reader.status_rows = (STATUS_HEIGHT + app.cell_h - 1) / app.cell_h;
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
        }

        app_render(&app);
        SDL_Delay(16);
    }

    reader_free(&app.reader);
    book_list_free(app.books, app.book_count);

    TTF_CloseFont(app.font);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
