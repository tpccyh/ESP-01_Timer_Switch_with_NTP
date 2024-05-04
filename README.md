# ESP-01 Timer Switch with NTP
The objective of this sketch is to demonstrate the usage of *ESP8266WiFi.h* to handle both http **GET** and **POST** requests.
Beside, this is a feature rich sketch for ESP-01 based timer switch.

## Highlights as below:
* Use interfaces provided by ESP8266WiFi.h to handle simple http GET and POST request.
* Support **Station Mode**, and **Access Point Mode** as fallback.
* Automatically turn off the soft-AP upon successful connection to the home AP.
* **Configurations via web interface**:
    - Soft-AP SSID & password (not encrypted!)
    - Home AP SSID & password (not encrypted!)
    - NTP server & timezone
    - Adjust time when NTP is not available
    - Timer schedule
* Configurations are stored in non-volatile memory via LittleFS.
* Web interfaces are kept in the LittleFS, hence they are easy to maintain.
* Operate on the internal RTC when NTP is not available (no internet connection).
* 3 Operating modes:
    - Timer mode
    - Permanent ON
    - Permanent OFF
* Last operating mode is saved into non-volatile memory.
* Support **factory reset** via both web interface and HW button. This is crucial for the general user, where wrong configuration may cause the timer switch to be inaccessible.

## Hardware:
* ESP-01 / ESP-01S
* Relay module for ESP-01
