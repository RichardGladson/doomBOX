/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef SLEEP_MANAGER_H
#define SLEEP_MANAGER_H

extern void updateLastActivity();
extern void checkIdle();          // no-op
extern void wakeDisplay();        // no-op
extern bool anyButtonPressed();
extern void updateSleepTimeout(unsigned long newTimeout);  // no-op

#endif
