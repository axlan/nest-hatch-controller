// Hatch Rest (gen 1) BLE controller.
//
// Runs its own FreeRTOS task holding one persistent NimBLE connection (a gen 1
// Hatch can take ~20 s to accept a connection, which must not stall MQTT).
// Only color and sound are ever changed: no power, brightness or volume
// commands are sent. Color is written with the Hatch's current brightness.
//
// Protocol (same as pyhatchbabyrest):
//   write ASCII to 02240002-5efd-47eb-9c1a-de53f7a2b232
//     SCrrggbbLL  color + brightness (hex)    SNxx  sound id (hex)
//   read/notify 02260002-5efd-47eb-9c1a-de53f7a2b232
//     [5]='C' [6..9]=r,g,b,brightness [10]='S' [11]=sound [12]=volume
//     [13]='P' [14]=flags (power off when flags & 0xC0)
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>
#include <vector>

class NimBLEClient;
class NimBLERemoteCharacteristic;

struct HatchState {
  bool valid = false;
  uint8_t r = 0, g = 0, b = 0, brightness = 0, sound = 0, volume = 0;
  bool power = false;

  bool operator==(const HatchState& o) const {
    return valid == o.valid && r == o.r && g == o.g && b == o.b && brightness == o.brightness &&
           sound == o.sound && volume == o.volume && power == o.power;
  }
};

class HatchController {
 public:
  void Begin();
  // Thread-safe setters, callable from the Arduino loop.
  void SetTarget(const String& address, uint32_t sound_delay_ms);
  void RequestColor(uint8_t r, uint8_t g, uint8_t b);
  void RequestSound(uint8_t sound);

  // Returns true (and the state as JSON) when the Hatch state changed since
  // the last call.
  bool PopStateJson(String* json);
  void MarkStateDirty();
  // Connection details for GET_STATUS.
  String StatusJson();

 private:
  static void TaskEntry(void* arg);
  void Run();
  bool Connect();
  void Disconnect();
  void Service();
  bool Send(const char* cmd);
  void ReadState();
  void OnFeedback(const uint8_t* data, size_t len);

  void Wake();
  void WaitForWake(uint32_t ms);

  SemaphoreHandle_t lock_ = nullptr;
  // Our own wake-up signal. Task notifications must NOT be used for this:
  // NimBLE blocks the calling task on its notification value during
  // scans/connects/writes, and an extra notify releases it early (crash).
  SemaphoreHandle_t wake_ = nullptr;
  TaskHandle_t task_ = nullptr;

  // Guarded by lock_
  String target_address_;
  uint32_t sound_delay_ms_ = 600;
  bool target_changed_ = false;
  bool color_pending_ = false;
  uint8_t color_[3] = {0, 0, 0};
  bool sound_pending_ = false;
  uint8_t sound_ = 0;
  uint32_t sound_due_ms_ = 0;
  HatchState state_;
  bool state_dirty_ = false;
  bool connected_ = false;
  std::string connected_address_;
  std::string connected_name_;
  int rssi_ = 0;
  std::vector<std::string> seen_;  // Hatches seen in the last scan

  // Task-only
  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* tx_ = nullptr;
  NimBLERemoteCharacteristic* fb_ = nullptr;
  uint32_t read_due_ms_ = 0;
};
