/*
 * terminal.c
 *
 * Terminal input/output implementation.
 */

#include "terminal/terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

static struct termios original_termios;
static int raw_mode_active = 0;

int terminal_get_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return 0;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 1;
}

void terminal_restore(void)
{
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        raw_mode_active = 0;
    }
}

void terminal_setup_raw(void)
{
    tcgetattr(STDIN_FILENO, &original_termios);
    atexit(terminal_restore);

    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = 1;
}

int terminal_read_key(void)
{
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return KEY_OTHER;
    }

    if (c == 'q' || c == 'Q') {
        return KEY_QUIT;
    }

    /* Escape sequence - try to read the next two bytes. */
    if (c == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_OTHER;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_OTHER;

        if (seq[0] == '[') {
            if (seq[1] == 'C') return KEY_RIGHT;
            if (seq[1] == 'D') return KEY_LEFT;
        }
    }

    return KEY_OTHER;
}

void terminal_clear(void)
{
    printf("\033[2J\033[H");
}
