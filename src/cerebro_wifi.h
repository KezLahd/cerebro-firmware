#pragma once
#include <Arduino.h>

// Allocate recording buffer (call once in setup)
void wifiServerInit();

// Start HTTP server + mDNS (call after WiFi connects)
void wifiServerStart();

// Handle HTTP clients (call in loop)
void wifiServerLoop();

// True if app has hit an endpoint in the last 30s
bool wifiAppConnected();

// Face code set via HTTP (-1 = none)
int8_t wifiGetFaceCode();
