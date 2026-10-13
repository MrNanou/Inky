/*
 * books_dir.h
 *
 * Locate the Books/ directory from various run locations.
 */

#ifndef BOOKS_DIR_H
#define BOOKS_DIR_H

/*
 * Find the Books/ directory. Tries, in order:
 *   ./Books
 *   ../Books
 *   ../../Books
 * Returns a statically allocated string, or NULL if not found.
 */
const char *find_books_dir(void);

#endif
