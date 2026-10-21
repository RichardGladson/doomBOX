/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef ABOUT_H
#define ABOUT_H

#include <U8g2lib.h>
#include "pindefs.h"

#define NYANBOX_VERSION "v4.27.11"
extern const char* nyanboxVersion;

void aboutSetup();
void aboutLoop();
void aboutCleanup();

#endif