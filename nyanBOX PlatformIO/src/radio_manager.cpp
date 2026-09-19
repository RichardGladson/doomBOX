/*
    nyanBOX by Nyan Devices
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2026 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include "../include/radio_manager.h"
#include "../include/pindefs.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include <Arduino.h>
#include <RF24.h>
#include <SPI.h>

extern RF24 radios[1];

static bool classicBtMemReleased = false;

// ---------------------------------------------------------------------------
// Unified NRF24 SPI + chip bring-up (single module on CE=5, CSN=17)
// ---------------------------------------------------------------------------
void initNrf24Spi() {
  // Explicit pin map. SS = -1 so the bus does not claim CSN (GPIO 17);
  // RF24 / raw register code drive CSN themselves.
  SPI.begin(18, 19, 23, -1);  // SCK, MISO, MOSI, SS
  delay(20);
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(10000000);
  SPI.setBitOrder(MSBFIRST);

  pinMode(RADIO_CE_PIN_1, OUTPUT);
  pinMode(RADIO_CSN_PIN_1, OUTPUT);
  digitalWrite(RADIO_CSN_PIN_1, HIGH);
  digitalWrite(RADIO_CE_PIN_1, LOW);
  delay(5);
}

bool nrf24Begin() {
  initNrf24Spi();

  // Soft reset path: power down any prior state, then re-init
  radios[0].powerDown();
  delay(10);

  if (!radios[0].begin()) {
    return false;
  }
  if (!radios[0].isChipConnected()) {
    return false;
  }

  radios[0].setAutoAck(false);
  radios[0].stopListening();
  radios[0].setRetries(0, 0);
  radios[0].setPALevel(RF24_PA_MAX, true);
  radios[0].setDataRate(RF24_2MBPS);
  radios[0].setCRCLength(RF24_CRC_DISABLED);
  return true;
}

void nrf24PowerDown() {
  radios[0].powerDown();
  digitalWrite(RADIO_CE_PIN_1, LOW);
  digitalWrite(RADIO_CSN_PIN_1, HIGH);
}

bool initBLE() {
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        if (!classicBtMemReleased) {
            esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT); // Release Classic Bluetooth memory to free up resources for BLE operations
            classicBtMemReleased = true;
        }
    }

    if (!btStarted()) {
        btStart();
        delay(50);
    }

    esp_bluedroid_status_t bt_state = esp_bluedroid_get_status();
    if (bt_state == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        if (esp_bluedroid_init() != ESP_OK) return false;
        delay(50);
    }

    bt_state = esp_bluedroid_get_status();
    if (bt_state != ESP_BLUEDROID_STATUS_ENABLED) {
        if (esp_bluedroid_enable() != ESP_OK) return false;
        delay(50);
    }

    return true;
}

void cleanupBLE() {
    esp_ble_gap_stop_scanning();
    esp_ble_gap_stop_advertising();
    delay(50);

    esp_bluedroid_status_t bt_state = esp_bluedroid_get_status();
    if (bt_state == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_disable();
        delay(50);
    }

    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_deinit();
        delay(50);
    }

    if (btStarted()) {
        btStop();
        delay(50);
    }
}

bool initWiFi(wifi_mode_t mode) {
    wifi_mode_t currentMode;
    if (esp_wifi_get_mode(&currentMode) != ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(mode);
    if (esp_wifi_start() != ESP_OK) return false;

    return true;
}

void cleanupRadio() {
    nrf24PowerDown();
    cleanupWiFi();
    cleanupBLE();
}

void cleanupWiFi() {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_stop();
        delay(50);
        esp_wifi_deinit();
        delay(100);
    }

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != NULL) {
        esp_netif_destroy(sta_netif);
    }

    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL) {
        esp_netif_destroy(ap_netif);
    }

    delay(100);
}