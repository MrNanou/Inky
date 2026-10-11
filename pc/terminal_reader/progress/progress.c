/*
 * progress.c
 *
 * Reading progress persistence implementation.
 */

#include "progress/progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Build the sidecar path: <epub_path>.inky-progress
 * The returned buffer is heap-allocated; caller must free.
 */
static char *progress_file_path(const char *epub_path)
{
    size_t len = strlen(epub_path) + strlen(".inky-progress") + 1;
    char *path = malloc(len);
    if (!path) {
        return NULL;
    }
    snprintf(path, len, "%s.inky-progress", epub_path);
    return path;
}

int progress_load(const char *epub_path, int *chapter, int *page)
{
    if (!epub_path || !chapter || !page) {
        return 0;
    }

    char *path = progress_file_path(epub_path);
    if (!path) {
        return 0;
    }

    FILE *file = fopen(path, "r");
    free(path);
    if (!file) {
        return 0;
    }

    int loaded_chapter = -1;
    int loaded_page = -1;
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        /* Strip trailing newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        if (strncmp(line, "chapter=", 8) == 0) {
            loaded_chapter = atoi(line + 8);
        } else if (strncmp(line, "page=", 5) == 0) {
            loaded_page = atoi(line + 5);
        }
    }

    fclose(file);

    if (loaded_chapter < 0 || loaded_page < 0) {
        return 0;
    }

    *chapter = loaded_chapter;
    *page = loaded_page;
    return 1;
}

int progress_save(const char *epub_path, int chapter, int page)
{
    if (!epub_path) {
        return 0;
    }

    char *path = progress_file_path(epub_path);
    if (!path) {
        return 0;
    }

    FILE *file = fopen(path, "w");
    if (!file) {
        free(path);
        return 0;
    }

    fprintf(file, "chapter=%d\npage=%d\n", chapter, page);
    fclose(file);
    free(path);
    return 1;
}
