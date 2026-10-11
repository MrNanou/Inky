/*
 * layout.c
 *
 * Cell-based text layout implementation.
 */

#include "layout/layout.h"

#include <stdlib.h>
#include <string.h>

/*
 * Append a line offset to the layout, growing the array as needed.
 */
static int layout_add_line(layout_t *layout, int offset)
{
    if (layout->line_count >= layout->line_capacity) {
        int new_capacity = layout->line_capacity == 0 ? 256 : layout->line_capacity * 2;
        int *new_offsets = realloc(layout->line_offsets,
                                   new_capacity * sizeof(int));
        if (!new_offsets) {
            return 0;
        }
        layout->line_offsets = new_offsets;
        layout->line_capacity = new_capacity;
    }

    layout->line_offsets[layout->line_count++] = offset;
    return 1;
}

layout_t *layout_new(void)
{
    return calloc(1, sizeof(layout_t));
}

void layout_free(layout_t *layout)
{
    if (!layout) {
        return;
    }
    free(layout->text);
    free(layout->line_offsets);
    free(layout);
}

int layout_build(layout_t *layout, const char *text, int cols)
{
    if (!layout || !text || cols <= 0) {
        return 0;
    }

    /* Reset any previous content. */
    free(layout->text);
    free(layout->line_offsets);
    layout->text = NULL;
    layout->line_offsets = NULL;
    layout->line_count = 0;
    layout->line_capacity = 0;
    layout->cols = cols;

    size_t text_len = strlen(text);
    layout->text = malloc(text_len + 1);
    if (!layout->text) {
        return 0;
    }
    memcpy(layout->text, text, text_len + 1);

    /* Always start with a first line. */
    if (!layout_add_line(layout, 0)) {
        return 0;
    }

    int line_len = 0;
    int word_start = -1;
    int word_len = 0;

    for (int i = 0; layout->text[i] != '\0'; i++) {
        char c = layout->text[i];

        if (c == '\n') {
            /* Force a new line at the newline. */
            layout->text[i] = '\0';
            if (!layout_add_line(layout, i + 1)) {
                return 0;
            }
            line_len = 0;
            word_start = -1;
            word_len = 0;
            continue;
        }

        if (c == ' ') {
            /* End of a word. */
            if (word_start >= 0) {
                /*
                 * If this word doesn't fit on the current line, wrap before
                 * it. The line we've built so far ends where the word started.
                 */
                if (line_len > 0 && line_len + 1 + word_len > cols) {
                    layout->text[word_start - 1] = '\0';
                    if (!layout_add_line(layout, word_start)) {
                        return 0;
                    }
                    line_len = word_len;
                } else {
                    line_len += 1 + word_len;  /* space + word */
                }
                word_start = -1;
                word_len = 0;
            }
            continue;
        }

        if (word_start < 0) {
            word_start = i;
            word_len = 0;
        }
        word_len++;
    }

    /* Handle the final word if the text doesn't end with whitespace. */
    if (word_start >= 0) {
        if (line_len > 0 && line_len + 1 + word_len > cols) {
            layout->text[word_start - 1] = '\0';
            if (!layout_add_line(layout, word_start)) {
                return 0;
            }
        }
    }

    return 1;
}

int layout_page_count(const layout_t *layout, int rows)
{
    if (!layout || rows <= 1) {
        return 0;
    }
    int content_rows = rows - 1;
    return (layout->line_count + content_rows - 1) / content_rows;
}

void layout_render_page(const layout_t *layout, int page, int rows,
                        layout_render_line_fn callback, void *user_data)
{
    if (!layout || rows <= 2 || page < 0 || page >= layout_page_count(layout, rows)) {
        return;
    }

    int content_rows = rows - 2;  /* Reserve 2 rows for status lines. */
    int first_line = page * content_rows;
    int last_line = first_line + content_rows;
    if (last_line > layout->line_count) {
        last_line = layout->line_count;
    }

    /* Content lines. */
    for (int i = first_line; i < last_line; i++) {
        callback(i, layout->text + layout->line_offsets[i], 0, user_data);
    }

    /* Pad with empty lines to clear old content. */
    for (int i = last_line - first_line; i < content_rows; i++) {
        callback(i, "", 0, user_data);
    }

    /* Status lines are rendered at the bottom as line -1 and -2. */
    callback(-1, "", 1, user_data);
    callback(-2, "", 1, user_data);
}
