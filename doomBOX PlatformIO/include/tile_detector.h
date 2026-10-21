/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef TILE_DETECTOR_H
#define TILE_DETECTOR_H

#include <U8g2lib.h>
#include "neopixel.h"
#include "pindefs.h"

void tileDetectorSetup();
void tileDetectorLoop();

#endif