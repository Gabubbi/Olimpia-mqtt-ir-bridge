# Olimpia-mqtt-ir-bridge
MQTT IR bridge for Home Assistant (MQTT Discovery + power-loss recovery) based on ESP32

# Olimpia MQTT IR Bridge (ESP32)

This project provides a **local-first ESP32-based IR bridge** designed to integrate
**IR-controlled or cloud-locked heaters** into **Home Assistant** using **MQTT**.

The initial implementation targets an *Olimpia Splendid* heater, but the architecture
is **vendor-agnostic** and can be reused with any IR-based device once the IR codes are known.

---

## Key features

- ✅ Fully local control (no vendor cloud dependency)
- ✅ MQTT interface (command / status / availability)
- ✅ Home Assistant MQTT Discovery (buttons + status sensor)
- ✅ Power-loss recovery macro (restores a known state after outages)
- ✅ OTA updates via ArduinoOTA
- ✅ Stateless design: Home Assistant is the source of truth

---

## Architecture overview

Home Assistant
|
| MQTT
v
MQTT Broker
|
| MQTT
v
ESP32 ---- IR ----> Heater


The ESP32 acts as a **smart IR endpoint**:
- it does not maintain internal state
- it only executes IR commands on request
- all logic and automations live in Home Assistant

---

## MQTT topics

Topics are built dynamically using values defined in `config.h`:

hvac/<DEVICE_AREA>/<DEVICE_NAME_MQTT>/command
hvac/<DEVICE_AREA>/<DEVICE_NAME_MQTT>/status
hvac/<DEVICE_AREA>/<DEVICE_NAME_MQTT>/availability

example:
hvac/bathroom/geyser/command
hvac/bathroom/geyser/status
hvac/bathroom/geyser/availability

## Supported MQTT commands

Payloads are **case-insensitive** strings published to the `command` topic:

- `power_toggle`
- `power_level`
- `oscillation_toggle`
- `sleep_timer`
- `temp_mode`
- `temp_up`
- `temp_down`
- `restore_state`  
  Runs the power-loss recovery macro (set known temperature, then OFF)

Each command execution is logged on the `status` topic.

---

## Power-loss recovery logic

After a **POWERON** or **BROWNOUT** reset, the ESP32:
1. waits for a configurable delay
2. runs a predefined IR macro
3. restores the device to a known state

The same macro can also be triggered:
- via MQTT (`restore_state`)
- via a physical button connected to the ESP32

This solves a common limitation of IR-only devices:  
**loss of state after power outages**.

---

## Home Assistant integration

The firmware publishes **MQTT Discovery** messages to automatically create:

- multiple **button entities** (one per IR command)
- a **status sensor**
- a single **device entry** in the HA device registry

No manual YAML configuration is required.

---

## Configuration files

### `config.h`
Non-secret configuration:
- logical placement (`DEVICE_AREA`, `DEVICE_NAME_MQTT`)
- GPIO mapping
- restore macro timing parameters

### `secrets.h`
Private credentials (not committed to GitHub):
- Wi-Fi
- MQTT broker
- OTA password

A template is provided as `secrets.example.h`.

---

## Library requirements

Tested with the following Arduino libraries (installed via Arduino IDE Library Manager):

- **IRremoteESP8266** by David Conran et al.  
  **Tested version:** `2.8.6`  
  > ⚠️ Newer versions (e.g. 2.9.x) may introduce breaking changes.  
  > If you encounter build or runtime issues, make sure you are using **exactly v2.8.6**.

- **PubSubClient** by Nick O’Leary  
  **Tested version:** `2.8`

The following components are provided by the **ESP32 Arduino core**:
- WiFi
- ArduinoOTA

No additional libraries are required.

---

## Build & upload

- Platform: **ESP32**
- IDE: **Arduino IDE**
- Board: ESP32 Dev Module (or compatible)
- OTA supported after first USB upload

---

## Project status / roadmap

- Firmware is functional and actively used
- Environmental sensors (temperature / humidity) planned
- Wiring diagram will be added in a future update

---

## License

See `LICENSE` file.
Mini-check prima di commit
