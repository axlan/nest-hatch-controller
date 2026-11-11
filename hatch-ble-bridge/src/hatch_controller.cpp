#include "hatch_controller.h"

#include <NimBLEDevice.h>

#include "esp_log.h"

[[maybe_unused]] static const char* TAG = "hatch";

static const char* CHAR_TX = "02240002-5efd-47eb-9c1a-de53f7a2b232";
static const char* CHAR_FEEDBACK = "02260002-5efd-47eb-9c1a-de53f7a2b232";
static constexpr uint16_t HATCH_MANUFACTURER_ID = 1076;
static constexpr uint32_t SCAN_MS = 6000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 45000;  // gen 1 can take ~20 s
static constexpr uint32_t STATE_READ_DELAY_MS = 1500;  // color changes aren't notified

class Lock {
  SemaphoreHandle_t m_;

 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }
};

static HatchController* instance_s = nullptr;

void HatchController::Begin() {
  instance_s = this;
  lock_ = xSemaphoreCreateMutex();
  wake_ = xSemaphoreCreateBinary();
  NimBLEDevice::init("");
  xTaskCreatePinnedToCore(TaskEntry, "hatch", 8192, this, 1, &task_, 0);
}

void HatchController::Wake() {
  if (wake_) xSemaphoreGive(wake_);
}

void HatchController::WaitForWake(uint32_t ms) { xSemaphoreTake(wake_, pdMS_TO_TICKS(ms)); }

void HatchController::SetTarget(const String& address, uint32_t sound_delay_ms) {
  {
    Lock l(lock_);
    if (address != target_address_) {
      target_address_ = address;
      target_changed_ = true;
    }
    sound_delay_ms_ = sound_delay_ms;
  }
  Wake();
}

void HatchController::RequestColor(uint8_t r, uint8_t g, uint8_t b) {
  {
    Lock l(lock_);
    color_[0] = r;
    color_[1] = g;
    color_[2] = b;
    color_pending_ = true;
  }
  Wake();
}

void HatchController::RequestSound(uint8_t sound) {
  {
    Lock l(lock_);
    // Debounced: scrolling through sounds shouldn't play each one.
    sound_ = sound;
    sound_pending_ = true;
    sound_due_ms_ = millis() + sound_delay_ms_;
  }
  Wake();
}

static String StateToJson(const HatchState& s) {
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"power\":%s,\"hex\":\"#%02x%02x%02x\",\"r\":%u,\"g\":%u,\"b\":%u,"
           "\"brightness\":%u,\"sound\":%u,\"volume\":%u}",
           s.power ? "true" : "false", s.r, s.g, s.b, s.r, s.g, s.b, s.brightness, s.sound,
           s.volume);
  return String(buf);
}

bool HatchController::PopStateJson(String* json) {
  Lock l(lock_);
  if (!state_dirty_ || !state_.valid) return false;
  state_dirty_ = false;
  *json = StateToJson(state_);
  return true;
}

void HatchController::MarkStateDirty() {
  Lock l(lock_);
  state_dirty_ = true;
}

String HatchController::StatusJson() {
  Lock l(lock_);
  String s = "{\"connected\":";
  s += connected_ ? "true" : "false";
  s += ",\"target\":\"" + target_address_ + "\"";
  s += ",\"address\":\"" + String(connected_address_.c_str()) + "\"";
  s += ",\"name\":\"" + String(connected_name_.c_str()) + "\"";
  s += ",\"rssi\":" + String(rssi_);
  s += ",\"state\":" + (state_.valid ? StateToJson(state_) : String("null"));
  s += ",\"seen\":[";
  for (size_t i = 0; i < seen_.size(); i++) {
    if (i) s += ",";
    s += "\"" + String(seen_[i].c_str()) + "\"";
  }
  s += "]}";
  return s;
}

void HatchController::OnFeedback(const uint8_t* d, size_t len) {
  if (len < 15 || d[5] != 'C' || d[10] != 'S' || d[13] != 'P') return;
  HatchState s;
  s.valid = true;
  s.r = d[6];
  s.g = d[7];
  s.b = d[8];
  s.brightness = d[9];
  s.sound = d[11];
  s.volume = d[12];
  s.power = !(d[14] & 0xC0);
  Lock l(lock_);
  if (!(s == state_)) {
    state_ = s;
    state_dirty_ = true;
  }
}

void HatchController::TaskEntry(void* arg) { static_cast<HatchController*>(arg)->Run(); }

static bool IsHatch(const NimBLEAdvertisedDevice* dev) {
  if (!dev->haveManufacturerData()) return false;
  std::string m = dev->getManufacturerData();
  return m.size() >= 2 && (uint8_t)m[0] == (HATCH_MANUFACTURER_ID & 0xFF) &&
         (uint8_t)m[1] == (HATCH_MANUFACTURER_ID >> 8);
}

bool HatchController::Connect() {
  String target;
  {
    Lock l(lock_);
    target = target_address_;
    target_changed_ = false;
  }

  ESP_LOGI(TAG, "Scanning for %s", target.length() ? target.c_str() : "any Hatch");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(SCAN_MS, false);

  const NimBLEAdvertisedDevice* match = nullptr;
  std::vector<std::string> seen;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    if (!IsHatch(dev)) continue;
    std::string addr = dev->getAddress().toString();
    seen.push_back(addr + " " + dev->getName() + " " + std::to_string(dev->getRSSI()) + "dBm");
    bool wanted = target.length() ? (addr == std::string(target.c_str())) : true;
    if (wanted && (!match || dev->getRSSI() > match->getRSSI())) match = dev;
  }
  {
    Lock l(lock_);
    seen_ = seen;
  }
  if (!match) {
    ESP_LOGW(TAG, "Hatch not found (%u Hatches seen)", (unsigned)seen.size());
    scan->clearResults();
    return false;
  }

  NimBLEAddress addr = match->getAddress();
  std::string name = match->getName();
  int rssi = match->getRSSI();
  scan->clearResults();

  if (!client_) client_ = NimBLEDevice::createClient();
  client_->setConnectTimeout(CONNECT_TIMEOUT_MS);
  ESP_LOGI(TAG, "Connecting to %s (%s, %d dBm)", addr.toString().c_str(), name.c_str(), rssi);
  if (!client_->connect(addr)) {
    ESP_LOGW(TAG, "Connect failed");
    return false;
  }

  tx_ = fb_ = nullptr;
  for (NimBLERemoteService* svc : client_->getServices(true)) {
    if (!tx_) tx_ = svc->getCharacteristic(NimBLEUUID(CHAR_TX));
    if (!fb_) fb_ = svc->getCharacteristic(NimBLEUUID(CHAR_FEEDBACK));
  }
  if (!tx_ || !fb_) {
    ESP_LOGW(TAG, "Hatch characteristics not found");
    client_->disconnect();
    return false;
  }

  ReadState();
  if (fb_->canNotify()) {
    fb_->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
      if (instance_s) instance_s->OnFeedback(data, len);
    });
  }
  {
    Lock l(lock_);
    connected_ = true;
    connected_address_ = addr.toString();
    connected_name_ = name;
    rssi_ = rssi;
  }
  ESP_LOGI(TAG, "Connected to Hatch %s (%s)", addr.toString().c_str(), name.c_str());
  return true;
}

void HatchController::Disconnect() {
  if (client_ && client_->isConnected()) client_->disconnect();
  tx_ = fb_ = nullptr;
  Lock l(lock_);
  connected_ = false;
}

bool HatchController::Send(const char* cmd) {
  ESP_LOGD(TAG, "-> %s", cmd);
  return tx_ && tx_->writeValue((const uint8_t*)cmd, strlen(cmd), true);
}

void HatchController::ReadState() {
  if (!fb_) return;
  NimBLEAttValue v = fb_->readValue();
  OnFeedback(v.data(), v.size());
}

void HatchController::Service() {
  bool do_color = false, do_sound = false;
  uint8_t r = 0, g = 0, b = 0, brightness = 0x7F, sound = 0;
  {
    Lock l(lock_);
    if (color_pending_) {
      do_color = true;
      color_pending_ = false;
      r = color_[0];
      g = color_[1];
      b = color_[2];
      if (state_.valid) brightness = state_.brightness;
    }
    if (sound_pending_ && (int32_t)(millis() - sound_due_ms_) >= 0) {
      sound_pending_ = false;
      if (!state_.valid || state_.sound != sound_) {
        do_sound = true;
        sound = sound_;
      }
    }
  }
  char cmd[16];
  if (do_color) {
    snprintf(cmd, sizeof(cmd), "SC%02x%02x%02x%02x", r, g, b, brightness);
    if (Send(cmd)) read_due_ms_ = millis() + STATE_READ_DELAY_MS;
  }
  if (do_sound) {
    snprintf(cmd, sizeof(cmd), "SN%02x", sound);
    Send(cmd);
  }
}

void HatchController::Run() {
  uint32_t backoff_ms = 2000;
  for (;;) {
    bool retarget;
    {
      Lock l(lock_);
      retarget = target_changed_;
    }
    if (retarget) Disconnect();

    if (!client_ || !client_->isConnected() || !tx_) {
      if (tx_) {
        ESP_LOGW(TAG, "Hatch disconnected");
        Disconnect();
      }
      if (!Connect()) {
        Disconnect();
        WaitForWake(backoff_ms);  // a config change wakes us early
        backoff_ms = min<uint32_t>(backoff_ms * 2, 60000);
        continue;
      }
      backoff_ms = 2000;
    }

    Service();
    if (read_due_ms_ && (int32_t)(millis() - read_due_ms_) >= 0) {
      read_due_ms_ = 0;
      ReadState();
    }

    // Sleep until a request arrives or the next deadline (sound debounce / state read).
    uint32_t wait = 1000;
    {
      Lock l(lock_);
      if (color_pending_) wait = 0;
      if (sound_pending_) wait = min<uint32_t>(wait, max<int32_t>(0, sound_due_ms_ - millis()));
    }
    if (read_due_ms_) wait = min<uint32_t>(wait, max<int32_t>(0, read_due_ms_ - millis()));
    if (wait) WaitForWake(wait);
  }
}
