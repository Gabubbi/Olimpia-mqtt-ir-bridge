/**
 * ESP32 MQTT IR Bridge (Home Assistant friendly)
 * ----------------------------------------------
 * Goal:
 *  - Control an IR-only / cloud-locked heater locally via Home Assistant
 *  - MQTT interface (command + status + availability)
 *  - Home Assistant MQTT Discovery (buttons + status sensor)
 *  - Power-loss recovery macro (run after POWERON/BROWNOUT)
 *  - OTA updates (ArduinoOTA)
 *
 * Topics (built dynamically from config.h):
 *  hvac/<DEVICE_AREA>/<DEVICE_NAME_MQTT>/command
 *  hvac/<DEVICE_AREA>/<DEVICE_NAME_MQTT>/status
 *  hvac/<DEVICE_AREA>/<DEVICE_NAME_MQTT>/availability
 *
 * Files:
 *  - config.h       : non-secret settings (area/name, pins, macro timings)
 *  - secrets.h      : real credentials (WiFi/MQTT/OTA)
 *  - secrets.example.h : template to create secrets.h
 */

#include <WiFi.h>
#include <PubSubClient.h>

#include <IRremoteESP8266.h>
#include <IRsend.h>

#include <ArduinoOTA.h>

#include "config.h"
#include "secrets.h"

#include <esp_system.h>  // esp_reset_reason()

// ------------------------------------------------------------
// MQTT topics built at runtime from config.h
// ------------------------------------------------------------
String mqttTopicCmd;
String mqttTopicStatus;
String mqttTopicAvail;

static void buildMqttTopics() {
  String base = "hvac/";
  base += DEVICE_AREA;
  base += "/";
  base += DEVICE_NAME_MQTT;

  mqttTopicCmd    = base + "/command";
  mqttTopicStatus = base + "/status";
  mqttTopicAvail  = base + "/availability";
}

// Forward declaration (default argument ONLY here)
static void publishStatus(const String& msg, bool retain = true);

// ------------------------------------------------------------
// Home Assistant MQTT Discovery
// ------------------------------------------------------------
static const char* HA_DISCOVERY_PREFIX = "homeassistant";

// Availability payloads
static const char* AVAIL_ONLINE  = "online";
static const char* AVAIL_OFFLINE = "offline";

// ------------------------------------------------------------
// Device identity (HA device registry block)
// Keep identifiers stable once deployed.
// ------------------------------------------------------------
static const char* DEVICE_IDENT = "olimpia_ir_esp32_01";
static const char* DEVICE_NAME  = "IR Heater ESP32 (bathroom)";
static const char* DEVICE_MODEL = "ESP32 IR Bridge";
static const char* DEVICE_MFR   = "DIY";

// ------------------------------------------------------------
// IR
// ------------------------------------------------------------
IRsend irsend(PIN_IR_TX);

// NEC 32-bit codes (example set for Olimpia Splendid)
static const uint32_t IR_POWER_TOGGLE       = 0xFF807F;
static const uint32_t IR_POWER_LEVEL        = 0xFF00FF;
static const uint32_t IR_OSCILLATION_TOGGLE = 0xFF20DF;
static const uint32_t IR_SLEEP_TIMER        = 0xFF30CF;
static const uint32_t IR_TEMP_MODE          = 0xFFB04F;
static const uint32_t IR_TEMP_UP            = 0xFF906F;
static const uint32_t IR_TEMP_DOWN          = 0xFF10EF;

// ------------------------------------------------------------
// Button debounce (restore macro trigger)
// ------------------------------------------------------------
static const uint32_t DEBOUNCE_MS = 80;
static bool lastBtnState = true;      // INPUT_PULLUP: HIGH idle, LOW pressed
static uint32_t lastBtnChangeMs = 0;

// ------------------------------------------------------------
// Clients
// ------------------------------------------------------------
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Non-blocking retry timers
static const uint32_t WIFI_RETRY_MS = 5000;
static const uint32_t MQTT_RETRY_MS = 3000;
static uint32_t lastWifiAttemptMs = 0;
static uint32_t lastMqttAttemptMs = 0;

// Flags
static bool discoveryPublished = false;
static bool otaStarted = false;

// Boot restore scheduler
static bool bootRestoreScheduled = false;
static uint32_t bootRestoreAtMs = 0;

// ============================================================
// OTA
// ============================================================
static void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: start");
    publishStatus("ota:start");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("OTA: end");
    publishStatus("ota:end");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA: error[%u]\n", error);
    publishStatus(String("ota:error:") + String((int)error));
  });

  ArduinoOTA.begin();
  Serial.println("OTA: ready");
}

// ============================================================
// MQTT helpers
// ============================================================
static void mqttPublish(const char* topic, const String& payload, bool retain = true) {
  bool ok = mqtt.publish(topic, payload.c_str(), retain);
  if (!ok) {
    Serial.print("MQTT publish FAILED topic=");
    Serial.print(topic);
    Serial.print(" len=");
    Serial.println(payload.length());
  }
}

static void publishStatus(const String& msg, bool retain) {
  if (mqtt.connected()) {
    mqtt.publish(mqttTopicStatus.c_str(), msg.c_str(), retain);
  }
}

static void setAvailability(const char* state) {
  if (mqtt.connected()) {
    mqtt.publish(mqttTopicAvail.c_str(), state, true);
  }
}

// ============================================================
// Reset reason helpers
// ============================================================
static String resetReasonToString(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_EXT:       return "EXT_RESET(EN)";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "UNKNOWN";
  }
}

static bool shouldRunRestoreOnBoot(esp_reset_reason_t r) {
  return (r == ESP_RST_POWERON) || (r == ESP_RST_BROWNOUT);
}

// ============================================================
// IR helpers
// ============================================================
static void sendNEC(uint32_t code) {
  // Send twice for higher reliability
  irsend.sendNEC(code, 32);
  delay(50);
  irsend.sendNEC(code, 32);
  yield();
}

static void sendCmd(uint32_t code, const char* tag) {
  sendNEC(code);
  delay(IR_GAP_MS);
  publishStatus(String("sent:") + tag);
  Serial.println(String("IR sent: ") + tag);
  yield();
  delay(1);
}

static void waitMsWithYield(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    delay(5);
    yield();
  }
}

// ============================================================
// Restore macro (example: set 45°C then OFF)
// ============================================================
static void restoreTo45AndOff(const char* triggerTag, uint32_t preDelayMs) {
  publishStatus(String("restore:start:") + triggerTag);
  Serial.println(String("restore:start:") + triggerTag);

  if (preDelayMs > 0) waitMsWithYield(preDelayMs);

  sendCmd(IR_POWER_TOGGLE, "power_toggle_on");
  sendCmd(IR_TEMP_MODE, "temp_mode");

  for (uint16_t i = 0; i < TEMP_STEPS_TO_45; i++) {
    sendCmd(IR_TEMP_UP, "temp_up");
    yield();
    delay(1);
  }

  sendCmd(IR_POWER_TOGGLE, "power_toggle_off");

  publishStatus(String("restore:done:") + triggerTag);
  Serial.println(String("restore:done:") + triggerTag);
}

// ============================================================
// Home Assistant MQTT Discovery helpers
// ============================================================
static String deviceBlockJson() {
  String d = "{";
  d += "\"identifiers\":[\"" + String(DEVICE_IDENT) + "\"],";
  d += "\"name\":\"" + String(DEVICE_NAME) + "\",";
  d += "\"manufacturer\":\"" + String(DEVICE_MFR) + "\",";
  d += "\"model\":\"" + String(DEVICE_MODEL) + "\"";
  d += "}";
  return d;
}

static void publishButtonDiscovery(const char* object_id, const char* name, const char* payload_press) {
  String topic = String(HA_DISCOVERY_PREFIX) + "/button/" + object_id + "/config";

  String j = "{";
  j += "\"name\":\"" + String(name) + "\",";
  j += "\"command_topic\":\"" + mqttTopicCmd + "\",";
  j += "\"payload_press\":\"" + String(payload_press) + "\",";
  j += "\"availability_topic\":\"" + mqttTopicAvail + "\",";
  j += "\"payload_available\":\"" + String(AVAIL_ONLINE) + "\",";
  j += "\"payload_not_available\":\"" + String(AVAIL_OFFLINE) + "\",";
  j += "\"unique_id\":\"" + String(object_id) + "\",";
  j += "\"device\":" + deviceBlockJson();
  j += "}";

  mqttPublish(topic.c_str(), j, true);
}

static void publishStatusSensorDiscovery() {
  const char* object_id = "olimpia_ir_status_01";
  String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + object_id + "/config";

  String j = "{";
  j += "\"name\":\"IR Heater Status\",";
  j += "\"state_topic\":\"" + mqttTopicStatus + "\",";
  j += "\"availability_topic\":\"" + mqttTopicAvail + "\",";
  j += "\"payload_available\":\"" + String(AVAIL_ONLINE) + "\",";
  j += "\"payload_not_available\":\"" + String(AVAIL_OFFLINE) + "\",";
  j += "\"unique_id\":\"" + String(object_id) + "\",";
  j += "\"device\":" + deviceBlockJson();
  j += "}";

  mqttPublish(topic.c_str(), j, true);
}

static void publishAllDiscovery() {
  publishStatusSensorDiscovery();

  publishButtonDiscovery("ir_btn_power_toggle_01",   "IR Power Toggle",       "power_toggle");
  publishButtonDiscovery("ir_btn_power_level_01",    "IR Power Level",        "power_level");
  publishButtonDiscovery("ir_btn_osc_toggle_01",     "IR Oscillation Toggle", "oscillation_toggle");
  publishButtonDiscovery("ir_btn_sleep_timer_01",    "IR Sleep Timer",        "sleep_timer");
  publishButtonDiscovery("ir_btn_temp_mode_01",      "IR Temp Mode",          "temp_mode");
  publishButtonDiscovery("ir_btn_temp_up_01",        "IR Temp Up",            "temp_up");
  publishButtonDiscovery("ir_btn_temp_down_01",      "IR Temp Down",          "temp_down");
  publishButtonDiscovery("ir_btn_restore_45_off_01", "Restore 45C + OFF",     "restore_state");

  publishStatus("discovery:published");
  Serial.println("HA discovery published (retained).");
}

// ============================================================
// MQTT command handler
// ============================================================
static void handleCommand(const String& cmd) {
  Serial.print("MQTT cmd: ");
  Serial.println(cmd);

  if (cmd == "power_toggle")            sendCmd(IR_POWER_TOGGLE, "power_toggle");
  else if (cmd == "power_level")        sendCmd(IR_POWER_LEVEL, "power_level");
  else if (cmd == "oscillation_toggle") sendCmd(IR_OSCILLATION_TOGGLE, "oscillation_toggle");
  else if (cmd == "sleep_timer")        sendCmd(IR_SLEEP_TIMER, "sleep_timer");
  else if (cmd == "temp_mode")          sendCmd(IR_TEMP_MODE, "temp_mode");
  else if (cmd == "temp_up")            sendCmd(IR_TEMP_UP, "temp_up");
  else if (cmd == "temp_down")          sendCmd(IR_TEMP_DOWN, "temp_down");
  else if (cmd == "restore_state")      restoreTo45AndOff("mqtt", MANUAL_DELAY_MS);
  else {
    publishStatus("error:unknown_cmd");
    Serial.println("error:unknown_cmd");
  }
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  (void)topic;

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  msg.trim();
  msg.toLowerCase();
  handleCommand(msg);
}

// ============================================================
// Button polling
// ============================================================
static void pollRestoreButton() {
  bool cur = digitalRead(PIN_BTN_RESTORE);

  if (cur != lastBtnState && (millis() - lastBtnChangeMs) > DEBOUNCE_MS) {
    lastBtnChangeMs = millis();
    lastBtnState = cur;

    if (cur == LOW) {
      publishStatus("button:restore_pressed");
      Serial.println("button:restore_pressed");
      restoreTo45AndOff("button", MANUAL_DELAY_MS);
    }
  }
}

// ============================================================
// WiFi / MQTT (non-blocking reconnect)
// ============================================================
static void ensureWiFiNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;

  Serial.print("WiFi: connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void ensureMQTTNonBlocking() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  uint32_t now = millis();
  if (now - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = now;

  Serial.print("MQTT: connecting to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  String clientId = "esp32-ir-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok = mqtt.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASS,
    mqttTopicAvail.c_str(), // LWT topic
    0,
    true,
    AVAIL_OFFLINE
  );

  if (ok) {
    Serial.println("MQTT: connected");

    mqtt.subscribe(mqttTopicCmd.c_str());

    setAvailability(AVAIL_ONLINE);
    publishStatus("online");
    discoveryPublished = false;
  } else {
    Serial.print("MQTT: FAILED, state=");
    Serial.println(mqtt.state());
  }
}

// ============================================================
// Arduino lifecycle
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("SETUP: start");

  // Build MQTT topics from config.h
  buildMqttTopics();

  Serial.print("MQTT topics: ");
  Serial.print(mqttTopicCmd);
  Serial.print(" | ");
  Serial.print(mqttTopicStatus);
  Serial.print(" | ");
  Serial.println(mqttTopicAvail);

  // GPIO init
  pinMode(PIN_BTN_RESTORE, INPUT_PULLUP);

  // IR init
  irsend.begin();

  // MQTT init
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  // Needed for HA discovery JSON payload size
  mqtt.setBufferSize(2048);

  // Reset reason
  esp_reset_reason_t r = esp_reset_reason();
  Serial.print("boot:reset_reason:");
  Serial.println(resetReasonToString(r));

  // Schedule restore macro only on power-loss
  if (shouldRunRestoreOnBoot(r)) {
    bootRestoreScheduled = true;
    bootRestoreAtMs = millis() + BOOT_DELAY_MS;
    Serial.print("boot:powerloss restore scheduled at +ms=");
    Serial.println(BOOT_DELAY_MS);
  }

  ensureWiFiNonBlocking();
}

void loop() {
  // Alive log every 10 seconds
  static uint32_t tAlive = 0;
  if (millis() - tAlive > 10000) {
    tAlive = millis();
    Serial.print("LOOP: alive | WiFi=");
    Serial.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");
    Serial.print(" | MQTT=");
    Serial.println(mqtt.connected() ? "OK" : "NO");
  }

  ensureWiFiNonBlocking();

  // Start OTA once WiFi is connected
  if (WiFi.status() == WL_CONNECTED && !otaStarted) {
    otaStarted = true;

    Serial.print("WiFi: OK, IP=");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("WiFi RSSI: ");
    Serial.println(WiFi.RSSI());

    setupOTA();
  }

  if (otaStarted) ArduinoOTA.handle();

  ensureMQTTNonBlocking();

  if (mqtt.connected()) {
    mqtt.loop();

    // Publish HA discovery once per session (retained)
    if (!discoveryPublished) {
      publishAllDiscovery();
      discoveryPublished = true;

      esp_reset_reason_t r = esp_reset_reason();
      publishStatus(String("boot:reset_reason:") + resetReasonToString(r));
      publishStatus("ready");
    }
  }

  // Run scheduled macro after power-loss delay
  if (bootRestoreScheduled && millis() >= bootRestoreAtMs) {
    bootRestoreScheduled = false;
    if (mqtt.connected()) publishStatus("boot:powerloss_restore:run");
    restoreTo45AndOff("boot_powerloss", 0);
  }

  pollRestoreButton();

  // Give time to RTOS/WDT
  delay(1);
}
