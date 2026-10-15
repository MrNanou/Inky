/*
 * epub.h
 *
 * EPUB parsing interface for the Inky terminal reader.
 *
 * An EPUB is a ZIP archive with a specific internal structure:
 *   - META-INF/container.xml  -> points to the .opf package file
 *   - package.opf             -> manifest + spine + metadata
 *   - XHTML chapter files     -> actual content
 *
 * This module hides the ZIP and XML details behind a small API.
 */

#ifndef EPUB_H
#define EPUB_H

#include "../archive/archive.h"
#include <stddef.h>

/* One entry from the spine: its idref and its path inside the ZIP. */
typedef struct {
    char *idref;
    char *path;
} chapter_entry_t;

/* Result of loading an EPUB. */
typedef struct {
    inky_archive_t *archive;   /* Owned by epub; closed in archive_close(). */
    chapter_entry_t *chapters; /* Array, heap allocated. */
    int chapter_count;
} epub_t;

/*
 * Open an EPUB file and build the chapter list from its spine.
 * Returns a pointer on success, NULL on failure.
 */
epub_t *epub_open(const char *filename);

/*
 * Close an EPUB and free all associated memory.
 */
void epub_close(epub_t *epub);

/*
 * Read a file from inside the EPUB ZIP into a NUL-terminated buffer.
 * The caller must free() the returned buffer.
 * On success writes the byte length (without the NUL) into *out_len.
 */
char *epub_read_file(epub_t *epub, const char *filename, size_t *out_len);

#endif
