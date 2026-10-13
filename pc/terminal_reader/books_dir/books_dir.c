/*
 * books_dir.c
 *
 * Locate the Books/ directory from various run locations.
 */

#include "books_dir/books_dir.h"

#include <sys/stat.h>
#include <stdio.h>

static int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *find_books_dir(void)
{
    static const char *candidates[] = {
        "Books",
        "../Books",
        "../../Books",
        NULL,
    };

    for (int i = 0; candidates[i] != NULL; i++) {
        if (dir_exists(candidates[i])) {
            return candidates[i];
        }
    }

    return NULL;
}
