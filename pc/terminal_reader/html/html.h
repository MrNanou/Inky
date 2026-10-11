/*
 * html.h
 *
 * Minimal HTML-to-text converter.
 */

#ifndef HTML_H
#define HTML_H

#include <stddef.h>

/*
 * Convert a buffer of HTML/XHTML into plain text.
 *
 * Removes tags, collapses whitespace, decodes basic XML entities, and adds
 * blank lines after block-level end tags (p, h1-h6, div).
 */
void html_to_text(const char *html, char *out, size_t out_size);

#endif
