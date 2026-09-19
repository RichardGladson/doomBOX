/*
    nyanBOX by Nyan Devices
    Client Sniffer - based on ESP32-Nightshade sniffer logic
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2026 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#ifndef CLIENT_SNIFFER_H
#define CLIENT_SNIFFER_H

void clientSnifferSetup();
void clientSnifferLoop();
void clientSnifferCleanup();

#endif
