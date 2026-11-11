#include "mqtt_client.h"

#include <WiFi.h>

#include "esp_log.h"

[[maybe_unused]] static const char* TAG = "mqtt_client";

MQTTClientManager::MQTTClientManager(const char* client_name, MQTT_CALLBACK_SIGNATURE)
    : mqtt_client_secure_(wifi_client_secure_),
      mqtt_client_insecure_(wifi_client_),
      client_name_(client_name) {
  for (PubSubClient* c : {&mqtt_client_secure_, &mqtt_client_insecure_}) {
    c->setCallback(callback);
    c->setBufferSize(1024);
    c->setKeepAlive(30);
  }
  // As in pcb-map: TLS without certificate pinning (encrypted, not authenticated).
  wifi_client_secure_.setInsecure();
  mqtt_client_ = &mqtt_client_insecure_;
}

void MQTTClientManager::Update() {
  if (mqtt_client_->connected()) {
    mqtt_client_->loop();
  } else {
    Connect();
  }
}

void MQTTClientManager::Connect() {
  if (!server_set_ || WiFi.status() != WL_CONNECTED || config_.host.length() == 0) return;

  unsigned long now = millis();
  if (attempted_ && now - last_mqtt_attempt_ms_ < MQTT_RETRY_INTERVAL_MS) return;
  attempted_ = true;
  last_mqtt_attempt_ms_ = now;

  ESP_LOGI(TAG, "[MQTT] Connecting to %s:%u as %s", config_.host.c_str(), config_.port,
           client_name_.c_str());

  const char* user = config_.user_name.length() ? config_.user_name.c_str() : nullptr;
  const char* pass = config_.password.length() ? config_.password.c_str() : nullptr;
  bool ok;
  if (will_topic_.length()) {
    ok = mqtt_client_->connect(client_name_.c_str(), user, pass, will_topic_.c_str(), 0, true,
                               "offline");
  } else {
    ok = mqtt_client_->connect(client_name_.c_str(), user, pass);
  }

  if (!ok) {
    ESP_LOGW(TAG, "[MQTT] Connection failed, rc=%d. Retrying in %lums", mqtt_client_->state(),
             MQTT_RETRY_INTERVAL_MS);
    return;
  }
  ESP_LOGI(TAG, "[MQTT] Connected");
  if (will_topic_.length()) mqtt_client_->publish(will_topic_.c_str(), "online", true);
  for (const auto& topic : topics_) {
    mqtt_client_->subscribe(topic.c_str());
    ESP_LOGI(TAG, "[MQTT] Subscribed to %s", topic.c_str());
  }
  if (on_connected_) on_connected_(*mqtt_client_);
}

void MQTTClientManager::ForceReconnect() {
  if (mqtt_client_->connected()) mqtt_client_->disconnect();
  attempted_ = false;
  Connect();
}

void MQTTClientManager::OnConfigChange(const MQTTConfig& config) {
  ESP_LOGI(TAG, "[MQTT] Config: %s:%u (TLS: %s, user: %s)", config.host.c_str(), config.port,
           config.use_tls ? "yes" : "no", config.user_name.c_str());
  if (mqtt_client_->connected()) mqtt_client_->disconnect();
  config_ = config;
  mqtt_client_ = config_.use_tls ? &mqtt_client_secure_ : &mqtt_client_insecure_;
  mqtt_client_->setServer(config_.host.c_str(), config_.port);
  server_set_ = true;
  ForceReconnect();
}

void MQTTClientManager::SetTopics(const std::vector<String>& topics, const String& will_topic) {
  topics_ = topics;
  will_topic_ = will_topic;
  ForceReconnect();
}
