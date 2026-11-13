/*
 * archive_esp.c
 *
 * ESP32 implementation of the archive abstraction using miniz.
 */

#include "archive/archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz_esp.h"

struct inky_archive {
    mz_zip_archive zip;
    void *mem;
    size_t mem_size;
};

static void *load_file_to_memory(const char *filename, size_t *out_size)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    void *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buffer, 1, size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }

    *out_size = size;
    return buffer;
}

inky_archive_t *archive_open(const char *filename)
{
    if (!filename) {
        return NULL;
    }

    inky_archive_t *archive = calloc(1, sizeof(*archive));
    if (!archive) {
        return NULL;
    }

    archive->mem = load_file_to_memory(filename, &archive->mem_size);
    if (!archive->mem) {
        free(archive);
        return NULL;
    }

    mz_zip_zero_struct(&archive->zip);
    if (!mz_zip_reader_init_mem(&archive->zip, archive->mem, archive->mem_size, 0)) {
        free(archive->mem);
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

    mz_zip_reader_end(&archive->zip);
    if (archive->mem) {
        free(archive->mem);
    }
    free(archive);
}

char *archive_read_file(inky_archive_t *archive, const char *filename, size_t *out_len)
{
    if (!archive || !filename || !out_len) {
        return NULL;
    }

    int32_t file_index = mz_zip_reader_locate_file(&archive->zip, filename, NULL, 0);
    if (file_index < 0) {
        return NULL;
    }

    mz_zip_archive_file_stat stat_info;
    if (!mz_zip_reader_file_stat(&archive->zip, file_index, &stat_info)) {
        return NULL;
    }

    size_t size = stat_info.m_uncomp_size;
    char *buffer = (char *)mz_zip_reader_extract_to_heap(&archive->zip, file_index, &size, 0);
    if (!buffer) {
        return NULL;
    }

    /* Ensure NUL termination for text files. */
    char *nul_terminated = realloc(buffer, size + 1);
    if (!nul_terminated) {
        free(buffer);
        return NULL;
    }
    nul_terminated[size] = '\0';
    *out_len = size;

    return nul_terminated;
}
