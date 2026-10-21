/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef DISPLAY_MIRROR_H
#define DISPLAY_MIRROR_H

#include <U8g2lib.h>

void displayMirrorSetup();

void displayMirrorSend(U8G2_SSD1306_128X64_NONAME_F_HW_I2C &display);

void displayMirrorEnable(bool enable);

bool displayMirrorEnabled();

#endif
