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

void levelSystemSetup();
void levelSystemLoop();
void addXP(int amount);
int getCurrentLevel();
int getCurrentXP();
int getXPForNextLevel();
void displayLevelScreen();
void resetXPData();
// Raise XP so the player is at least this level (does not lower level)
void setMinimumLevel(int level);

#endif
