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
| `shared/` | Platform-independent core: EPUB parser, layout engine, Kavita client |
| `docs/` | Notes, architecture diagrams, API references |

## Prototype milestones

1. **Hello EPUB** ✅: open a book, extract chapters, print the first one.
2. **Text layout**: word-wrap and paginate for a fixed screen size.
3. **SDL reader**: render pages in a window with Page Up / Page Down.
4. **Kavita auth**: log in and fetch the OPDS catalog.
5. **Download + read**: pull an EPUB from Kavita and open it.
6. **Progress sync**: save and load reading position with Kavita.
7. **Port to ESP-IDF**: add e-ink and SD card drivers.

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
| Tasks | main loop | FreeRTOS tasks |

## Running the terminal reader prototype

The terminal reader opens an EPUB and lets you navigate chapters with the keyboard. It is the first step toward the paginated SDL2 reader.

```bash
cd pc/terminal_reader
make
./inky_term /path/to/book.epub
```

Controls:

| Key | Action |
|---|---|
| `←` (left arrow) | Previous chapter |
| `→` (right arrow) | Next chapter |
| `q` | Quit |

The reader skips common front/back matter (cover, title page, copyright, table of contents, maps, colophon) and starts at the first chapter that looks like real content.

## Development notes

- The PC prototype lets us iterate on parsing, layout, and the UI without flashing hardware.
- Platform-specific code lives behind clean interfaces so the same core can run on Linux and ESP32.
- Keep EPUBs text-only for the first iterations; image support may come later.

## Useful resources

- [ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/)
- [miniz](https://github.com/richgel999/miniz)
- [cJSON](https://github.com/DaveGamble/cJSON)
- [SDL2](https://www.libsdl.org/)
- [Kavita API docs](https://www.kavitareader.com/)
