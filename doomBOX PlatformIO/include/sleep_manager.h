/*
    nyanBOX by Nyan Devices
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2025 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT

    Sleep feature removed in doomBOX revamp 2.
    Stubs retained so existing module includes still compile.
*/

#ifndef SLEEP_MANAGER_H
#define SLEEP_MANAGER_H

extern void updateLastActivity();
extern void checkIdle();          // no-op
extern void wakeDisplay();        // no-op
extern bool anyButtonPressed();
extern void updateSleepTimeout(unsigned long newTimeout);  // no-op

#endif
