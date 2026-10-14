/*
    doomBOX / nyanBOX
    EAPOL / PMKID handshake capture
    Copyright (c) 2026 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#ifndef HANDSHAKE_CAPTURE_H
#define HANDSHAKE_CAPTURE_H

#include <Arduino.h>

void handshakeCaptureSetup();
void handshakeCaptureLoop();

void handshakeViewSetup();
void handshakeViewLoop();

#endif
