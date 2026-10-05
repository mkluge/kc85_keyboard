#include "kc85_output.h"

#include <freertos/task.h>

namespace {
constexpr UBaseType_t kEventQueueLength = 32;
}

bool Kc85Output::begin() {
  events_ = xQueueCreate(kEventQueueLength, sizeof(KeyState));
  if (events_ == nullptr) {
    return false;
  }
  // Keep frame generation off the Arduino loop's core. BLE can continue to
  // publish input transitions while the current KC85 frame finishes.
  const BaseType_t outputCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
  if (xTaskCreatePinnedToCore(taskEntry, "kc85-output", 3072, this, 1,
                              nullptr, outputCore) != pdPASS) {
    vQueueDelete(events_);
    events_ = nullptr;
    return false;
  }
  return true;
}

void Kc85Output::press(KcKey key, bool shifted) {
  const KeyState state{true, key, shifted};
  if (submitted_.pressed && submitted_.key == key &&
      submitted_.shifted == shifted) {
    return;
  }
  // A full queue means the KC85 has fallen more than a second behind. Apply
  // backpressure instead of silently losing a key or its eventual release.
  xQueueSend(events_, &state, portMAX_DELAY);
  submitted_ = state;
  canceled_ = false;
}

void Kc85Output::release() {
  const KeyState state{false, KcKey::Unused, false};
  if (!submitted_.pressed) {
    return;
  }
  xQueueSend(events_, &state, portMAX_DELAY);
  submitted_ = state;
}

void Kc85Output::cancel() {
  if (canceled_) {
    return;
  }
  const KeyState state{false, KcKey::Unused, false};
  // FreeRTOS permits resetting a queue while its consumer task is active.
  // A frame already being transmitted still completes, then this release wins.
  xQueueReset(events_);
  xQueueSend(events_, &state, portMAX_DELAY);
  submitted_ = state;
  canceled_ = true;
}

void Kc85Output::taskEntry(void *context) {
  static_cast<Kc85Output *>(context)->run();
}

void Kc85Output::run() {
  keyboard_.begin();
  bool pressNeedsWord = false;
  for (;;) {
    KeyState state;
    // Do not consume the event following a press until that press has emitted
    // a complete word. In particular, a quick release must not cancel a press
    // while it is waiting for the mandatory inter-word gap.
    if (!pressNeedsWord && xQueueReceive(events_, &state, 0) == pdTRUE) {
      if (state.pressed) {
        keyboard_.pressKey(state.key, state.shifted);
        pressNeedsWord = true;
      } else {
        keyboard_.releaseKey();
      }
    }
    if (keyboard_.service()) {
      pressNeedsWord = false;
    }
    vTaskDelay(1);
  }
}
