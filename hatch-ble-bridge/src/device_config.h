// Persistent settings plus the UDP configuration service.
//
// Follows the provisioning scheme of axlan/pcb-map (firmware/src/mqtt_config.*):
// settings live in NVS (Preferences) and can be changed at runtime with plain
// text UDP commands on port 5432, advertised over mDNS as _mqtt-config._udp.
//
//   SET_MQTT host,port,use_tls(0/1),user,password                 -> OK
//   SET_BRIDGE picker_topic,hatch_topic,hatch_address,sound_delay_ms -> OK
//       (empty hatch_address = use the first Hatch found)
//   GET_STATUS                                                    -> JSON
//   FORGET_WIFI                                                   -> OK, then reboot into the setup AP
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <functional>

static constexpr uint16_t CONFIG_UDP_PORT = 5432;

#ifndef DEFAULT_MQTT_HOST
#define DEFAULT_MQTT_HOST "mqtt.local"
#endif

struct MQTTConfig {
  String host = DEFAULT_MQTT_HOST;
  uint16_t port = 1883;
  bool use_tls = false;
  String user_name;
  String password;
};

struct BridgeConfig {
  String picker_topic = "nest/picker";  // reads <picker_topic>/color and /sound
  String hatch_topic = "hatch/rest";    // publishes <hatch_topic>/state and /status
  String hatch_address;                 // "" = first Hatch seen
  uint32_t sound_delay_ms = 600;        // sound must be stable this long before it is sent
};

using MQTTConfigCallback = std::function<void(const MQTTConfig&)>;
using BridgeConfigCallback = std::function<void(const BridgeConfig&)>;
using StatusProvider = std::function<String()>;
using ForgetWifiCallback = std::function<void()>;

class DeviceConfig {
 public:
  static void LoadMQTT(MQTTConfig* config);
  static void SaveMQTT(const MQTTConfig& config);
  static void LoadBridge(BridgeConfig* config);
  static void SaveBridge(const BridgeConfig& config);

  // Loads both configs, invokes the callbacks once with them and starts the
  // UDP service + mDNS advertisement (MDNS.begin must already have run).
  static void Begin(const MQTTConfigCallback& on_mqtt, const BridgeConfigCallback& on_bridge,
                    const StatusProvider& status, const ForgetWifiCallback& forget_wifi);
  static void Update();
};
