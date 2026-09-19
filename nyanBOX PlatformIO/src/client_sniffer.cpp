/*
    nyanBOX by Nyan Devices
    Client Sniffer - exact sniffer logic from ESP32-Nightshade
    https://github.com/RichardGladson/ESP32-Nightshade
    Adapted for nyanBOX UI
    Copyright (c) 2026 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <string.h>
#include <vector>

#include "../include/client_sniffer.h"
#include "../include/radio_manager.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/pindefs.h"
#include "../include/setting.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// ====================== Nightshade-style data structures ======================
#define MAX_NETWORKS 40
#define MAX_CLIENTS  40
#define CLIENT_TIMEOUT 25000UL

struct Network {
  char ssid[33];
  uint8_t bssid[6];
  int channel;
  int rssi;
  wifi_auth_mode_t encryption;
};

struct ClientEntry {
  uint8_t mac[6];
  unsigned long lastSeen;
  int8_t rssi;
};

static Network networks[MAX_NETWORKS];
static ClientEntry clients[MAX_CLIENTS];
static int numNetworks = 0;
static int numClients = 0;

static uint8_t targetBSSID[6] = {0};
static int targetChannel = 1;
static char targetSSID[33] = {0};

static portMUX_TYPE clientMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool sniffing = false;

// UI state
enum SnifferState {
  STATE_SCANNING,
  STATE_SELECT_AP,
  STATE_SNIFFING
};

static SnifferState currentState = STATE_SCANNING;
static int selectedIndex = 0;
static int listStartIndex = 0;
static int clientScroll = 0;
static bool needsRedraw = true;
static unsigned long lastButtonPress = 0;
static const unsigned long debounceDelay = 200;
static unsigned long lastCleanup = 0;
static unsigned long lastRedraw = 0;

// ====================== Exact Nightshade helpers ======================
static bool validMAC(const uint8_t* m) {
  static const uint8_t nullMAC[6] = {0};
  if (memcmp(m, nullMAC, 6) == 0) return false;
  if ((m[0] & 1) == 1) return false; // multicast/broadcast
  return true;
}

static void addClient(const uint8_t* mac, int8_t rssi = 0) {
  if (!validMAC(mac)) return;
  portENTER_CRITICAL(&clientMux);
  for (int i = 0; i < numClients; i++) {
    if (memcmp(clients[i].mac, mac, 6) == 0) {
      clients[i].lastSeen = millis();
      if (rssi != 0) clients[i].rssi = rssi;
      portEXIT_CRITICAL(&clientMux);
      return;
    }
  }
  if (numClients < MAX_CLIENTS) {
    memcpy(clients[numClients].mac, mac, 6);
    clients[numClients].lastSeen = millis();
    clients[numClients].rssi = rssi;
    numClients++;
  }
  portEXIT_CRITICAL(&clientMux);
}

static void cleanupStaleClients() {
  portENTER_CRITICAL(&clientMux);
  unsigned long now = millis();
  int j = 0;
  for (int i = 0; i < numClients; i++) {
    if (now - clients[i].lastSeen < CLIENT_TIMEOUT) {
      if (j != i) memcpy(&clients[j], &clients[i], sizeof(ClientEntry));
      j++;
    }
  }
  numClients = j;
  portEXIT_CRITICAL(&clientMux);
}

// ====================== EXACT Nightshade snifferCallback ======================
void IRAM_ATTR snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!sniffing) return;
  wifi_promiscuous_pkt_t* raw = (wifi_promiscuous_pkt_t*)buf;
  if (raw->rx_ctrl.sig_len < 24) return;

  uint8_t* p = raw->payload;
  uint16_t fc = *(uint16_t*)p;
  uint8_t ft = (fc & 0x000C) >> 2;
  uint8_t subtype = (fc & 0x00F0) >> 4;
  int8_t rssi = raw->rx_ctrl.rssi;

  // Data frames (type 2)
  if (ft == 0x02) {
    bool toDS = (fc >> 8) & 1;
    bool fromDS = (fc >> 9) & 1;
    // STA -> AP
    if (toDS && !fromDS && memcmp(&p[4], targetBSSID, 6) == 0) {
      addClient(&p[10], rssi);
    }
    // AP -> STA
    else if (!toDS && fromDS && memcmp(&p[10], targetBSSID, 6) == 0) {
      addClient(&p[4], rssi);
    }
  }
  // Management frames directed at the target AP
  else if (ft == 0x00 && memcmp(&p[16], targetBSSID, 6) == 0) {
    // Association Request, Reassociation, Authentication, etc.
    if (subtype == 0x00 || subtype == 0x02 || subtype == 0x0B ||
        subtype == 0x05 || subtype == 0x0A || subtype == 0x0C) {
      if (memcmp(&p[10], targetBSSID, 6) != 0) {
        addClient(&p[10], rssi);
      }
    }
  }
}

// ====================== Helpers ======================
static void macToStr(const uint8_t* mac, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void stopSniffing() {
  sniffing = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(NULL);
}

static void startSniffing() {
  numClients = 0;
  clientScroll = 0;

  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(snifferCallback);
  esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(true);
  sniffing = true;
}

// ====================== Scanning APs ======================
static void doAPScan() {
  numNetworks = 0;
  currentState = STATE_SCANNING;
  needsRedraw = true;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks(false, true); // async=false, show_hidden=true
  if (n < 0) n = 0;
  if (n > MAX_NETWORKS) n = MAX_NETWORKS;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "(Hidden)";
    strncpy(networks[i].ssid, ssid.c_str(), 32);
    networks[i].ssid[32] = 0;
    memcpy(networks[i].bssid, WiFi.BSSID(i), 6);
    networks[i].channel = WiFi.channel(i);
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].encryption = WiFi.encryptionType(i);
  }
  numNetworks = n;
  selectedIndex = 0;
  listStartIndex = 0;
  currentState = STATE_SELECT_AP;
  needsRedraw = true;
  WiFi.scanDelete();
}

// ====================== Drawing ======================
static void drawScanning() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 12, "Client Sniffer");
  u8g2.drawStr(0, 32, "Scanning networks...");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 62, "SEL=Exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawAPList() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 10, "Select AP to sniff");

  u8g2.setFont(u8g2_font_5x8_tr);
  const int visible = 4;
  if (selectedIndex < listStartIndex) listStartIndex = selectedIndex;
  if (selectedIndex >= listStartIndex + visible) listStartIndex = selectedIndex - visible + 1;

  for (int i = 0; i < visible; i++) {
    int idx = listStartIndex + i;
    if (idx >= numNetworks) break;
    char line[32];
    char mark = (idx == selectedIndex) ? '>' : ' ';
    // Truncate SSID
    char shortSSID[16];
    strncpy(shortSSID, networks[idx].ssid, 14);
    shortSSID[14] = 0;
    snprintf(line, sizeof(line), "%c%s ch%d", mark, shortSSID, networks[idx].channel);
    u8g2.drawStr(0, 22 + i * 10, line);
  }

  u8g2.drawStr(0, 62, "U/D=Move R=Sniff SEL=Exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawClientList() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  char title[28];
  char shortSSID[12];
  strncpy(shortSSID, targetSSID, 10);
  shortSSID[10] = 0;
  snprintf(title, sizeof(title), "%s [%d]", shortSSID, numClients);
  u8g2.drawStr(0, 10, title);

  u8g2.setFont(u8g2_font_5x8_tr);

  if (numClients == 0) {
    u8g2.drawStr(0, 30, "Waiting for clients...");
    u8g2.drawStr(0, 42, "Stay near the AP");
  } else {
    const int visible = 4;
    if (clientScroll < 0) clientScroll = 0;
    if (clientScroll > numClients - visible && numClients > visible)
      clientScroll = numClients - visible;
    if (clientScroll < 0) clientScroll = 0;

    portENTER_CRITICAL(&clientMux);
    for (int i = 0; i < visible; i++) {
      int idx = clientScroll + i;
      if (idx >= numClients) break;
      char macStr[18];
      macToStr(clients[idx].mac, macStr);
      char line[28];
      snprintf(line, sizeof(line), "%s %ddBm", macStr, clients[idx].rssi);
      u8g2.drawStr(0, 22 + i * 10, line);
    }
    portEXIT_CRITICAL(&clientMux);
  }

  u8g2.drawStr(0, 62, "U/D=Scroll L=Back SEL=Exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

// ====================== Public API ======================
void clientSnifferSetup() {
  cleanupRadio();
  initWiFi(WIFI_MODE_STA);

  pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_CENTER, INPUT_PULLUP);

  sniffing = false;
  numNetworks = 0;
  numClients = 0;
  selectedIndex = 0;
  listStartIndex = 0;
  clientScroll = 0;
  currentState = STATE_SCANNING;
  needsRedraw = true;
  lastButtonPress = 0;
  lastCleanup = 0;
  lastRedraw = 0;

  drawScanning();
  doAPScan();
}

void clientSnifferLoop() {
  unsigned long now = millis();

  bool up    = digitalRead(BUTTON_PIN_UP)    == LOW;
  bool down  = digitalRead(BUTTON_PIN_DOWN)  == LOW;
  bool left  = digitalRead(BUTTON_PIN_LEFT)  == LOW;
  bool right = digitalRead(BUTTON_PIN_RIGHT) == LOW;

  // Periodic client cleanup
  if (sniffing && (now - lastCleanup > 2000)) {
    cleanupStaleClients();
    lastCleanup = now;
    needsRedraw = true;
  }

  if (now - lastButtonPress < debounceDelay) {
    // still debounce
  } else {
    switch (currentState) {
      case STATE_SCANNING:
        // nothing interactive
        break;

      case STATE_SELECT_AP:
        if (up && numNetworks > 0) {
          selectedIndex = (selectedIndex - 1 + numNetworks) % numNetworks;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (down && numNetworks > 0) {
          selectedIndex = (selectedIndex + 1) % numNetworks;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (right && numNetworks > 0) {
          // Start sniffing selected AP
          memcpy(targetBSSID, networks[selectedIndex].bssid, 6);
          targetChannel = networks[selectedIndex].channel;
          strncpy(targetSSID, networks[selectedIndex].ssid, 32);
          targetSSID[32] = 0;
          startSniffing();
          currentState = STATE_SNIFFING;
          needsRedraw = true;
          lastButtonPress = now;
        }
        break;

      case STATE_SNIFFING:
        if (up) {
          clientScroll--;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (down) {
          clientScroll++;
          needsRedraw = true;
          lastButtonPress = now;
        } else if (left) {
          stopSniffing();
          currentState = STATE_SELECT_AP;
          needsRedraw = true;
          lastButtonPress = now;
        }
        break;
    }
  }

  // Redraw
  if (needsRedraw || (sniffing && now - lastRedraw > 800)) {
    needsRedraw = false;
    lastRedraw = now;
    switch (currentState) {
      case STATE_SCANNING:   drawScanning();   break;
      case STATE_SELECT_AP:  drawAPList();     break;
      case STATE_SNIFFING:   drawClientList(); break;
    }
  }
}

void clientSnifferCleanup() {
  stopSniffing();
  cleanupWiFi();
}
