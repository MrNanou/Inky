/*
 * progress.c
 *
 * Reading progress persistence implementation.
 *
 * Progress is stored in a central location rather than as a sidecar next to
 * the EPUB file. This keeps the user's book directories clean.
 *
 * On Linux we use $XDG_STATE_HOME/inky/progress/ (falling back to
 * ~/.local/state/inky/progress/). Each EPUB gets a file named after a simple
 * hash of its full path.
 *
 * On ESP32 this will later be replaced with NVS or an SD card directory.
 */

#include "progress/progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

/*
 * Very simple hash of a string. Not cryptographic; just maps paths to short
 * filenames. Collisions are acceptable for a prototype because the file
 * contents also store the path.
 */
static unsigned long hash_path(const char *path)
{
    unsigned long hash = 5381;
    int c;

    while ((c = (unsigned char)*path++) != '\0') {
        hash = ((hash << 5) + hash) + c;  /* hash * 33 + c */
    }

    return hash;
}

/*
 * Get the directory that holds progress files.
 * Uses $XDG_STATE_HOME/inky/progress if set, otherwise ~/.local/state/inky/progress.
 * The returned buffer is heap-allocated; caller must free.
 */
static char *progress_dir_path(void)
{
    const char *xdg_state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char *dir = NULL;

    if (xdg_state && xdg_state[0] != '\0') {
        size_t len = strlen(xdg_state) + strlen("/inky/progress") + 1;
        dir = malloc(len);
        if (dir) {
            snprintf(dir, len, "%s/inky/progress", xdg_state);
        }
    } else if (home && home[0] != '\0') {
        size_t len = strlen(home) + strlen("/.local/state/inky/progress") + 1;
        dir = malloc(len);
        if (dir) {
            snprintf(dir, len, "%s/.local/state/inky/progress", home);
        }
    }

    return dir;
}

/*
 * Create a directory and all its parent directories (like mkdir -p).
 */
static int mkdir_recursive(const char *path)
{
    if (!path || path[0] == '\0') {
        return 0;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }

    /*
     * Make a mutable copy so we can temporarily null-terminate the parent
     * path.
     */
    size_t len = strlen(path) + 1;
    char *copy = malloc(len);
    if (!copy) {
        return 0;
    }
    memcpy(copy, path, len);

    /* Find the last separator and recurse on the parent directory. */
    char *sep = strrchr(copy, '/');
    if (sep && sep != copy) {
        *sep = '\0';
        if (!mkdir_recursive(copy)) {
            free(copy);
            return 0;
        }
    }

    free(copy);

    return mkdir(path, 0755) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/*
 * Ensure the progress directory exists.
 */
static int ensure_progress_dir(const char *dir)
{
    if (!dir) {
        return 0;
    }

    return mkdir_recursive(dir);
}

/*
 * Build the full path to the progress file for a given EPUB.
 * The returned buffer is heap-allocated; caller must free.
 */
static char *progress_file_path(const char *epub_path)
{
    if (!epub_path) {
        return NULL;
    }

    /*
     * Normalize the path so save and load use the same key regardless of how
     * the book was opened (relative path, absolute path, symlink, etc.).
     */
    char *normalized = realpath(epub_path, NULL);
    const char *key_path = normalized ? normalized : epub_path;

    char *dir = progress_dir_path();
    if (!dir || !ensure_progress_dir(dir)) {
        free(dir);
        free(normalized);
        return NULL;
    }

    /* Filename is a hash of the EPUB path plus the .progress extension. */
    unsigned long h = hash_path(key_path);
    size_t len = strlen(dir) + 32;
    char *path = malloc(len);
    if (!path) {
        free(dir);
        return NULL;
    }

    snprintf(path, len, "%s/%016lx.progress", dir, h);
    free(dir);
    free(normalized);
    return path;
}


/*
 * Format the current UTC time as ISO-8601 into the provided buffer.
 * Returns 1 on success, 0 on failure.
 */
static int format_now_iso8601(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    if (!tm_info) {
        return 0;
    }
    return strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", tm_info) > 0;
}

int progress_load_full(const char *epub_path, int *chapter, int *page,
                       char *updated_at, size_t updated_at_size)
{
    if (!epub_path || !chapter || !page) {
        return 0;
    }

    if (updated_at && updated_at_size > 0) {
        updated_at[0] = '\0';
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

    /*
     * Normalize the caller-provided path so it matches the path stored in
     * the progress file regardless of how the book was opened.
     */
    char *normalized = realpath(epub_path, NULL);
    const char *verify_path = normalized ? normalized : epub_path;

    int loaded_chapter = -1;
    int loaded_page = -1;
    int path_matched = 0;
    char line[2048];

    while (fgets(line, sizeof(line), file)) {
        /* Strip trailing newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        if (strncmp(line, "path=", 5) == 0) {
            if (strcmp(line + 5, verify_path) == 0) {
                path_matched = 1;
            }
        } else if (strncmp(line, "chapter=", 8) == 0) {
            loaded_chapter = atoi(line + 8);
        } else if (strncmp(line, "page=", 5) == 0) {
            loaded_page = atoi(line + 5);
        } else if (strncmp(line, "updated_at=", 11) == 0) {
            if (updated_at && updated_at_size > 0) {
                snprintf(updated_at, updated_at_size, "%s", line + 11);
            }
        }
    }

    fclose(file);
    free(normalized);

    if (!path_matched || loaded_chapter < 0 || loaded_page < 0) {
        return 0;
    }

    *chapter = loaded_chapter;
    *page = loaded_page;
    return 1;
}

int progress_load(const char *epub_path, int *chapter, int *page)
{
    return progress_load_full(epub_path, chapter, page, NULL, 0);
}

int progress_save_full(const char *epub_path, int chapter, int page,
                       const char *updated_at)
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

    fprintf(file, "# Inky progress\n");

    /*
     * Store the normalized path so progress_load can verify it against any
     * spelling of the same file.
     */
    char *normalized_save = realpath(epub_path, NULL);
    const char *stored_path = normalized_save ? normalized_save : epub_path;
    fprintf(file, "path=%s\n", stored_path);
    free(normalized_save);

    fprintf(file, "chapter=%d\npage=%d\n", chapter, page);

    if (updated_at && updated_at[0] != '\0') {
        fprintf(file, "updated_at=%s\n", updated_at);
    } else {
        char now[32];
        if (format_now_iso8601(now, sizeof(now))) {
            fprintf(file, "updated_at=%s\n", now);
        }
    }

    fclose(file);
    free(path);
    return 1;
}

int progress_save(const char *epub_path, int chapter, int page)
{
    return progress_save_full(epub_path, chapter, page, NULL);
}
