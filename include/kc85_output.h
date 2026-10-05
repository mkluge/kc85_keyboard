#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "kc85_keyboard.h"

// Owns the timing-sensitive transmitter in a separate task. Key transitions
// are kept in order so every accepted press reaches the KC85 at least once.
class Kc85Output {
 public:
  explicit Kc85Output(uint8_t dataPin) : keyboard_(dataPin) {}
  bool begin();
  void press(KcKey key, bool shifted);
  void release();
  // Discard queued input after a disconnect/error and stop the active key.
  void cancel();

 private:
  struct KeyState {
    bool pressed;
    KcKey key;
    bool shifted;
  };

  static void taskEntry(void *context);
  void run();

  Kc85Keyboard keyboard_;
  QueueHandle_t events_ = nullptr;
  KeyState submitted_{false, KcKey::Unused, false};
  bool canceled_ = false;
};
