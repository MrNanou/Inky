# Inky

A portable e-ink ebook reader built around an ESP32-S3, targeting a 4.7" LilyGo T5 e-paper board. Inky reads DRM-free EPUBs from a microSD card and syncs reading progress with a self-hosted [Kavita](https://www.kavitareader.com/) instance.

> **Status:** early prototype. The engine is being developed on Linux with SDL2 (currently a terminal-based prototype) and will be ported to ESP-IDF once the core is solid.

## Hardware target

- **Board:** LilyGo T5 4.7" S3
- **SoC:** ESP32-S3 (8 MB PSRAM, 16 MB flash)
- **Display:** 4.7" e-paper
- **Storage:** microSD card (FATFS)
- **Connectivity:** Wi-Fi, optionally routed through a VPN for remote Kavita
- **Power:** 3.7 V LiPo (JST-PH 2.0), USB-C for charging / flashing

## Features (planned)

- Read DRM-free EPUBs from SD card
- Text layout and pagination for e-ink display
- Page turn controls with partial refresh and periodic full refresh
- Deep sleep between page turns for long battery life
- Sync reading progress to Kavita (OPDS + Reader API)
- Offline progress queue, flushed on reconnect
- Optional future sync to [Hardcover](https://hardcover.app/)

## Project structure

| Path | Purpose |
|---|---|
| `pc/terminal_reader/` | Terminal EPUB reader prototype |
| `pc/sdl_reader/` | SDL2 paginated reader (future) |
| `esp/` | ESP-IDF firmware (created at porting time) |
| `shared/` | Platform-independent core: EPUB parser, HTML stripper, layout engine, progress persistence |
| `docs/` | Notes, architecture diagrams, API references |

## Prototype milestones

1. **Hello EPUB** ✅: open a book, extract chapters, print the first one.
2. **Terminal reader** ✅: word-wrap, paginate, and navigate chapters with left/right arrows.
3. **Code modularity** ✅: split the monolithic `main.c` into feature-based modules (epub, html, layout, terminal).
4. **Progress persistence** ✅: remember chapter and page across sessions in `~/.local/state/inky/progress/`.
5. **Bottom status bar** ✅: show current chapter page percentage and overall book percentage.
6. **SDL reader** ✅: render pages in a 960x540 SDL2 window with keyboard navigation.
7. **Book library** ✅: selectable Books/ folder, launch without arguments to pick a book.
8. **Kavita auth**: log in and fetch the OPDS catalog.
9. **Download + read**: pull an EPUB from Kavita and open it.
10. **Progress sync**: upload and download reading position with Kavita.
11. **Port to ESP-IDF**: add e-ink and SD card drivers.

## Tech stack

| Task | PC prototype | ESP target |
|---|---|---|
| Language | C / C++ | C / C++ |
| Build system | Makefile | ESP-IDF CMake |
| Display | terminal / SDL2 | GxEPD2 / LOVYANGFX |
| ZIP / EPUB | libzip / miniz | miniz |
| XML | hand-rolled parser | hand-rolled parser |
| JSON | cJSON | cJSON |
| HTTP / HTTPS | libcurl | ESP HTTP client |
| Font | FreeType | FreeType or lighter renderer |
| Filesystem | POSIX | FATFS / SD card |
| Progress storage | `~/.local/state/inky/progress/` | NVS / SD card |
| Tasks | main loop | FreeRTOS tasks |

## Running the prototypes

Both prototypes share the same core engine under `shared/` and use the same progress store (`~/.local/state/inky/progress/`).

### Terminal reader

```bash
cd pc/terminal_reader
make
./inky_term /path/to/book.epub
```

Controls:

| Key | Action |
|---|---|
| `←` (left arrow) | Previous page |
| `→` (right arrow) | Next page |
| `q` | Quit |

### SDL reader

Simulates the 960×540 LilyGo T5 4.7" S3 e-paper display. Uses a monospace font so the cell-based layout engine matches the rendered text.

```bash
cd pc/sdl_reader
make
./inky_sdl /path/to/book.epub
```

Controls:

| Key | Action |
|---|---|
| `←` (left arrow) / `Page Up` | Previous page |
| `→` (right arrow) / `Page Down` / `Space` | Next page |
| `q` / `Esc` | Quit |

At chapter boundaries, the navigation keys continue into the previous or next chapter. The reader starts at the first entry of the book's spine, then resumes from the last saved position if one exists.

A status bar at the bottom of the screen shows the current chapter's page percentage and the overall book percentage.

## Module layout

| Module | Files | Responsibility |
|---|---|---|
| `epub` | `epub/epub.h`, `epub/epub.c` | Open EPUB ZIP, read `container.xml` / `.opf`, build chapter list from spine |
| `html` | `html/html.h`, `html/html.c` | Strip HTML tags and collapse whitespace into plain text |
| `layout` | `layout/layout.h`, `layout/layout.c` | Cell-based word-wrap and pagination (`rows × cols`) |
| `progress` | `progress/progress.h`, `progress/progress.c` | Save/load reading position to XDG state directory |
| `terminal` | `terminal/terminal.h`, `terminal/terminal.c` | Terminal size, raw input, and keyboard input abstraction |

## Development notes

- The PC prototype lets us iterate on parsing, layout, and the UI without flashing hardware.
- Platform-specific code lives behind clean interfaces so the same core can run on Linux and ESP32.
- Keep EPUBs text-only for the first iterations; image support may come later.
- Progress files are stored in `$XDG_STATE_HOME/inky/progress/` (usually `~/.local/state/inky/progress/`) rather than as sidecars next to EPUBs, so book directories stay clean.

## Useful resources

- [ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/)
- [miniz](https://github.com/richgel999/miniz)
- [cJSON](https://github.com/DaveGamble/cJSON)
- [SDL2](https://www.libsdl.org/)
- [Kavita API docs](https://www.kavitareader.com/)
