/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef BLE_SPAM_H
#define BLE_SPAM_H

#include <esp_bt.h>

extern bool isBleSpamming;

void bleSpamSetup();
void bleSpamLoop();

#endif
