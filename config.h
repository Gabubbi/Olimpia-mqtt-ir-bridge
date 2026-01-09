#pragma once

// ---------------------
// Logical placement used to build MQTT topics: hvac/<AREA>/<DEVICE>/...
// Keep lowercase and MQTT-safe (letters, numbers, underscore)
// ---------------------
const char* DEVICE_AREA = "bathroom";
const char* DEVICE_NAME_MQTT = "geyser";

// ---------------------
// GPIO mapping (ESP32 DevKit)
// IR TX: GPIO driving the IR LED transistor stage
// Restore button: momentary switch to GND (INPUT_PULLUP)
// ---------------------
constexpr uint16_t PIN_IR_TX = 18;
constexpr uint8_t  PIN_BTN_RESTORE = 27;

// ---------------------
// Restore macro tuning
// ---------------------
constexpr uint32_t BOOT_DELAY_MS   = 15000; // delay after power-loss before macro
constexpr uint32_t MANUAL_DELAY_MS = 500;   // delay when manually triggered
constexpr uint16_t TEMP_STEPS_TO_45 = 20;   // 25 -> 45 (1°C per step)
constexpr uint16_t IR_GAP_MS        = 180;  // ms between IR commands
