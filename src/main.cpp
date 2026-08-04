#include <Arduino.h>

#include "kc85_keyboard.h"

constexpr uint8_t KCDATA_PIN = 1;

Kc85Keyboard kcKeyboard(KCDATA_PIN);

void setup()
{
  kcKeyboard.begin();

  // The hardware/input layer should call one of:
  //   kcKeyboard.pressKey(KcKey::A, shifted);
  //   kcKeyboard.pressIbus(0x01);
  //   kcKeyboard.pressIso7('a');
  // and call kcKeyboard.releaseKey() when the key is released.
}

void loop()
{
  kcKeyboard.service();
}
