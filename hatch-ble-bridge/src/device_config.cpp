#include "device_config.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "esp_log.h"

[[maybe_unused]] static const char* TAG = "device_config";

// Restarts the UDP listener whenever WiFi (re)connects (from pcb-map).
class ManagedUDP {
  WiFiUDP udp_;
  uint16_t port_ = 0;
  bool active_ = false;

 public:
  bool begin(uint16_t p) {
    port_ = p;
    udp_.stop();
    active_ = (udp_.begin(port_) == 1);
    return active_;
  }

  void stop() {
    udp_.stop();
    active_ = false;
  }

  void maintain() {
    if (port_ == 0) return;
    bool connected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != INADDR_NONE);
    if (connected && !active_) {
      begin(port_);
    } else if (!connected && active_) {
      stop();
    }
  }

  bool isValid() const { return active_ && (WiFi.status() == WL_CONNECTED); }

  WiFiUDP& get() { return udp_; }
};

static Preferences prefs_s;
static ManagedUDP udp_s;
static MQTTConfigCallback on_mqtt_s;
static BridgeConfigCallback on_bridge_s;
static StatusProvider status_s;
static ForgetWifiCallback forget_wifi_s;

// Preferences logs an error for every missing key, so only read keys that exist.
static String GetString(const char* key, const String& def) {
  return prefs_s.isKey(key) ? prefs_s.getString(key, def) : def;
}

void DeviceConfig::LoadMQTT(MQTTConfig* c) {
  MQTTConfig d;
  prefs_s.begin("mqtt", false);
  c->host = GetString("host", d.host);
  c->port = (uint16_t)prefs_s.getUInt("port", d.port);
  c->use_tls = prefs_s.getBool("use_tls", d.use_tls);
  c->user_name = GetString("user_name", d.user_name);
  c->password = GetString("password", d.password);
  prefs_s.end();
}

void DeviceConfig::SaveMQTT(const MQTTConfig& c) {
  prefs_s.begin("mqtt", false);
  prefs_s.putString("host", c.host);
  prefs_s.putUInt("port", c.port);
  prefs_s.putBool("use_tls", c.use_tls);
  prefs_s.putString("user_name", c.user_name);
  prefs_s.putString("password", c.password);
  prefs_s.end();
}

void DeviceConfig::LoadBridge(BridgeConfig* c) {
  BridgeConfig d;
  prefs_s.begin("bridge", false);
  c->picker_topic = GetString("picker_topic", d.picker_topic);
  c->hatch_topic = GetString("hatch_topic", d.hatch_topic);
  c->hatch_address = GetString("hatch_addr", d.hatch_address);
  c->sound_delay_ms = prefs_s.getUInt("sound_delay", d.sound_delay_ms);
  prefs_s.end();
}

void DeviceConfig::SaveBridge(const BridgeConfig& c) {
  prefs_s.begin("bridge", false);
  prefs_s.putString("picker_topic", c.picker_topic);
  prefs_s.putString("hatch_topic", c.hatch_topic);
  prefs_s.putString("hatch_addr", c.hatch_address);
  prefs_s.putUInt("sound_delay", c.sound_delay_ms);
  prefs_s.end();
}

static void SendUDPReply(const IPAddress& ip, uint16_t port, const char* msg) {
  auto& udp = udp_s.get();
  udp.beginPacket(ip, port);
  udp.print(msg);
  udp.endPacket();
}

// Splits "a,b,c" into exactly `n` fields (empty fields allowed). Returns false
// on a different field count.
static bool SplitFields(const char* s, String* out, int n) {
  int idx = 0;
  String cur;
  for (const char* p = s; *p; ++p) {
    if (*p == ',') {
      if (idx >= n) return false;
      out[idx++] = cur;
      cur = "";
    } else {
      cur += *p;
    }
  }
  if (idx >= n) return false;
  out[idx++] = cur;
  return idx == n;
}

static bool StartsWith(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void ProcessUDPCommands() {
  static constexpr size_t BUFFER_SIZE = 256;
  char input[BUFFER_SIZE];

  auto& udp = udp_s.get();
  int packet_size = udp.parsePacket();
  if (packet_size <= 0) return;

  IPAddress ip = udp.remoteIP();
  uint16_t port = udp.remotePort();
  if (packet_size >= (int)BUFFER_SIZE) {
    ESP_LOGW(TAG, "[UDP] Command too big %d >= %u", packet_size, BUFFER_SIZE);
    udp.clear();
    SendUDPReply(ip, port, "CMD_SIZE");
    return;
  }
  int len = udp.read(input, packet_size);
  if (len != packet_size) {
    SendUDPReply(ip, port, "READ_ERROR");
    return;
  }
  input[len] = 0;
  while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r')) input[--len] = 0;

  // Don't log passwords.
  ESP_LOGI(TAG, "[UDP] CMD from %s:%u: %.10s...", ip.toString().c_str(), port, input);

  if (StartsWith(input, "SET_MQTT ")) {
    // Same format as pcb-map: "SET_MQTT host,port,tls,user,password"
    String f[5];
    if (!SplitFields(input + 9, f, 5) || f[0].length() == 0) {
      SendUDPReply(ip, port, "PARSE_ERROR");
      return;
    }
    MQTTConfig c;
    c.host = f[0];
    c.port = (uint16_t)f[1].toInt();
    if (c.port == 0) c.port = (f[2] == "1") ? 8883 : 1883;
    c.use_tls = (f[2] == "1");
    c.user_name = f[3];
    c.password = f[4];
    DeviceConfig::SaveMQTT(c);
    if (on_mqtt_s) on_mqtt_s(c);
    SendUDPReply(ip, port, "OK");
  } else if (StartsWith(input, "SET_BRIDGE ")) {
    String f[4];
    if (!SplitFields(input + 11, f, 4) || f[0].length() == 0 || f[1].length() == 0) {
      SendUDPReply(ip, port, "PARSE_ERROR");
      return;
    }
    BridgeConfig c;
    c.picker_topic = f[0];
    c.hatch_topic = f[1];
    c.hatch_address = f[2];
    c.hatch_address.toLowerCase();
    c.sound_delay_ms = f[3].length() ? (uint32_t)f[3].toInt() : BridgeConfig().sound_delay_ms;
    DeviceConfig::SaveBridge(c);
    if (on_bridge_s) on_bridge_s(c);
    SendUDPReply(ip, port, "OK");
  } else if (strcmp(input, "GET_STATUS") == 0) {
    String s = status_s ? status_s() : String("{}");
    SendUDPReply(ip, port, s.c_str());
  } else if (strcmp(input, "FORGET_WIFI") == 0) {
    SendUDPReply(ip, port, "OK");
    if (forget_wifi_s) forget_wifi_s();
  } else {
    SendUDPReply(ip, port, "PARSE_ERROR");
  }
}

void DeviceConfig::Begin(const MQTTConfigCallback& on_mqtt, const BridgeConfigCallback& on_bridge,
                         const StatusProvider& status, const ForgetWifiCallback& forget_wifi) {
  on_mqtt_s = on_mqtt;
  on_bridge_s = on_bridge;
  status_s = status;
  forget_wifi_s = forget_wifi;

  MQTTConfig mqtt;
  LoadMQTT(&mqtt);
  BridgeConfig bridge;
  LoadBridge(&bridge);
  if (on_bridge_s) on_bridge_s(bridge);
  if (on_mqtt_s) on_mqtt_s(mqtt);

  udp_s.begin(CONFIG_UDP_PORT);
  MDNS.addService("mqtt-config", "udp", CONFIG_UDP_PORT);
  ESP_LOGI(TAG, "Config service registered: _mqtt-config._udp port %u", CONFIG_UDP_PORT);
}

void DeviceConfig::Update() {
  udp_s.maintain();
  if (!udp_s.isValid()) return;
  ProcessUDPCommands();
}
