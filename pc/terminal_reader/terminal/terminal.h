/*
 * terminal.h
 *
 * Terminal input/output helpers for the Inky PC prototype.
 */

#ifndef TERMINAL_H
#define TERMINAL_H

/*
 * Query the terminal size using the TIOCGWINSZ ioctl.
 * Returns 1 on success, 0 on failure.
 */
int terminal_get_size(int *rows, int *cols);

/*
 * Put the terminal into raw-ish mode so we can read arrow keys as escape
 * sequences without waiting for Enter.
 */
void terminal_setup_raw(void);

/*
 * Restore the terminal to its original state.
 */
void terminal_restore(void);

/* Key codes returned by terminal_read_key(). */
#define KEY_QUIT  0
#define KEY_LEFT  1
#define KEY_RIGHT 2
#define KEY_UP    3
#define KEY_DOWN  4
#define KEY_ENTER 5
#define KEY_OTHER 6

/*
 * Read a single key press.
 */
int terminal_read_key(void);

/*
 * Clear the terminal screen and move the cursor to the top-left corner.
 */
void terminal_clear(void);

#endif
