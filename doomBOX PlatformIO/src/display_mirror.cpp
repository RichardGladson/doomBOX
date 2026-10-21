/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#include "../include/display_mirror.h"
#include <Arduino.h>

static bool mirrorEnabled = false;

void displayMirrorSetup() {
    mirrorEnabled = false;
}

void displayMirrorEnable(bool enable) {
    mirrorEnabled = enable;
}

bool displayMirrorEnabled() {
    return mirrorEnabled;
}

void displayMirrorSend(U8G2_SSD1306_128X64_NONAME_F_HW_I2C &display) {
    if (!mirrorEnabled) return;
    if (!Serial) return;

    uint8_t *buffer = display.getBufferPtr();
    size_t bufferSize = display.getBufferTileWidth() * display.getBufferTileHeight() * 8;

    Serial.write("<FB>");

    uint16_t size = bufferSize;
    Serial.write((uint8_t)(size & 0xFF));
    Serial.write((uint8_t)((size >> 8) & 0xFF));

    Serial.write(buffer, bufferSize);

    Serial.write("</FB>");
}
