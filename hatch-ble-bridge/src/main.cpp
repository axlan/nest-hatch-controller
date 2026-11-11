// hatch-ble-bridge: ESP32-S3 firmware that applies the Nest picker's color and
// sound (MQTT) to a Hatch Rest gen 1 night light over BLE.
//
// Provisioning follows axlan/pcb-map:
//   - WiFi: WiFiManager captive portal on the "hatch-ble-bridge-ap" access point
//     (the MQTT broker can be entered there too).
//   - MQTT broker and bridge settings: UDP commands on port 5432
//     (_mqtt-config._udp via mDNS), see device_config.h. Use the
//     `hatch-ble-bridge-setup` Python tool in ../hatch-bridge.
//   - OTA: pio run -t upload --upload-port hatch-ble-bridge.local
//
// MQTT topics:
//   <picker>/color  in   {"h":210,...} or {"r":..,"g":..,"b":..}
//   <picker>/sound  in   {"id":3,"name":"noise"}, a bare id, or a name
//   <hatch>/state   out  retained JSON (power, color, brightness, sound, volume)
//   <hatch>/status  out  retained online/offline (Last Will)
//   hatch-ble-bridge/ping -> hatch-ble-bridge/pong
//   hatch-ble-bridge/clear_wifi  forget WiFi and reboot into the setup AP
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>

#include "device_config.h"
#include "esp_log.h"
#include "hatch_controller.h"
#include "mqtt_client.h"

[[maybe_unused]] static const char* TAG = "main";

static constexpr const char* BASE_NAME = "hatch-ble-bridge";
static constexpr const char* ADHOC_AP = "hatch-ble-bridge-ap";
static constexpr const char* MQTT_PING_TOPIC = "hatch-ble-bridge/ping";
static constexpr const char* MQTT_PONG_TOPIC = "hatch-ble-bridge/pong";
static constexpr const char* MQTT_CLEAR_WIFI_TOPIC = "hatch-ble-bridge/clear_wifi";

// Hatch Rest gen 1 sound ids
static const struct {
  const char* name;
  uint8_t id;
} SOUNDS[] = {{"none", 0},     {"stream", 2},    {"noise", 3},     {"dryer", 4},
              {"ocean", 5},    {"wind", 6},      {"rain", 7},      {"bird", 9},
              {"crickets", 10}, {"brahms", 11},  {"twinkle", 13},  {"rockabye", 14}};

static void HandleMQTTMessage(const char* topic, byte* payload, unsigned int length);

static MQTTClientManager mqtt_manager(BASE_NAME, HandleMQTTMessage);
static HatchController hatch;
static BridgeConfig bridge_config;
static String color_topic, sound_topic, state_topic, status_topic;
static bool forget_wifi_requested = false;

static void ForgetWifiAndRestart() {
  ESP_LOGI(TAG, "Clearing WiFi credentials and restarting");
  WiFiManager wm;
  wm.resetSettings();
  delay(1000);
  ESP.restart();
}

static void HsvToRgb(float h, float s, uint8_t* r, uint8_t* g, uint8_t* b) {
  h = fmodf(h, 360.0f);
  if (h < 0) h += 360.0f;
  float c = s, hp = h / 60.0f, x = c * (1 - fabsf(fmodf(hp, 2.0f) - 1)), m = 1 - c;
  float rf, gf, bf;
  if (hp < 1) { rf = c; gf = x; bf = 0; }
  else if (hp < 2) { rf = x; gf = c; bf = 0; }
  else if (hp < 3) { rf = 0; gf = c; bf = x; }
  else if (hp < 4) { rf = 0; gf = x; bf = c; }
  else if (hp < 5) { rf = x; gf = 0; bf = c; }
  else { rf = c; gf = 0; bf = x; }
  *r = (uint8_t)lroundf((rf + m) * 255);
  *g = (uint8_t)lroundf((gf + m) * 255);
  *b = (uint8_t)lroundf((bf + m) * 255);
}

static void HandleColor(const byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) || !doc.is<JsonObject>()) {
    ESP_LOGW(TAG, "Bad color payload");
    return;
  }
  uint8_t r, g, b;
  if (doc["h"].is<float>()) {
    float s = constrain(doc["s"] | 100.0f, 0.0f, 100.0f) / 100.0f;
    HsvToRgb(doc["h"].as<float>(), s, &r, &g, &b);
  } else if (doc["r"].is<int>() && doc["g"].is<int>() && doc["b"].is<int>()) {
    int rr = doc["r"], gg = doc["g"], bb = doc["b"];
    int peak = max(rr, max(gg, bb));
    if (peak <= 0) peak = 255;  // black: leave as-is (brightness is the Hatch's)
    r = (uint8_t)(rr * 255 / peak);
    g = (uint8_t)(gg * 255 / peak);
    b = (uint8_t)(bb * 255 / peak);
  } else {
    ESP_LOGW(TAG, "Color payload has neither h nor r/g/b");
    return;
  }
  if (r == 254 && g == 254 && b == 254) r = 255;  // 254,254,254 = Hatch gradient mode
  ESP_LOGI(TAG, "color #%02x%02x%02x", r, g, b);
  hatch.RequestColor(r, g, b);
}

static int LookupSound(const char* name) {
  for (const auto& s : SOUNDS)
    if (strcasecmp(s.name, name) == 0) return s.id;
  return -1;
}

static void HandleSound(const byte* payload, unsigned int length) {
  String text;
  text.reserve(length);
  for (unsigned i = 0; i < length; i++) text += (char)payload[i];
  text.trim();

  int id = LookupSound(text.c_str());
  if (id < 0) {
    JsonDocument doc;
    if (!deserializeJson(doc, text)) {
      if (doc.is<int>()) id = doc.as<int>();
      else if (doc["id"].is<int>()) id = doc["id"];
      else if (doc["name"].is<const char*>()) id = LookupSound(doc["name"]);
    }
  }
  if (id < 0 || id > 255) {
    ESP_LOGW(TAG, "Unrecognised sound payload");
    return;
  }
  ESP_LOGI(TAG, "sound %d", id);
  hatch.RequestSound((uint8_t)id);
}

static void HandleMQTTMessage(const char* topic, byte* payload, unsigned int length) {
  if (color_topic == topic) {
    HandleColor(payload, length);
  } else if (sound_topic == topic) {
    HandleSound(payload, length);
  } else if (strcmp(topic, MQTT_PING_TOPIC) == 0) {
    mqtt_manager.GetClient()->publish(MQTT_PONG_TOPIC, "pong");
  } else if (strcmp(topic, MQTT_CLEAR_WIFI_TOPIC) == 0) {
    forget_wifi_requested = true;  // don't restart from inside the MQTT callback
  }
}

static void OnBridgeConfig(const BridgeConfig& c) {
  bridge_config = c;
  color_topic = c.picker_topic + "/color";
  sound_topic = c.picker_topic + "/sound";
  state_topic = c.hatch_topic + "/state";
  status_topic = c.hatch_topic + "/status";
  ESP_LOGI(TAG, "Bridge: %s -> Hatch %s (state on %s)", c.picker_topic.c_str(),
           c.hatch_address.length() ? c.hatch_address.c_str() : "(first found)", state_topic.c_str());
  hatch.SetTarget(c.hatch_address, c.sound_delay_ms);
  mqtt_manager.SetTopics({color_topic, sound_topic, MQTT_PING_TOPIC, MQTT_CLEAR_WIFI_TOPIC},
                         status_topic);
}

static String StatusJson() {
  const MQTTConfig& m = mqtt_manager.Config();
  String s = "{\"name\":\"";
  s += BASE_NAME;
  s += "\",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  s += ",\"rssi\":" + String(WiFi.RSSI());
  s += ",\"uptime\":" + String(millis() / 1000);
  s += ",\"heap_free\":" + String(ESP.getFreeHeap());
  s += ",\"mqtt\":{\"host\":\"" + m.host + "\",\"port\":" + String(m.port);
  s += ",\"tls\":" + String(m.use_tls ? "true" : "false");
  s += ",\"connected\":" + String(mqtt_manager.Connected() ? "true" : "false") + "}";
  s += ",\"bridge\":{\"picker_topic\":\"" + bridge_config.picker_topic + "\"";
  s += ",\"hatch_topic\":\"" + bridge_config.hatch_topic + "\"";
  s += ",\"hatch_address\":\"" + bridge_config.hatch_address + "\"";
  s += ",\"sound_delay_ms\":" + String(bridge_config.sound_delay_ms) + "}";
  s += ",\"hatch\":" + hatch.StatusJson() + "}";
  return s;
}

static void SetupOTA() {
  ArduinoOTA.setHostname(BASE_NAME);
  ArduinoOTA.onStart([]() { ESP_LOGI(TAG, "OTA update starting"); });
  ArduinoOTA.onEnd([]() { ESP_LOGI(TAG, "OTA update complete"); });
  ArduinoOTA.onError([](ota_error_t e) { ESP_LOGE(TAG, "OTA error %u", e); });
  ArduinoOTA.begin();
}

// WiFiManager portal, with MQTT fields as in pcb-map. Blocks until WiFi is up.
static void RunWifiManager() {
  WiFiManager wm;
  MQTTConfig config;
  DeviceConfig::LoadMQTT(&config);

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", config.port);
  WiFiManagerParameter p_host("host", "MQTT Host", config.host.c_str(), 64);
  WiFiManagerParameter p_port("port", "MQTT Port", port_str, 6);
  WiFiManagerParameter p_user("user", "MQTT User", config.user_name.c_str(), 64);
  WiFiManagerParameter p_pass("pass", "MQTT Password", config.password.c_str(), 64);
  WiFiManagerParameter p_tls("tls", "Use TLS (1/0)", config.use_tls ? "1" : "0", 2);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.addParameter(&p_tls);
  wm.setHostname(BASE_NAME);

  if (!wm.autoConnect(ADHOC_AP)) {
    ESP_LOGE(TAG, "WiFi setup failed, restarting");
    ESP.restart();
  }

  MQTTConfig updated = config;
  updated.host = p_host.getValue();
  updated.port = (uint16_t)atoi(p_port.getValue());
  updated.user_name = p_user.getValue();
  updated.password = p_pass.getValue();
  updated.use_tls = strcmp(p_tls.getValue(), "1") == 0;
  if (updated.host != config.host || updated.port != config.port ||
      updated.user_name != config.user_name || updated.password != config.password ||
      updated.use_tls != config.use_tls) {
    ESP_LOGI(TAG, "Saving MQTT config from WiFi portal");
    DeviceConfig::SaveMQTT(updated);
  }
}

void setup() {
  Serial.begin(115200);
  hatch.Begin();  // BLE scanning/connecting can start before WiFi is up
  RunWifiManager();
  WiFi.setSleep(true);  // required for WiFi + BLE coexistence

  MDNS.begin(BASE_NAME);
  SetupOTA();
  // Re-publish the last known Hatch state on every (re)connect.
  mqtt_manager.SetConnectedCallback([](PubSubClient&) { hatch.MarkStateDirty(); });
  DeviceConfig::Begin([](const MQTTConfig& c) { mqtt_manager.OnConfigChange(c); }, OnBridgeConfig,
                      StatusJson, []() { forget_wifi_requested = true; });
  ESP_LOGI(TAG, "Ready: %s.local (%s)", BASE_NAME, WiFi.localIP().toString().c_str());
}

void loop() {
  ArduinoOTA.handle();
  DeviceConfig::Update();
  mqtt_manager.Update();

  String state;
  if (mqtt_manager.Connected() && hatch.PopStateJson(&state)) {
    mqtt_manager.GetClient()->publish(state_topic.c_str(), state.c_str(), true);
  }

  if (forget_wifi_requested) ForgetWifiAndRestart();
  delay(5);
}
