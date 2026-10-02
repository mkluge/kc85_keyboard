#include "kc85_output.h"

#include <freertos/task.h>

bool Kc85Output::begin() {
  mailbox_ = xQueueCreate(1, sizeof(KeyState));
  if (mailbox_ == nullptr) {
    return false;
  }
  // Keep frame generation off the Arduino loop's core. BLE can consume and
  // publish the latest input state while the current KC85 frame finishes.
  const BaseType_t outputCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
  if (xTaskCreatePinnedToCore(taskEntry, "kc85-output", 3072, this, 1,
                              nullptr, outputCore) != pdPASS) {
    vQueueDelete(mailbox_);
    mailbox_ = nullptr;
    return false;
  }
  return true;
}

void Kc85Output::press(KcKey key, bool shifted) {
  const KeyState state{true, key, shifted};
  xQueueOverwrite(mailbox_, &state);
}

void Kc85Output::release() {
  const KeyState state{false, KcKey::Unused, false};
  xQueueOverwrite(mailbox_, &state);
}

void Kc85Output::taskEntry(void *context) {
  static_cast<Kc85Output *>(context)->run();
}

void Kc85Output::run() {
  keyboard_.begin();
  for (;;) {
    KeyState state;
    if (xQueueReceive(mailbox_, &state, 0) == pdTRUE) {
      if (state.pressed) {
        keyboard_.pressKey(state.key, state.shifted);
      } else {
        keyboard_.releaseKey();
      }
    }
    // Finish a frame already on the wire, then consume the newest key or
    // release before beginning another. The mailbox holds only one state.
    keyboard_.service();
    vTaskDelay(1);
  }
}
