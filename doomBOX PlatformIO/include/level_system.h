/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef LEVEL_SYSTEM_H
#define LEVEL_SYSTEM_H

#include <Arduino.h>

// Uptime system (replaces leveling)
void levelSystemSetup();   // init / load uptime from EEPROM
void levelSystemLoop();    // draw uptime screen
void displayLevelScreen(); // draw uptime screen

// Total accumulated uptime in seconds (persisted)
uint32_t getTotalUptimeSeconds();

// Call periodically from main loop to accumulate and periodically save
void updateUptimeTracking();

// Stubs kept so existing call sites still compile (no-ops)
void addXP(int amount);
int getCurrentLevel();
int getCurrentXP();
int getXPForNextLevel();
void resetXPData();
void setMinimumLevel(int level);

#endif
