#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "kc85_keyboard.h"

// Owns the timing-sensitive transmitter in a separate task. Calls from the
// BLE owner replace the pending key state, so obsolete presses cannot pile up.
class Kc85Output {
 public:
  explicit Kc85Output(uint8_t dataPin) : keyboard_(dataPin) {}
  bool begin();
  void press(KcKey key, bool shifted);
  void release();

 private:
  struct KeyState {
    bool pressed;
    KcKey key;
    bool shifted;
  };

  static void taskEntry(void *context);
  void run();

  Kc85Keyboard keyboard_;
  QueueHandle_t mailbox_ = nullptr;
};
