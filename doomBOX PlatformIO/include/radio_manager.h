/*
    nyanBOX by Nyan Devices
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2026 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "esp_wifi.h"
#include "esp_bt_main.h"
#include <RF24.h>
#include <SPI.h>

// Single shared NRF24 instance (defined in nyanBOX.ino)
extern RF24 radios[1];

// Unified SPI + CE/CSN bring-up for the single NRF24 module
// Uses SCK=18, MISO=19, MOSI=23; does NOT claim CSN as hardware SS
void initNrf24Spi();

// Full RF24 library init on radios[0]. Returns true if chip responds.
bool nrf24Begin();

// Power down the shared NRF24
void nrf24PowerDown();

// Stop ESP32 WiFi + Bluetooth radios so they do not compete on 2.4 GHz
// while the NRF24 is jamming (smoochiee-style isolation).
void silenceEsp32Rf();

bool initBLE();
void cleanupBLE();
bool initWiFi(wifi_mode_t mode);
void cleanupWiFi();
void cleanupRadio();
