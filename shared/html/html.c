/*
 * html.c
 *
 * Minimal HTML-to-text converter.
 */

#include "html/html.h"

#include <ctype.h>
#include <string.h>

void html_to_text(const char *html, char *out, size_t out_size)
{
    size_t j = 0;
    int inside_tag = 0;
    int last_was_space = 1;

    for (size_t i = 0; html[i] != '\0' && j + 1 < out_size; i++) {
        char c = html[i];

        if (c == '<') {
            inside_tag = 1;
            continue;
        }
        if (c == '>') {
            inside_tag = 0;

            /* Insert blank lines after block-level end tags. */
            if (j > 0) {
                const char *prev = &html[i];
                int block = 0;
                if (strncasecmp(prev - 2, "/p", 2) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h1", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h2", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h3", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h4", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h5", 3) == 0) block = 1;
                else if (prev - 3 >= html && strncasecmp(prev - 3, "/h6", 3) == 0) block = 1;
                else if (prev - 4 >= html && strncasecmp(prev - 4, "/div", 4) == 0) block = 1;

                if (block) {
                    if (out[j - 1] != '\n') {
                        out[j++] = '\n';
                    }
                    out[j++] = '\n';
                    last_was_space = 1;
                }
            }
            continue;
        }

        if (inside_tag) {
            continue;
        }

        /* Decode basic XML entities. */
        if (c == '&') {
            if (strncmp(&html[i + 1], "amp;", 4) == 0) {
                c = '&';  i += 4;
            } else if (strncmp(&html[i + 1], "lt;", 3) == 0) {
                c = '<';  i += 3;
            } else if (strncmp(&html[i + 1], "gt;", 3) == 0) {
                c = '>';  i += 3;
            } else if (strncmp(&html[i + 1], "quot;", 5) == 0) {
                c = '"';  i += 5;
            } else if (strncmp(&html[i + 1], "apos;", 5) == 0) {
                c = '\''; i += 5;
            }
        }

        /* Collapse all whitespace to a single space. */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_was_space && j + 1 < out_size) {
                out[j++] = ' ';
                last_was_space = 1;
            }
            continue;
        }

        out[j++] = c;
        last_was_space = 0;
    }

    out[j] = '\0';

    if (j > 0 && out[j - 1] == ' ') {
        out[j - 1] = '\0';
    }
}
