#pragma once
// ── Wi-Fi credentials — TEMPLATE ─────────────────────────────────────────
// Copy this file to include/secrets.h and fill in the real values.
// secrets.h is gitignored; this template is committed.
//
//   cp include/secrets.example.h include/secrets.h
//
// APs are listed in priority order (see WIFI_APS[] in src/main.cpp).

#define WIFI_AP1_SSID  "your-primary-ssid"
#define WIFI_AP1_PASS  "your-primary-password"

#define WIFI_AP2_SSID  "your-fallback-ssid"
#define WIFI_AP2_PASS  "your-fallback-password"
