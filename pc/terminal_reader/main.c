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
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <zip.h>

/*
 * Layout state.
 *
 * The layout engine takes plain text and a column width and breaks it into
 * wrapped lines. It then groups those lines into pages of a given row height.
 *
 * This is intentionally cell-based (rows x cols) rather than pixel-based so
 * the same engine can later be driven by font metrics on SDL2 / e-ink.
 */
typedef struct {
    char *text;           /* NUL-terminated plain text (owned by layout) */
    int *line_offsets;    /* byte offset in text where each wrapped line starts */
    int line_count;       /* number of wrapped lines */
    int line_capacity;    /* allocated size of line_offsets */
    int cols;             /* column width used to build the layout */
} layout_t;

/*
 * A chapter as listed in the spine: its idref and its path inside the ZIP.
 */
typedef struct {
    char *idref;
    char *path;
} chapter_entry_t;

/*
 * Global-ish state for the reader. We group it so helper functions can load
 * chapters without needing a dozen parameters.
 */
typedef struct {
    zip_t *archive;
    chapter_entry_t *chapters;
    int chapter_count;
    int current_chapter;
    int current_page;

    /* Terminal / screen dimensions. */
    int rows;
    int cols;

    /* Current chapter content. */
    char *chapter_text;
    layout_t *layout;
} reader_t;

/*
 * Read a file from the ZIP archive into a freshly allocated NUL-terminated
 * buffer. Returns a pointer to the buffer and writes its size (without the
 * extra NUL byte) into *out_len. The caller must free() the buffer.
 *
 * On error returns NULL.
 */
static char *read_zip_file(zip_t *archive, const char *filename, size_t *out_len)
{
    zip_stat_t stat_info;
    if (zip_stat(archive, filename, 0, &stat_info) != 0) {
        fprintf(stderr, "Cannot stat '%s' in archive: %s\n",
                filename, zip_strerror(archive));
        return NULL;
    }

    zip_file_t *file = zip_fopen(archive, filename, 0);
    if (!file) {
        fprintf(stderr, "Cannot open '%s' in archive\n", filename);
        return NULL;
    }

    char *buffer = malloc(stat_info.size + 1);
    if (!buffer) {
        zip_fclose(file);
        return NULL;
    }

    zip_int64_t read_bytes = zip_fread(file, buffer, stat_info.size);
    zip_fclose(file);

    if (read_bytes != (zip_int64_t)stat_info.size) {
        fprintf(stderr, "Failed to read full content of '%s'\n", filename);
        free(buffer);
        return NULL;
    }

    buffer[stat_info.size] = '\0';
    if (out_len) {
        *out_len = stat_info.size;
    }
    return buffer;
}

/*
 * Search for an attribute inside an XML tag and copy its value into out_value.
 *
 * We scan the XML text for: attribute="value" or attribute='value'.
 * This is enough for EPUB metadata files because they never contain tricky
 * XML features like namespaces, comments inside tags, or entity references in
 * attribute values.
 *
 * Returns 1 if the attribute was found, 0 otherwise.
 */
static int get_xml_attribute(const char *xml, const char *tag_name,
                             const char *attr_name,
                             char *out_value, size_t out_size)
{
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "<%s", tag_name);
    size_t pattern_len = strlen(pattern);

    const char *pos = xml;
    while ((pos = strstr(pos, pattern)) != NULL) {
        pos += pattern_len;

        /* The character after the tag name must be whitespace, '>', or '/'. */
        if (*pos != ' ' && *pos != '\t' && *pos != '\r' &&
            *pos != '\n' && *pos != '>' && *pos != '/') {
            continue;
        }

        /* Look for the requested attribute inside this opening tag. */
        char attr_pattern[256];
        snprintf(attr_pattern, sizeof(attr_pattern), "%s=\"", attr_name);
        const char *attr_start = strstr(pos, attr_pattern);

        if (!attr_start) {
            snprintf(attr_pattern, sizeof(attr_pattern), "%s='", attr_name);
            attr_start = strstr(pos, attr_pattern);
            if (!attr_start) {
                continue;
            }
        }

        /* The attribute must belong to the current tag, before its '>'. */
        const char *tag_end = strchr(pos, '>');
        if (tag_end && attr_start > tag_end) {
            continue;
        }

        attr_start += strlen(attr_pattern);
        char quote = attr_pattern[strlen(attr_pattern) - 1];

        const char *attr_end = strchr(attr_start, quote);
        if (!attr_end) {
            return 0;
        }

        size_t len = (size_t)(attr_end - attr_start);
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out_value, attr_start, len);
        out_value[len] = '\0';
        return 1;
    }

    return 0;
}

/*
 * Parse META-INF/container.xml to find the path of the package (.opf) file.
 */
static int find_package_path(zip_t *archive, char *out_path, size_t out_size)
{
    size_t len;
    char *container = read_zip_file(archive, "META-INF/container.xml", &len);
    if (!container) {
        return 0;
    }

    int found = get_xml_attribute(container, "rootfile", "full-path",
                                  out_path, out_size);
    free(container);

    if (!found) {
        fprintf(stderr, "Could not find rootfile full-path in container.xml\n");
    }
    return found;
}

/*
 * Look up one manifest item by its id and return its href attribute.
 */
static int find_manifest_href(const char *opf, const char *idref,
                              char *out_path, size_t out_size)
{
    const char *manifest_pos = strstr(opf, "<manifest");
    const char *manifest_end = strstr(opf, "</manifest>");

    if (!manifest_pos || !manifest_end) {
        fprintf(stderr, "Could not locate <manifest> section\n");
        return 0;
    }

    const char *item_pos = manifest_pos;
    while ((item_pos = strstr(item_pos, "<item")) != NULL) {
        if (item_pos >= manifest_end) {
            break;
        }

        char item_id[128];
        if (get_xml_attribute(item_pos, "item", "id",
                              item_id, sizeof(item_id))) {
            if (strcmp(item_id, idref) == 0) {
                return get_xml_attribute(item_pos, "item", "href",
                                         out_path, out_size);
            }
        }

        item_pos += 5;
    }

    return 0;
}

/*
 * Build a list of all chapter file paths in spine reading order.
 *
 * We parse the spine, skip items that are not XHTML (e.g. images), resolve
 * each href relative to the .opf directory, and store the resulting paths in
 * an array. The array and its strings are allocated on the heap; the caller
 * must free them.
 *
 * On success returns the number of chapters and writes *chapters_out.
 * On failure returns 0.
 */
static int build_chapter_list(zip_t *archive, const char *opf_path,
                              chapter_entry_t **chapters_out)
{
    size_t opf_len;
    char *opf = read_zip_file(archive, opf_path, &opf_len);
    if (!opf) {
        return 0;
    }

    const char *spine_pos = strstr(opf, "<spine");
    const char *spine_end = strstr(opf, "</spine>");
    if (!spine_pos || !spine_end) {
        fprintf(stderr, "Could not locate <spine> section\n");
        free(opf);
        return 0;
    }

    /*
     * First pass: count how many XHTML itemrefs are in the spine so we know
     * how many strings to allocate.
     */
    int count = 0;
    const char *itemref_pos = spine_pos;
    while ((itemref_pos = strstr(itemref_pos, "<itemref")) != NULL) {
        if (itemref_pos >= spine_end) {
            break;
        }
        count++;
        itemref_pos += 5;
    }

    if (count == 0) {
        fprintf(stderr, "Spine is empty\n");
        free(opf);
        return 0;
    }

    chapter_entry_t *chapters = calloc(count, sizeof(chapter_entry_t));
    if (!chapters) {
        free(opf);
        return 0;
    }

    /*
     * Second pass: extract each idref, look up its manifest href, and build
     * the full path inside the ZIP.
     */
    int actual = 0;
    itemref_pos = spine_pos;
    while ((itemref_pos = strstr(itemref_pos, "<itemref")) != NULL) {
        if (itemref_pos >= spine_end) {
            break;
        }

        char idref[128];
        if (!get_xml_attribute(itemref_pos, "itemref", "idref",
                               idref, sizeof(idref))) {
            itemref_pos += 5;
            continue;
        }

        char href[512];
        if (!find_manifest_href(opf, idref, href, sizeof(href))) {
            fprintf(stderr, "Warning: no manifest entry for idref '%s'\n", idref);
            itemref_pos += 5;
            continue;
        }

        /*
         * Only keep XHTML content for now. EPUBs sometimes reference images
         * or font files in the spine, which we cannot render as text.
         */
        if (strstr(href, ".xhtml") == NULL && strstr(href, ".html") == NULL &&
            strstr(href, ".htm") == NULL) {
            itemref_pos += 5;
            continue;
        }

        /* Resolve href relative to the .opf directory. */
        char opf_dir[512];
        strncpy(opf_dir, opf_path, sizeof(opf_dir) - 1);
        opf_dir[sizeof(opf_dir) - 1] = '\0';
        char *last_slash = strrchr(opf_dir, '/');
        if (last_slash) {
            last_slash[1] = '\0';
        } else {
            opf_dir[0] = '\0';
        }

        char *path = malloc(strlen(opf_dir) + strlen(href) + 1);
        if (!path) {
            break;
        }
        sprintf(path, "%s%s", opf_dir, href);
        chapters[actual].path = path;
        chapters[actual].idref = strdup(idref);
        actual++;

        itemref_pos += 5;
    }

    free(opf);

    if (actual == 0) {
        free(chapters);
        return 0;
    }

    *chapters_out = chapters;
    return actual;
}

/*
 * Convert a buffer of HTML/XHTML into plain text.
 *
 * We remove everything inside <tags>, collapse consecutive whitespace,
 * and decode the basic XML entities. Block-level tags (headings, paragraphs)
 * get extra newlines so the output is readable.
 */
static void html_to_text(const char *html, char *out, size_t out_size)
{
    size_t j = 0;
    int inside_tag = 0;
    int last_was_space = 1;

    for (size_t i = 0; html[i] != '\0' && j + 1 < out_size; i++) {
        char c = html[i];

        if (c == '<') {
            inside_tag = 1;
            continue;
        }
        if (c == '>') {
            inside_tag = 0;

            /* Insert blank lines after block-level end tags. */
            if (j > 0) {
                const char *prev = &html[i];
                int block = 0;
                if (strncasecmp(prev - 2, "/p", 2) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h1", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h2", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h3", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h4", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h5", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h6", 3) == 0) block = 1;
                else if (prev - 4 >= html && strncasecmp(prev - 4, "/div", 4) == 0) block = 1;

                if (block) {
                    if (out[j - 1] != '\n') {
                        out[j++] = '\n';
                    }
                    out[j++] = '\n';
                    last_was_space = 1;
                }
            }
            continue;
        }

        if (inside_tag) {
            continue;
        }

        /* Decode basic XML entities. */
        if (c == '&') {
            if (strncmp(&html[i + 1], "amp;", 4) == 0) {
                c = '&';  i += 4;
            } else if (strncmp(&html[i + 1], "lt;", 3) == 0) {
                c = '<';  i += 3;
            } else if (strncmp(&html[i + 1], "gt;", 3) == 0) {
                c = '>';  i += 3;
            } else if (strncmp(&html[i + 1], "quot;", 5) == 0) {
                c = '"';  i += 5;
            } else if (strncmp(&html[i + 1], "apos;", 5) == 0) {
                c = '\''; i += 5;
            }
        }

        /* Collapse all whitespace to a single space. */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_was_space && j + 1 < out_size) {
                out[j++] = ' ';
                last_was_space = 1;
            }
            continue;
        }

        out[j++] = c;
        last_was_space = 0;
    }

    out[j] = '\0';

    if (j > 0 && out[j - 1] == ' ') {
        out[j - 1] = '\0';
    }
}

/*
 * Create a new empty layout.
 */
static layout_t *layout_new(void)
{
    return calloc(1, sizeof(layout_t));
}

/*
 * Free all memory owned by a layout.
 */
static void layout_free(layout_t *layout)
{
    if (!layout) {
        return;
    }
    free(layout->text);
    free(layout->line_offsets);
    free(layout);
}

/*
 * Append a line offset to the layout, growing the array as needed.
 */
static int layout_add_line(layout_t *layout, int offset)
{
    if (layout->line_count >= layout->line_capacity) {
        int new_capacity = layout->line_capacity == 0 ? 256 : layout->line_capacity * 2;
        int *new_offsets = realloc(layout->line_offsets,
                                   new_capacity * sizeof(int));
        if (!new_offsets) {
            return 0;
        }
        layout->line_offsets = new_offsets;
        layout->line_capacity = new_capacity;
    }

    layout->line_offsets[layout->line_count++] = offset;
    return 1;
}

/*
 * Build a layout by word-wrapping plain text to a fixed column width.
 *
 * We walk through the text one word at a time. A word ends at whitespace.
 * If adding the next word would exceed the column width, we start a new line.
 * Newlines in the source text are preserved as forced line breaks.
 *
 * Returns 1 on success, 0 on failure.
 */
static int layout_build(layout_t *layout, const char *text, int cols)
{
    if (!layout || !text || cols <= 0) {
        return 0;
    }

    /* Reset any previous content. */
    free(layout->text);
    free(layout->line_offsets);
    layout->text = NULL;
    layout->line_offsets = NULL;
    layout->line_count = 0;
    layout->line_capacity = 0;
    layout->cols = cols;

    size_t text_len = strlen(text);
    layout->text = malloc(text_len + 1);
    if (!layout->text) {
        return 0;
    }
    memcpy(layout->text, text, text_len + 1);

    /* Always start with a first line. */
    if (!layout_add_line(layout, 0)) {
        return 0;
    }

    int line_len = 0;
    int word_start = -1;
    int word_len = 0;

    for (int i = 0; layout->text[i] != '\0'; i++) {
        char c = layout->text[i];

        if (c == '\n') {
            /* Force a new line at the newline. */
            layout->text[i] = '\0';
            if (!layout_add_line(layout, i + 1)) {
                return 0;
            }
            line_len = 0;
            word_start = -1;
            word_len = 0;
            continue;
        }

        if (c == ' ') {
            /* End of a word. */
            if (word_start >= 0) {
                /*
                 * If this word doesn't fit on the current line, wrap before
                 * it. The line we've built so far ends where the word started.
                 */
                if (line_len > 0 && line_len + 1 + word_len > cols) {
                    layout->text[word_start - 1] = '\0';
                    if (!layout_add_line(layout, word_start)) {
                        return 0;
                    }
                    line_len = word_len;
                } else {
                    line_len += 1 + word_len;  /* space + word */
                }
                word_start = -1;
                word_len = 0;
            }
            continue;
        }

        if (word_start < 0) {
            word_start = i;
            word_len = 0;
        }
        word_len++;
    }

    /* Handle the final word if the text doesn't end with whitespace. */
    if (word_start >= 0) {
        if (line_len > 0 && line_len + 1 + word_len > cols) {
            layout->text[word_start - 1] = '\0';
            if (!layout_add_line(layout, word_start)) {
                return 0;
            }
        }
    }

    return 1;
}

/*
 * Return the number of pages for a given row height.
 * We reserve one row at the top for the status line, so the content area is
 * rows - 1 lines tall.
 */
static int layout_page_count(const layout_t *layout, int rows)
{
    if (!layout || rows <= 1) {
        return 0;
    }
    int content_rows = rows - 1;
    return (layout->line_count + content_rows - 1) / content_rows;
}

/*
 * Print one page of the layout.
 *
 * The terminal has `rows` rows and `cols` columns. Row 0 is used for a status
 * line showing the chapter and page. The remaining rows - 1 lines are filled
 * from the layout.
 */
static void layout_render_page(const layout_t *layout, int page, int rows,
                               const char *chapter_name,
                               int chapter_index, int chapter_count)
{
    if (!layout || rows <= 1 || page < 0 || page >= layout_page_count(layout, rows)) {
        return;
    }

    /* Clear the screen and move cursor to top-left. */
    printf("\033[2J\033[H");

    int content_rows = rows - 1;
    int first_line = page * content_rows;
    int last_line = first_line + content_rows;
    if (last_line > layout->line_count) {
        last_line = layout->line_count;
    }

    /* Status line. */
    printf("\033[7m %s  |  page %d / %d  |  chapter %d / %d \033[0m\n",
           chapter_name,
           page + 1, layout_page_count(layout, rows),
           chapter_index + 1, chapter_count);

    /* Content lines. */
    for (int i = first_line; i < last_line; i++) {
        printf("%s\n", layout->text + layout->line_offsets[i]);
    }

    /* Clear any remaining rows below the content to avoid old text showing. */
    for (int i = last_line - first_line; i < content_rows; i++) {
        printf("\033[K\n");
    }

    fflush(stdout);
}

/*
 * Load a chapter by index into the reader state. This frees the previous
 * chapter's text and layout, reads the new HTML, converts it to plain text,
 * and builds a fresh layout.
 */
static void reader_load_chapter(reader_t *reader, int index)
{
    layout_free(reader->layout);
    reader->layout = NULL;
    free(reader->chapter_text);
    reader->chapter_text = NULL;
    reader->current_page = 0;

    if (index < 0 || index >= reader->chapter_count) {
        return;
    }

    size_t chapter_len;
    char *chapter_html = read_zip_file(reader->archive,
                                       reader->chapters[index].path,
                                       &chapter_len);
    if (!chapter_html) {
        printf("\033[2J\033[H[Could not read chapter]\n");
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
        printf("\033[2J\033[H[Out of memory]\n");
        return;
    }

    html_to_text(chapter_html, reader->chapter_text, text_size);
    free(chapter_html);

    reader->layout = layout_new();
    if (!reader->layout || !layout_build(reader->layout, reader->chapter_text,
                                         reader->cols)) {
        layout_free(reader->layout);
        reader->layout = NULL;
        printf("\033[2J\033[H[Layout failed]\n");
        return;
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
 * Terminal input helpers.
 *
 * We put the terminal into raw-ish mode so we can read arrow keys as escape
 * sequences without waiting for Enter. This is only for the PC prototype;
 * the ESP32 will use physical buttons instead.
 */
static struct termios original_termios;

static void terminal_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
}

static void terminal_setup_raw(void)
{
    tcgetattr(STDIN_FILENO, &original_termios);
    atexit(terminal_restore);

    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/*
 * Read a single key press. Arrow keys come in as a 3-byte escape sequence:
 *   Up:    ESC [ A
 *   Down:  ESC [ B
 *   Right: ESC [ C
 *   Left:  ESC [ D
 */
#define KEY_QUIT  0
#define KEY_LEFT  1
#define KEY_RIGHT 2
#define KEY_OTHER 3

static int read_key(void)
{
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return KEY_OTHER;
    }

    if (c == 'q' || c == 'Q') {
        return KEY_QUIT;
    }

    /* Escape sequence - try to read the next two bytes. */
    if (c == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_OTHER;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_OTHER;

        if (seq[0] == '[') {
            if (seq[1] == 'C') return KEY_RIGHT;
            if (seq[1] == 'D') return KEY_LEFT;
        }
    }

    return KEY_OTHER;
}

/*
 * Query the terminal size using the TIOCGWINSZ ioctl.
 * Returns 1 on success, 0 on failure.
 */
static int get_terminal_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return 0;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <book.epub>\n", argv[0]);
        return 1;
    }

    reader_t reader = {0};

    if (!get_terminal_size(&reader.rows, &reader.cols)) {
        reader.rows = 24;
        reader.cols = 80;
    }

    if (reader.rows < 3 || reader.cols < 20) {
        fprintf(stderr, "Terminal is too small (need at least 3x20)\n");
        return 1;
    }

    int zip_error_code = 0;
    reader.archive = zip_open(argv[1], ZIP_RDONLY, &zip_error_code);
    if (!reader.archive) {
        fprintf(stderr, "Cannot open '%s' as ZIP (error %d)\n",
                argv[1], zip_error_code);
        return 1;
    }

    /* Find the .opf package file. */
    char opf_path[512];
    if (!find_package_path(reader.archive, opf_path, sizeof(opf_path))) {
        zip_close(reader.archive);
        return 1;
    }

    /* Build the ordered list of chapter file paths from the spine. */
    reader.chapter_count = build_chapter_list(reader.archive, opf_path,
                                              &reader.chapters);
    if (reader.chapter_count == 0) {
        fprintf(stderr, "No readable chapters found\n");
        zip_close(reader.archive);
        return 1;
    }

    /* Start at the first chapter of the spine. */
    reader.current_chapter = 0;

    terminal_setup_raw();
    reader_load_chapter(&reader, reader.current_chapter);

    int running = 1;
    while (running) {
        /* Re-render before reading input so resizes take effect. */
        if (reader.layout) {
            const char *chapter_name = reader.chapters[reader.current_chapter].idref;
            layout_render_page(reader.layout, reader.current_page, reader.rows,
                               chapter_name, reader.current_chapter,
                               reader.chapter_count);
        }

        int key = read_key();

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
            } else if (reader.current_chapter < reader.chapter_count - 1) {
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
        if (get_terminal_size(&new_rows, &new_cols)) {
            if (new_rows != reader.rows || new_cols != reader.cols) {
                reader.rows = new_rows;
                reader.cols = new_cols;
                reader_relayout(&reader);
            }
        }
    }

    printf("\033[2J\033[HGoodbye.\n");

    layout_free(reader.layout);
    free(reader.chapter_text);
    for (int i = 0; i < reader.chapter_count; i++) {
        free(reader.chapters[i].path);
        free(reader.chapters[i].idref);
    }
    free(reader.chapters);

    zip_close(reader.archive);
    return 0;
}
