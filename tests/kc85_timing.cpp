#include <cassert>
#include <cstdint>
#include <iostream>

#include "kc85_keyboard.h"
#include "hid_keyboard.h"

namespace {
uint32_t clockUs = 0;
uint32_t clockStep = 1;
uint32_t clockJitter = 0;
unsigned pulseCount = 0;

void checkTranslation(uint8_t usage, bool hidShift, uint8_t expectedIso7,
                      uint8_t expectedIbus) {
  uint8_t iso7;
  KcKey key;
  bool kcShift;
  assert(hidUsageToIso7(usage, hidShift, iso7));
  assert(iso7 == expectedIso7);
  assert(Kc85Keyboard::keyForIso7(iso7, key, kcShift));
  assert(Kc85Keyboard::ibusForKey(key, kcShift) == expectedIbus);
  Kc85Keyboard keyboard(18);
  assert(keyboard.pressIbus(expectedIbus));
  assert(keyboard.isShifted() == kcShift);
}

void checkFrames(uint32_t start, uint32_t step) {
  clockUs = start;
  clockStep = step;
  clockJitter = 0;
  pulseCount = 0;
  Kc85Keyboard keyboard(18);
  keyboard.begin();
  for (unsigned frame = 0; frame < 256; ++frame) {
    // Cover both shift planes and all used matrix positions.
    const auto key = static_cast<KcKey>(frame % 64);
    if (key == KcKey::Unused) {
      continue;
    }
    assert(keyboard.pressKey(key, (frame & 64) != 0));
    unsigned polls = 0;
    while (!keyboard.service()) {
      assert(++polls < 100000);
    }
    keyboard.releaseKey();
    assert(!keyboard.service());
  }
  // 252 frames, eight bursts per frame, five pulses per burst.
  assert(pulseCount == 252 * 8 * 5);
}
}  // namespace

uint32_t micros() {
  // Deterministic jitter models interrupts between consecutive timer reads.
  clockJitter = clockJitter * 1664525U + 1013904223U;
  clockUs += 1 + clockJitter % clockStep;
  return clockUs;
}

void delayMicroseconds(uint32_t us) {
  // No legitimate bit interval needs a delay greater than OneSpacingUs.
  // The old two-read deadline race instead requests roughly UINT32_MAX us.
  assert(us <= Kc85Keyboard::OneSpacingUs);
  clockUs += us;
}

void digitalWrite(uint8_t, uint8_t level) {
  if (level == HIGH) {
    ++pulseCount;
  }
}

void pinMode(uint8_t, uint8_t) {}

int main() {
  checkTranslation(0x04, false, 'A', 0x81);
  checkTranslation(0x04, true, 'a', 0x01);
  checkTranslation(0x1E, false, '1', 0xBA);
  checkTranslation(0x1E, true, '!', 0x3A);
  checkTranslation(0x1F, false, '2', 0x82);
  checkTranslation(0x2C, false, ' ', 0xA3);
  checkTranslation(0x2C, true, ' ', 0xA3);
  for (uint32_t step : {1U, 2U, 7U, 17U}) {
    checkFrames(0, step);
    checkFrames(UINT32_MAX - 10000, step);
  }
  std::cout << "KC85 timing checks passed\n";
}
