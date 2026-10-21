/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef AXON_DETECTOR_H
#define AXON_DETECTOR_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "pindefs.h"

void axonDetectorSetup();
void axonDetectorLoop();

#endif