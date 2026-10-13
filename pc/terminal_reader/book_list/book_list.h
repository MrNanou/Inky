/*
 * book_list.h
 *
 * Scans the Books/ directory for .epub files and presents them as a
 * selectable list. This is the PC equivalent of the SD card library on ESP32.
 */

#ifndef BOOK_LIST_H
#define BOOK_LIST_H

/*
 * Information about one book entry.
 */
typedef struct {
    char *path;
    char *filename;
} book_entry_t;

/*
 * Scans `dir` for files matching `*.epub` (case-insensitive) and fills the
 * `entries` array. Returns the number of books found, or -1 on error.
 *
 * The caller must call book_list_free() to release the returned array.
 */
int book_list_scan(const char *dir, book_entry_t **entries);

/*
 * Free all memory allocated by book_list_scan().
 */
void book_list_free(book_entry_t *entries, int count);

#endif
