/* refresh_modes.h - the refresh rates to offer for a display.
 *
 * A panel reports one number the system can trust about its refresh: the
 * highest it will accept (from its EDID, carried as max_refresh_mhz).  A
 * refresh-rate chooser needs a LIST, though, not a ceiling - the handful of
 * standard rates at or below that ceiling, plus whatever the panel is running
 * at now so the current setting is always one of the choices.  This turns the
 * one number into that list, with no guessing about rates the panel never
 * claimed it can do.
 *
 * Pure arithmetic, so it is unit-tested on the host (tools/refresh_modes_host_test.c)
 * rather than only where a display exists.  See refresh_modes.c.
 */
#ifndef KESTREL_REFRESH_MODES_H
#define KESTREL_REFRESH_MODES_H

#include <stdint.h>

/* Fill `out` with the refresh rates (in millihertz) worth offering, given the
 * panel's ceiling `max_mhz` and the rate it is running at now, `cur_mhz`
 * (either may be 0 when unknown).  Returns how many were written, at most
 * `cap`.  The list is sorted low to high and never contains a duplicate; the
 * current rate is always included when it is non-zero. */
int refresh_options(uint32_t max_mhz, uint32_t cur_mhz, uint32_t *out, int cap);

#endif
