/*
 * layout.h
 *
 * Cell-based text layout engine.
 *
 * The layout engine takes plain text, a column width, and a row height and
 * produces pages of wrapped lines. It is intentionally cell-based (rows x
 * cols) rather than pixel-based so the same engine can later be driven by
 * font metrics on SDL2 / e-ink.
 */

#ifndef LAYOUT_H
#define LAYOUT_H

/*
 * Layout state.
 *
 * The internal text buffer is split into NUL-terminated lines. line_offsets
 * stores the byte offset of each line. line_count is the number of wrapped
 * lines.
 */
typedef struct {
    char *text;
    int *line_offsets;
    int line_count;
    int line_capacity;
    int cols;
} layout_t;

/*
 * Create a new empty layout.
 */
layout_t *layout_new(void);

/*
 * Free all memory owned by a layout.
 */
void layout_free(layout_t *layout);

/*
 * Build a layout by word-wrapping plain text to a fixed column width.
 * Returns 1 on success, 0 on failure.
 */
int layout_build(layout_t *layout, const char *text, int cols);

/*
 * Return the number of pages for a given content row count.
 */
int layout_page_count(const layout_t *layout, int content_rows);

/*
 * Render one page of the layout using a caller-provided callback.
 *
 * For each content line that belongs to the page, the callback is invoked with:
 *   - line_number: the 0-based line index within the layout
 *   - line_text:   the NUL-terminated text of the line
 *   - is_status:   always 0 for content lines rendered by this function
 *
 * Status/chrome lines are the caller's responsibility; the callback may be
 * reused for them with negative line_number sentinels (e.g. -1 and -2) if the
 * caller chooses.
 *
 * The layout engine does not clear the screen or move the cursor; that is the
 * caller's responsibility.
 */
typedef void (*layout_render_line_fn)(int line_number, const char *line_text,
                                      int is_status, void *user_data);

/*
 * Render a page. Content lines are rendered first; status/chrome must be drawn
 * separately by the caller. The callback receives each content line in order.
 */
void layout_render_page(const layout_t *layout, int page, int content_rows,
                        layout_render_line_fn callback, void *user_data);

#endif
