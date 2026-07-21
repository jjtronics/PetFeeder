# Changelog

All notable changes to this project will be documented in this file.

## v4.0.8
- Hardened ESP8266 WiFiManager setup mode: open setup AP, deterministic `*-SETUP-XXXX` SSID, forced channel 6 and serial AP/STA diagnostics.
- Added Wi‑Fi credential reset from both the normal Settings page and the WiFiManager captive portal; reset clears only ESP8266 Wi‑Fi credentials and reboots into setup AP mode.
- Documented the reproduced ESP8266/home-AP channel issue: a new 2.4 GHz access point on channel 1 caused unstable or failed ESP8266 association, while moving the home AP to channel 5 restored reliable connectivity.
- Shortened Wi‑Fi connection retries before setup portal fallback to avoid long waits with missing or invalid credentials.

## v4.0
- Initial open-source release with Wi-Fi configuration portal and responsive web interface.
- Daily schedule management and manual dispensing actions.
- Daily quota tracking with 7-day history chart.
- OTA firmware updates with configurable password.
- Local JSON API endpoints.
- Physical button for feed/unclog/safe-mode toggle.
- Optional MQTT and webhook notifications.
- Various bug fixes and stability improvements.
