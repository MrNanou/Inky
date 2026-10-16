/*
 * battery.c
 *
 * Battery indicator formatting implementation.
 */

#include "battery/battery.h"

#include <stdio.h>

const char *battery_string(int pct)
{
    static char buffer[16];

    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }

    snprintf(buffer, sizeof(buffer), "%d%%", pct);
    return buffer;
}
