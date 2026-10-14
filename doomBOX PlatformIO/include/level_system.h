/*
    doomBOX
    Based on nyanBOX by Nyan Devices
    Copyright (c) 2025 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
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
