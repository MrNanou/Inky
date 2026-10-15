/*
 * archive.h
 *
 * Opaque archive abstraction for Inky. On PC this is implemented with
 * libzip; on ESP32 it will be replaced with miniz (or similar).
 */

#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stddef.h>

typedef struct inky_archive inky_archive_t;

/*
 * Open an archive (e.g. an EPUB ZIP) for reading.
 * Returns NULL on failure.
 */
inky_archive_t *archive_open(const char *filename);

/*
 * Close an archive and free all associated resources.
 */
void archive_close(inky_archive_t *archive);

/*
 * Read a file from inside the archive into a NUL-terminated buffer.
 * The caller must free() the returned buffer.
 * On success writes the byte length (without the NUL) into *out_len.
 * Returns NULL if the file is not found or cannot be read.
 */
char *archive_read_file(inky_archive_t *archive, const char *filename, size_t *out_len);

#endif
