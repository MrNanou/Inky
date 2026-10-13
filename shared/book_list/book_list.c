/*
 * book_list.c
 *
 * Scans a directory for EPUB files.
 */

#include "book_list/book_list.h"

#include "../epub/epub.h"
#include "../progress/progress.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Case-insensitive check if `name` ends with `suffix`.
 */
static int ends_with_ignore_case(const char *name, const char *suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);

    if (name_len < suffix_len) {
        return 0;
    }

    const char *tail = name + name_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        char a = tail[i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z') {
            a = a - 'A' + 'a';
        }
        if (b >= 'A' && b <= 'Z') {
            b = b - 'A' + 'a';
        }

        if (a != b) {
            return 0;
        }
    }

    return 1;
}

int book_list_scan(const char *dir, book_entry_t **entries)
{
    if (!dir || !entries) {
        return -1;
    }

    *entries = NULL;

    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }

    int capacity = 16;
    int count = 0;
    book_entry_t *list = calloc(capacity, sizeof(book_entry_t));
    if (!list) {
        closedir(d);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) {
            continue;
        }

        if (!ends_with_ignore_case(entry->d_name, ".epub")) {
            continue;
        }

        if (count >= capacity) {
            int new_capacity = capacity * 2;
            book_entry_t *new_list = realloc(list, new_capacity * sizeof(book_entry_t));
            if (!new_list) {
                book_list_free(list, count);
                closedir(d);
                return -1;
            }
            list = new_list;
            capacity = new_capacity;
        }

        size_t path_len = strlen(dir) + 1 + strlen(entry->d_name) + 1;
        list[count].path = malloc(path_len);
        /*
         * Store a display name without the .epub extension.
         */
        size_t name_len = strlen(entry->d_name);
        if (name_len > 5 && ends_with_ignore_case(entry->d_name, ".epub")) {
            list[count].filename = malloc(name_len - 4);  /* strip ".epub" */
            if (list[count].filename) {
                memcpy(list[count].filename, entry->d_name, name_len - 5);
                list[count].filename[name_len - 5] = '\0';
            }
        } else {
            list[count].filename = strdup(entry->d_name);
        }

        if (!list[count].path || !list[count].filename) {
            book_list_free(list, count + 1);
            closedir(d);
            return -1;
        }

        snprintf(list[count].path, path_len, "%s/%s", dir, entry->d_name);

        /*
         * Look up saved progress for this book and compute a global percentage.
         * We briefly open the EPUB to get the chapter count; this is acceptable
         * for a small local library.
         */
        int chapter = 0;
        int page = 0;
        list[count].has_progress = progress_load(list[count].path, &chapter, &page);
        list[count].progress_pct = 0;

        if (list[count].has_progress) {
            epub_t *epub = epub_open(list[count].path);
            if (epub) {
                int total_chapters = epub->chapter_count;
                if (total_chapters > 0) {
                    /* Approximate: current chapter over total chapters. */
                    list[count].progress_pct =
                        ((chapter + 1) * 100) / total_chapters;
                    if (list[count].progress_pct > 100) {
                        list[count].progress_pct = 100;
                    }
                }
                epub_close(epub);
            }
        }

        count++;
    }

    closedir(d);
    *entries = list;
    return count;
}

void book_list_free(book_entry_t *entries, int count)
{
    if (!entries) {
        return;
    }

    for (int i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].filename);
    }

    free(entries);
}
