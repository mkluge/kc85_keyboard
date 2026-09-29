#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "hid_keyboard.h"

// Parses the input fields in a HID Report Map and converts keyboard reports
// into the boot-keyboard-shaped structure used by the rest of the firmware.
// The decoder supports both key-code arrays and one-bit-per-key (NKRO) fields.
class HidReportMap
{
public:
  bool parse(const uint8_t *data, size_t length);
  bool hasKeyboardInput(uint8_t reportId) const;
  bool decodeKeyboardInput(uint8_t reportId, const uint8_t *data,
                           size_t length, HidKeyboardReport &report) const;

private:
  struct InputField
  {
    uint8_t reportId;
    uint16_t bitOffset;
    uint8_t bitSize;
    uint16_t count;
    bool variable;
    uint16_t usageMinimum;
    std::vector<uint16_t> usages;
  };

  static uint32_t readBits(const uint8_t *data, size_t length,
                           uint32_t bitOffset, uint8_t bitSize);
  static uint32_t unsignedValue(const uint8_t *data, uint8_t size);
  static uint16_t usageForIndex(const InputField &field, uint16_t index);

  std::vector<InputField> keyboardFields_;
};
