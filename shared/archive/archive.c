/*
 * archive.c
 *
 * PC implementation of the archive abstraction using libzip.
 */

#include "archive/archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zip.h>

struct inky_archive {
    zip_t *zip;
};

inky_archive_t *archive_open(const char *filename)
{
    if (!filename) {
        return NULL;
    }

    inky_archive_t *archive = calloc(1, sizeof(*archive));
    if (!archive) {
        return NULL;
    }

    int zip_error_code = 0;
    archive->zip = zip_open(filename, ZIP_RDONLY, &zip_error_code);
    if (!archive->zip) {
        fprintf(stderr, "Cannot open '%s' as ZIP (error %d)\n", filename, zip_error_code);
        free(archive);
        return NULL;
    }

    return archive;
}

void archive_close(inky_archive_t *archive)
{
    if (!archive) {
        return;
    }

    if (archive->zip) {
        zip_close(archive->zip);
    }
    free(archive);
}

char *archive_read_file(inky_archive_t *archive, const char *filename, size_t *out_len)
{
    if (!archive || !archive->zip || !filename || !out_len) {
        return NULL;
    }

    zip_stat_t stat_info;
    if (zip_stat(archive->zip, filename, 0, &stat_info) != 0) {
        fprintf(stderr, "Cannot stat '%s' in archive: %s\n",
                filename, zip_strerror(archive->zip));
        return NULL;
    }

    zip_file_t *file = zip_fopen(archive->zip, filename, 0);
    if (!file) {
        fprintf(stderr, "Cannot open '%s' in archive: %s\n",
                filename, zip_strerror(archive->zip));
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
        fprintf(stderr, "Short read from archive for '%s'\n", filename);
        free(buffer);
        return NULL;
    }

    buffer[stat_info.size] = '\0';
    *out_len = stat_info.size;
    return buffer;
}
