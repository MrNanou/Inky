/*
 * Inky terminal EPUB reader - prototype step 2.
 *
 * Goal: open an EPUB, find the first readable chapter, and let the user
 * navigate chapters with the left/right arrow keys. Press 'q' to quit.
 *
 * This is a terminal prototype. The real e-reader will paginate text within
 * a screen-sized area; here we just print one chapter at a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <zip.h>

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
 * Heuristic: is this spine idref front/back matter that should be skipped
 * when searching for the first readable chapter?
 */
static int is_front_or_back_matter(const char *idref)
{
    static const char *skip_prefixes[] = {
        "cvi", "cov", "tp", "cop", "toc", "map", "col",
        "ded", "epi", "ack", "ata", "front", "back", "bm",
        NULL
    };

    for (int i = 0; skip_prefixes[i] != NULL; i++) {
        size_t len = strlen(skip_prefixes[i]);
        if (strncmp(idref, skip_prefixes[i], len) == 0) {
            return 1;
        }
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
                              char ***chapters_out)
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

    char **chapters = calloc(count, sizeof(char *));
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
        chapters[actual++] = path;

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
 * get extra newlines so the output is readable in a terminal.
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

            /*
             * Insert blank lines after block-level end tags to make headings
             * and paragraphs stand apart in the terminal.
             */
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
                else if (strncasecmp(prev - 2, "/div", 4) == 0) block = 1;

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
 * Print a chapter by its index in the chapter list.
 * Clears the terminal first so each chapter appears on a fresh screen.
 */
static void print_chapter(zip_t *archive, char **chapters, int chapter_count,
                          int index)
{
    if (index < 0 || index >= chapter_count) {
        return;
    }

    /* ANSI escape sequence to clear the screen and move cursor to top-left. */
    printf("\033[2J\033[H");
    printf("=== Chapter %d / %d: %s ===\n\n", index + 1, chapter_count, chapters[index]);

    size_t chapter_len;
    char *chapter_html = read_zip_file(archive, chapters[index], &chapter_len);
    if (!chapter_html) {
        printf("[Could not read chapter]\n");
        return;
    }

    size_t text_size = chapter_len * 2 + 1;
    char *text = malloc(text_size);
    if (!text) {
        free(chapter_html);
        printf("[Out of memory]\n");
        return;
    }

    html_to_text(chapter_html, text, text_size);
    printf("%s\n", text);

    free(text);
    free(chapter_html);
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
 * We return simple codes for the caller to interpret.
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

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <book.epub>\n", argv[0]);
        return 1;
    }

    int zip_error_code = 0;
    zip_t *archive = zip_open(argv[1], ZIP_RDONLY, &zip_error_code);
    if (!archive) {
        fprintf(stderr, "Cannot open '%s' as ZIP (error %d)\n",
                argv[1], zip_error_code);
        return 1;
    }

    /* Find the .opf package file. */
    char opf_path[512];
    if (!find_package_path(archive, opf_path, sizeof(opf_path))) {
        zip_close(archive);
        return 1;
    }

    /* Build the ordered list of chapter file paths from the spine. */
    char **chapters = NULL;
    int chapter_count = build_chapter_list(archive, opf_path, &chapters);
    if (chapter_count == 0) {
        fprintf(stderr, "No readable chapters found\n");
        zip_close(archive);
        return 1;
    }

    /* Find the first chapter that is not front/back matter. */
    int current = 0;
    while (current < chapter_count) {
        /* Extract the idref-ish filename tail to check for front matter. */
        const char *filename = strrchr(chapters[current], '/');
        if (!filename) filename = chapters[current];
        else filename++;

        /*
         * Simple heuristic: if the filename contains skipped prefixes,
         * advance. Real front/back detection would need idrefs; this is close
         * enough for a prototype because filenames usually echo idrefs.
         */
        if (is_front_or_back_matter(filename)) {
            current++;
        } else {
            break;
        }
    }

    terminal_setup_raw();

    print_chapter(archive, chapters, chapter_count, current);

    int running = 1;
    while (running) {
        int key = read_key();

        switch (key) {
        case KEY_QUIT:
            running = 0;
            break;
        case KEY_LEFT:
            if (current > 0) {
                current--;
                print_chapter(archive, chapters, chapter_count, current);
            }
            break;
        case KEY_RIGHT:
            if (current < chapter_count - 1) {
                current++;
                print_chapter(archive, chapters, chapter_count, current);
            }
            break;
        default:
            /* Ignore unknown keys. */
            break;
        }
    }

    /* Clear screen and show a goodbye message after restoring the terminal. */
    printf("\033[2J\033[H");
    printf("Goodbye.\n");

    /* Free the chapter list. */
    for (int i = 0; i < chapter_count; i++) {
        free(chapters[i]);
    }
    free(chapters);

    zip_close(archive);
    return 0;
}
