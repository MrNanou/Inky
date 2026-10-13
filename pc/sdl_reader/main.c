/*
 * Inky SDL2 reader - step 2.
 *
 * Reads an EPUB and renders it paginated in a 960x540 SDL2 window.
 * Reuses the shared engine from the terminal reader.
 *
 * Controls:
 *   Right arrow / Page Down / Space -> next page
 *   Left arrow  / Page Up           -> previous page
 *   q / Escape                      -> quit
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../shared/epub/epub.h"
#include "../../shared/html/html.h"
#include "../../shared/layout/layout.h"
#include "../../shared/progress/progress.h"

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
 * Reader state shared between the SDL loop and rendering helpers.
 */
typedef struct {
    epub_t *epub;
    int current_chapter;
    int current_page;

    char *chapter_text;
    layout_t *layout;

    TTF_Font *font;
    SDL_Renderer *renderer;
    SDL_Color fg;
    SDL_Color bg;

    int cell_w;   /* Average glyph width in pixels. */
    int cell_h;   /* Line height in pixels. */
    int cols;     /* Content columns. */
    int rows;     /* Content rows. */
} reader_t;

/*
 * Try to load a usable TTF font from common Arch Linux paths.
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

    int min_h, max_h;
    TTF_SizeUTF8(font, "Ay", NULL, &max_h);
    TTF_SizeUTF8(font, ".", NULL, &min_h);

    *cell_w = advance;
    *cell_h = max_h + LINE_SPACING;
}

/*
 * Load a chapter by index into the reader state.
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
        return;
    }

    size_t text_size = chapter_len + 4096;
    reader->chapter_text = malloc(text_size);
    if (!reader->chapter_text) {
        free(chapter_html);
        return;
    }

    html_to_text(chapter_html, reader->chapter_text, text_size);
    free(chapter_html);

    reader->layout = layout_new();
    if (reader->layout) {
        layout_build(reader->layout, reader->chapter_text, reader->cols);
    }
}

/*
 * Render a line of text at the given pixel coordinates.
 */
static void draw_text_line(reader_t *reader, const char *text, int x, int y)
{
    if (!text || text[0] == '\0') {
        return;
    }

    SDL_Surface *surface = TTF_RenderUTF8_Shaded(reader->font, text, reader->fg, reader->bg);
    if (!surface) {
        return;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(reader->renderer, surface);
    SDL_Rect dst = {x, y, surface->w, surface->h};
    SDL_RenderCopy(reader->renderer, texture, NULL, &dst);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

/*
 * Render the current page and the bottom status bar.
 */
static void reader_render(reader_t *reader)
{
    if (!reader->layout) {
        return;
    }

    /* White background. */
    SDL_SetRenderDrawColor(reader->renderer,
                           reader->bg.r, reader->bg.g, reader->bg.b, reader->bg.a);
    SDL_RenderClear(reader->renderer);

    int y = MARGIN_Y;

    int first_line = reader->current_page * reader->rows;
    int last_line = first_line + reader->rows;
    if (last_line > reader->layout->line_count) {
        last_line = reader->layout->line_count;
    }

    for (int i = first_line; i < last_line && y + reader->cell_h <= SCREEN_HEIGHT - STATUS_HEIGHT; i++) {
        draw_text_line(reader,
                       reader->layout->text + reader->layout->line_offsets[i],
                       MARGIN_X, y);
        y += reader->cell_h;
    }

    /* Draw status bar at the bottom. */
    int total_pages = layout_page_count(reader->layout, reader->rows);
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

    draw_text_line(reader, status, MARGIN_X, SCREEN_HEIGHT - reader->cell_h - 4);

    SDL_RenderPresent(reader->renderer);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <book.epub>\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Inky SDL Reader",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    TTF_Font *font = load_font(FONT_SIZE);
    if (!font) {
        fprintf(stderr, "TTF_OpenFont failed: %s\n", TTF_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    reader_t reader = {0};
    reader.epub = epub_open(argv[1]);
    if (!reader.epub) {
        TTF_CloseFont(font);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    reader.font = font;
    reader.renderer = renderer;
    reader.fg = (SDL_Color){0, 0, 0, 255};
    reader.bg = (SDL_Color){255, 255, 255, 255};

    measure_font(font, &reader.cell_w, &reader.cell_h);

    reader.cols = (SCREEN_WIDTH - 2 * MARGIN_X) / reader.cell_w;
    reader.rows = (SCREEN_HEIGHT - 2 * MARGIN_Y - STATUS_HEIGHT) / reader.cell_h;

    if (!progress_load(argv[1], &reader.current_chapter, &reader.current_page)) {
        reader.current_chapter = 0;
        reader.current_page = 0;
    }
    if (reader.current_chapter < 0) {
        reader.current_chapter = 0;
    }
    if (reader.current_chapter >= reader.epub->chapter_count) {
        reader.current_chapter = reader.epub->chapter_count - 1;
    }

    reader_load_chapter(&reader, reader.current_chapter);
    if (reader.layout) {
        int pages = layout_page_count(reader.layout, reader.rows);
        if (reader.current_page >= pages) {
            reader.current_page = pages > 0 ? pages - 1 : 0;
        }
    }

    reader_render(&reader);

    int running = 1;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            }
            if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE:
                    running = 0;
                    break;
                case SDLK_RIGHT:
                case SDLK_PAGEDOWN:
                case SDLK_SPACE:
                    if (reader.layout &&
                        reader.current_page < layout_page_count(reader.layout, reader.rows) - 1) {
                        reader.current_page++;
                    } else if (reader.current_chapter < reader.epub->chapter_count - 1) {
                        reader.current_chapter++;
                        reader_load_chapter(&reader, reader.current_chapter);
                    }
                    break;
                case SDLK_LEFT:
                case SDLK_PAGEUP:
                    if (reader.current_page > 0) {
                        reader.current_page--;
                    } else if (reader.current_chapter > 0) {
                        reader.current_chapter--;
                        reader_load_chapter(&reader, reader.current_chapter);
                        reader.current_page = layout_page_count(reader.layout, reader.rows) - 1;
                        if (reader.current_page < 0) {
                            reader.current_page = 0;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }

        reader_render(&reader);
        SDL_Delay(16);
    }

    progress_save(argv[1], reader.current_chapter, reader.current_page);

    layout_free(reader.layout);
    free(reader.chapter_text);
    epub_close(reader.epub);

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
