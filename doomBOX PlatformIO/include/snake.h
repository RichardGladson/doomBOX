/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef SNAKE_H
#define SNAKE_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "pindefs.h"

#define SNAKE_CELL   4
#define SNAKE_COLS  (128 / SNAKE_CELL)
#define SNAKE_ROWS  ( 64 / SNAKE_CELL)
#define SNAKE_MAX   (SNAKE_COLS * SNAKE_ROWS)

void snakeSetup();
void snakeLoop();
void snakeCleanup();

#endif
