// MQTT connection manager, adapted from axlan/pcb-map firmware/src/mqtt_client.*
// Adds a Last Will, an on-connect hook and runtime-changeable subscriptions.
#pragma once

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <functional>
#include <vector>

#include "device_config.h"

class MQTTClientManager {
 public:
  using ConnectedCallback = std::function<void(PubSubClient&)>;

  MQTTClientManager(const char* client_name, MQTT_CALLBACK_SIGNATURE);

  // Handles reconnection and processes incoming messages; call from loop().
  void Update();

  void OnConfigChange(const MQTTConfig& config);
  // Replaces the subscription list (and the Last Will) and reconnects.
  void SetTopics(const std::vector<String>& topics, const String& will_topic);
  void SetConnectedCallback(const ConnectedCallback& cb) { on_connected_ = cb; }

  bool Connected() { return mqtt_client_->connected(); }
  const MQTTConfig& Config() const { return config_; }
  PubSubClient* GetClient() { return mqtt_client_; }

 private:
  void Connect();
  void ForceReconnect();

  WiFiClient wifi_client_;
  WiFiClientSecure wifi_client_secure_;
  PubSubClient mqtt_client_secure_;
  PubSubClient mqtt_client_insecure_;
  PubSubClient* mqtt_client_;
  unsigned long last_mqtt_attempt_ms_ = 0;
  bool attempted_ = false;
  bool server_set_ = false;
  static constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
  std::vector<String> topics_;
  String will_topic_;
  MQTTConfig config_;
  String client_name_;
  ConnectedCallback on_connected_;
};
