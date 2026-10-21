/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef BEACON_SPAM_H
#define BEACON_SPAM_H

#include "esp_wifi.h"
#include <vector>
#include "pindefs.h"
#include <stdint.h>
#include <U8g2lib.h>

void beaconSpamSetup();
void beaconSpamLoop();

#endif
