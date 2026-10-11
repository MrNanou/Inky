/*
 * epub.c
 *
 * EPUB parsing implementation.
 */

#include "epub/epub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration of internal ZIP reader. */
static char *read_zip_file(zip_t *archive, const char *filename, size_t *out_len);

/*
 * Search for an attribute inside an XML tag and copy its value into out_value.
 *
 * We scan the XML text for: attribute="value" or attribute='value'.
 * This is enough for EPUB metadata files because they never contain tricky
 * XML features like namespaces, comments inside tags, or entity references in
 * attribute values.
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
 * Read a file from the ZIP archive into a freshly allocated NUL-terminated
 * buffer. Returns a pointer to the buffer and writes its size (without the
 * extra NUL byte) into *out_len. The caller must free() the buffer.
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
 * Build a list of all XHTML chapter file paths in spine reading order.
 *
 * We parse the spine, skip items that are not XHTML, resolve each href
 * relative to the .opf directory, and store the resulting paths in an array.
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

    /* Count itemrefs to know how many entries to allocate. */
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

        /* Keep only XHTML chapters for now. */
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

char *epub_read_file(epub_t *epub, const char *filename, size_t *out_len)
{
    return read_zip_file(epub->archive, filename, out_len);
}

epub_t *epub_open(const char *filename)
{
    int zip_error_code = 0;
    zip_t *archive = zip_open(filename, ZIP_RDONLY, &zip_error_code);
    if (!archive) {
        fprintf(stderr, "Cannot open '%s' as ZIP (error %d)\n",
                filename, zip_error_code);
        return NULL;
    }

    char opf_path[512];
    if (!find_package_path(archive, opf_path, sizeof(opf_path))) {
        zip_close(archive);
        return NULL;
    }

    chapter_entry_t *chapters = NULL;
    int chapter_count = build_chapter_list(archive, opf_path, &chapters);
    if (chapter_count == 0) {
        fprintf(stderr, "No readable chapters found\n");
        zip_close(archive);
        return NULL;
    }

    epub_t *epub = malloc(sizeof(epub_t));
    if (!epub) {
        for (int i = 0; i < chapter_count; i++) {
            free(chapters[i].path);
            free(chapters[i].idref);
        }
        free(chapters);
        zip_close(archive);
        return NULL;
    }

    epub->archive = archive;
    epub->chapters = chapters;
    epub->chapter_count = chapter_count;
    return epub;
}

void epub_close(epub_t *epub)
{
    if (!epub) {
        return;
    }

    if (epub->chapters) {
        for (int i = 0; i < epub->chapter_count; i++) {
            free(epub->chapters[i].path);
            free(epub->chapters[i].idref);
        }
        free(epub->chapters);
    }

    if (epub->archive) {
        zip_close(epub->archive);
    }

    free(epub);
}
