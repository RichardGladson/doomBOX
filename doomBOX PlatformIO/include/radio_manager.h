/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#pragma once

#include "esp_wifi.h"
#include "esp_bt_main.h"
#include <SPI.h>

bool initBLE();
void cleanupBLE();
bool initWiFi(wifi_mode_t mode);
void cleanupWiFi();
void cleanupRadio();
