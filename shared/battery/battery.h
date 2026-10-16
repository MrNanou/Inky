/*
 * battery.h
 *
 * Battery indicator formatting.
 */

#ifndef BATTERY_H
#define BATTERY_H

/*
 * Render a battery percentage into a short string suitable for the status bar.
 * Returns a pointer to a static buffer.
 */
const char *battery_string(int pct);

#endif
