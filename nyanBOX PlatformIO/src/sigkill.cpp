/*
    nyanBOX by Nyan Devices
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2025 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include <Arduino.h>
#include "../include/sigkill.h"
#include "../include/radio_manager.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/icon.h"
#include "../include/pindefs.h"
#include <esp_bt_main.h>

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// Use the single shared RF24 instance (radios[0]) — do NOT create a second object

enum SigKillMode { SIG_MENU, SIG_WIFI_SELECT, SIG_JAMMING };
enum ProtocolType { ALL, WIFI, BLUETOOTH, BLE, VIDEO_TX, RC, USB_WIRELESS, ZIGBEE, NRF24 };

static SigKillMode currentMode = SIG_MENU;
static ProtocolType selectedProtocol = ALL;
static int menuSelection = 0;
static int wifiChannelSelection = 0;  // 0 = All channels, 1-11 = specific channel
static unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;

static bool needsRedraw = true;
static int lastMenuSelection = -1;
static SigKillMode lastMode = SIG_MENU;
static ProtocolType lastProtocol = ALL;
static int lastWifiChannelSelection = -1;
static bool radioReady = false;

// Protocol channel definitions
const byte bluetooth_channels[]        = {32,34,46,48,50,52,0,1,2,4,6,8,22,24,26,28,30,74,76,78,80};
const byte ble_channels[]              = {2,26,80};
const byte wifi_channels[]             = {1,2,3,4,5,6,7,8,9,10,11,12};
const byte usbWireless_channels[]      = {40,50,60};
const byte videoTransmitter_channels[] = {70,75,80};
const byte rc_channels[]               = {1,3,5,7};
const byte zigbee_channels[]           = {11,15,20,25};
const byte nrf24_channels[]            = {76,78,79};

const char* protocolNames[] = {
  "All", "WiFi", "Bluetooth", "BLE", "Video TX", "RC",
  "USB Wireless", "Zigbee", "NRF24"
};

static void configureForJamming(byte initialChannel) {
  radios[0].setAutoAck(false);
  radios[0].stopListening();
  radios[0].setRetries(0, 0);
  radios[0].setPALevel(RF24_PA_MAX, true);
  radios[0].setDataRate(RF24_2MBPS);
  radios[0].setCRCLength(RF24_CRC_DISABLED);
  radios[0].startConstCarrier(RF24_PA_MAX, initialChannel);
}

static bool initializeRadios() {
  // Unified SPI + single RF24 instance
  radioReady = nrf24Begin();
  if (radioReady) {
    configureForJamming(2);
  }
  return radioReady;
}

static void powerDownRadios() {
  nrf24PowerDown();
  radioReady = false;
  delay(50);
}

static void drawSigMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 10, "SigKill Mode:");

  int start = (menuSelection / 4) * 4;

  for (int i = 0; i < 4 && (start + i) < 9; i++) {
    int idx = start + i;
    char line[24];
    snprintf(line, sizeof(line), "%s %s",
             (idx == menuSelection) ? ">" : " ",
             protocolNames[idx]);
    u8g2.drawStr(0, 22 + (i * 10), line);
  }

  int scrollbarX = 122;
  int scrollbarWidth = 4;
  int scrollbarY = 14;
  int scrollbarHeight = 34;

  u8g2.drawFrame(scrollbarX, scrollbarY, scrollbarWidth, scrollbarHeight);

  int thumbHeight = (scrollbarHeight * 4) / 9;
  int maxThumbTravel = scrollbarHeight - thumbHeight - 2;
  int thumbY = scrollbarY + 1 + ((menuSelection * maxThumbTravel) / 8);

  u8g2.drawBox(scrollbarX + 1, thumbY, scrollbarWidth - 2, thumbHeight);

  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 62, "U/D=Move R=Select SEL=Exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawWifiSelect() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 10, "WiFi Channel:");

  u8g2.setFont(u8g2_font_5x8_tr);
  const int visible = 4;
  const int totalOptions = 12;  // All + Ch1..Ch11
  int start = (wifiChannelSelection / visible) * visible;

  for (int i = 0; i < visible; i++) {
    int idx = start + i;
    if (idx >= totalOptions) break;
    char line[24];
    if (idx == 0) {
      snprintf(line, sizeof(line), "%s All Channels",
               (idx == wifiChannelSelection) ? ">" : " ");
    } else {
      snprintf(line, sizeof(line), "%s Channel %d",
               (idx == wifiChannelSelection) ? ">" : " ", idx);
    }
    u8g2.drawStr(0, 22 + i * 10, line);
  }

  u8g2.drawStr(0, 62, "U/D=Move R=Jam L=Back SEL=Exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawActiveJamming(const char* protocolName) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  char title[32];
  if (selectedProtocol == WIFI && wifiChannelSelection > 0) {
    snprintf(title, sizeof(title), "WiFi Ch%d Jam", wifiChannelSelection);
  } else {
    snprintf(title, sizeof(title), "%s Jamming", protocolName);
  }
  u8g2.drawStr(0, 12, title);

  u8g2.drawStr(0, 28, radioReady ? "Status: JAMMING" : "Status: FAIL");

  u8g2.setFont(u8g2_font_5x8_tr);
  char radioStatus[32];
  snprintf(radioStatus, sizeof(radioStatus), "R1: %s",
           radioReady ? "ACTIVE" : "FAIL");
  u8g2.drawStr(0, 42, radioStatus);

  if (selectedProtocol == WIFI) {
    if (wifiChannelSelection == 0) {
      u8g2.drawStr(0, 52, "Mode: All channels");
    } else {
      char chLine[24];
      snprintf(chLine, sizeof(chLine), "Mode: Fixed Ch %d", wifiChannelSelection);
      u8g2.drawStr(0, 52, chLine);
    }
  }

  u8g2.drawStr(0, 62, "L=Back SEL=Exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

void sigkillSetup() {
  Serial.begin(115200);

  cleanupRadio();

  pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);

  currentMode = SIG_MENU;
  menuSelection = 0;
  selectedProtocol = ALL;
  wifiChannelSelection = 0;
  radioReady = false;

  needsRedraw = true;
  lastMenuSelection = -1;
  lastMode = SIG_MENU;
  lastProtocol = ALL;
  lastWifiChannelSelection = -1;

  powerDownRadios();
  drawSigMenu();
}

void sigkillLoop() {
  unsigned long now = millis();

  bool up = digitalRead(BUTTON_PIN_UP) == LOW;
  bool down = digitalRead(BUTTON_PIN_DOWN) == LOW;
  bool left = digitalRead(BUTTON_PIN_LEFT) == LOW;
  bool right = digitalRead(BUTTON_PIN_RIGHT) == LOW;

  if (lastMode != currentMode) {
    lastMode = currentMode;
    needsRedraw = true;
  }
  if (lastMenuSelection != menuSelection) {
    lastMenuSelection = menuSelection;
    needsRedraw = true;
  }
  if (lastProtocol != selectedProtocol) {
    lastProtocol = selectedProtocol;
    needsRedraw = true;
  }
  if (lastWifiChannelSelection != wifiChannelSelection) {
    lastWifiChannelSelection = wifiChannelSelection;
    needsRedraw = true;
  }

  switch (currentMode) {
    case SIG_MENU:
      if (now - lastButtonPress > debounceDelay) {
        if (up) {
          menuSelection = (menuSelection - 1 + 9) % 9;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (down) {
          menuSelection = (menuSelection + 1) % 9;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (right) {
          selectedProtocol = (ProtocolType)menuSelection;
          if (selectedProtocol == WIFI) {
            wifiChannelSelection = 0;
            currentMode = SIG_WIFI_SELECT;
          } else {
            currentMode = SIG_JAMMING;
            initializeRadios();
          }
          needsRedraw = true;
          lastButtonPress = now;
        }
      }

      if (needsRedraw) {
        needsRedraw = false;
        drawSigMenu();
      }
      break;

    case SIG_WIFI_SELECT:
      if (now - lastButtonPress > debounceDelay) {
        if (up) {
          wifiChannelSelection = (wifiChannelSelection - 1 + 12) % 12;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (down) {
          wifiChannelSelection = (wifiChannelSelection + 1) % 12;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (right) {
          currentMode = SIG_JAMMING;
          initializeRadios();
          if (radioReady && wifiChannelSelection > 0) {
            radios[0].setChannel(wifiChannelSelection);
          }
          needsRedraw = true;
          lastButtonPress = now;
        } else if (left) {
          currentMode = SIG_MENU;
          needsRedraw = true;
          lastButtonPress = now;
        }
      }

      if (needsRedraw) {
        needsRedraw = false;
        drawWifiSelect();
      }
      break;

    case SIG_JAMMING:
      if (needsRedraw) {
        needsRedraw = false;
        drawActiveJamming(protocolNames[selectedProtocol]);
      }

      if (radioReady) {
        int randomIndex;
        int channel;

        switch (selectedProtocol) {
          case ALL:
            {
              static int protocolIndex = 0;
              const byte* channelArray;
              int arraySize;

              switch (protocolIndex % 8) {
                case 0: channelArray = wifi_channels; arraySize = sizeof(wifi_channels); break;
                case 1: channelArray = bluetooth_channels; arraySize = sizeof(bluetooth_channels); break;
                case 2: channelArray = ble_channels; arraySize = sizeof(ble_channels); break;
                case 3: channelArray = videoTransmitter_channels; arraySize = sizeof(videoTransmitter_channels); break;
                case 4: channelArray = rc_channels; arraySize = sizeof(rc_channels); break;
                case 5: channelArray = usbWireless_channels; arraySize = sizeof(usbWireless_channels); break;
                case 6: channelArray = zigbee_channels; arraySize = sizeof(zigbee_channels); break;
                case 7: channelArray = nrf24_channels; arraySize = sizeof(nrf24_channels); break;
                default: channelArray = wifi_channels; arraySize = sizeof(wifi_channels); break;
              }

              randomIndex = random(0, arraySize / sizeof(byte));
              channel = channelArray[randomIndex];
              radios[0].setChannel(channel);
              protocolIndex++;
            }
            break;

          case WIFI:
            if (wifiChannelSelection == 0) {
              randomIndex = random(0, sizeof(wifi_channels) / sizeof(wifi_channels[0]));
              channel = wifi_channels[randomIndex];
              radios[0].setChannel(channel);
            } else {
              radios[0].setChannel(wifiChannelSelection);
            }
            break;

          case BLUETOOTH:
            randomIndex = random(0, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]));
            radios[0].setChannel(bluetooth_channels[randomIndex]);
            break;

          case BLE:
            randomIndex = random(0, sizeof(ble_channels) / sizeof(ble_channels[0]));
            radios[0].setChannel(ble_channels[randomIndex]);
            break;

          case VIDEO_TX:
            randomIndex = random(0, sizeof(videoTransmitter_channels) / sizeof(videoTransmitter_channels[0]));
            radios[0].setChannel(videoTransmitter_channels[randomIndex]);
            break;

          case RC:
            randomIndex = random(0, sizeof(rc_channels) / sizeof(rc_channels[0]));
            radios[0].setChannel(rc_channels[randomIndex]);
            break;

          case USB_WIRELESS:
            randomIndex = random(0, sizeof(usbWireless_channels) / sizeof(usbWireless_channels[0]));
            radios[0].setChannel(usbWireless_channels[randomIndex]);
            break;

          case ZIGBEE:
            randomIndex = random(0, sizeof(zigbee_channels) / sizeof(zigbee_channels[0]));
            radios[0].setChannel(zigbee_channels[randomIndex]);
            break;

          case NRF24:
            randomIndex = random(0, sizeof(nrf24_channels) / sizeof(nrf24_channels[0]));
            radios[0].setChannel(nrf24_channels[randomIndex]);
            break;
        }
      }

      if (left && now - lastButtonPress > debounceDelay) {
        powerDownRadios();
        if (selectedProtocol == WIFI) {
          currentMode = SIG_WIFI_SELECT;
        } else {
          currentMode = SIG_MENU;
        }
        needsRedraw = true;
        lastButtonPress = now;
      }
      break;
  }
}
