#include "hid_keyboard.h"

#include <algorithm>
#include <iterator>

namespace
{
constexpr char UnshiftedDigits[] = "1234567890";
constexpr char ShiftedDigits[] = "!@#$%^&*()";

struct PrintablePair
{
  uint8_t usage;
  uint8_t unshifted;
  uint8_t shifted;
};

// USB HID usages follow the standard US logical layout. Characters that the
// KC85 cannot produce (for example braces, pipe, grave and tilde) are omitted.
constexpr PrintablePair PrintableKeys[] = {
    {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', 0},
    {0x30, ']', 0},   {0x31, '\\', 0},  {0x33, ';', ':'},
    {0x34, '\'', '"'}, {0x36, ',', '<'},  {0x37, '.', '>'},
    {0x38, '/', '?'}, {0x64, '\\', 0},
};
} // namespace

void HidKeyboardTransitions::reset()
{
  previous_ = HidKeyboardReport{};
  activeUsage_ = 0;
}

size_t HidKeyboardTransitions::update(
    const HidKeyboardReport &report, HidKeyboardEvent (&events)[MaxEvents])
{
  size_t count = 0;
  if (activeUsage_ != 0 &&
      std::find(std::begin(report.keys), std::end(report.keys), activeUsage_) ==
          std::end(report.keys))
  {
    events[count++] = {0, 0};
    activeUsage_ = 0;
  }

  const bool shifted = (report.modifiers & 0x22) != 0;
  for (size_t index = 0; index < sizeof(report.keys); ++index)
  {
    const uint8_t usage = report.keys[index];
    uint8_t iso7;
    if (usage == 0 ||
        std::find(std::begin(previous_.keys), std::end(previous_.keys), usage) !=
            std::end(previous_.keys) ||
        std::find(report.keys, report.keys + index, usage) !=
            report.keys + index ||
        !hidUsageToIso7(usage, shifted, iso7))
    {
      continue;
    }
    events[count++] = {usage, iso7};
    activeUsage_ = usage;
  }
  previous_ = report;
  return count;
}

void mergeKeyboardReport(HidKeyboardReport &aggregate,
                         const HidKeyboardReport &report)
{
  aggregate.modifiers |= report.modifiers;
  for (uint8_t usage : report.keys)
  {
    if (usage == 0 ||
        std::find(std::begin(aggregate.keys), std::end(aggregate.keys), usage) !=
            std::end(aggregate.keys))
    {
      continue;
    }
    auto empty = std::find(std::begin(aggregate.keys), std::end(aggregate.keys), 0);
    if (empty != std::end(aggregate.keys))
    {
      *empty = usage;
    }
  }
}

bool hidUsageToIso7(uint8_t usage, bool shifted, uint8_t &iso7Code)
{
  // Match the KC85 keyboard: uppercase on the base plane, lowercase on SHIFT.
  if (usage >= 0x04 && usage <= 0x1D)
  {
    iso7Code = static_cast<uint8_t>((shifted ? 'a' : 'A') + usage - 0x04);
    return true;
  }

  // Keyboard 1/! through 0/).
  if (usage >= 0x1E && usage <= 0x27)
  {
    const uint8_t index = usage - 0x1E;
    iso7Code = static_cast<uint8_t>(shifted ? ShiftedDigits[index]
                                            : UnshiftedDigits[index]);
    return true;
  }

  for (const PrintablePair &key : PrintableKeys)
  {
    if (key.usage == usage)
    {
      iso7Code = shifted ? key.shifted : key.unshifted;
      return iso7Code != 0;
    }
  }

  switch (usage)
  {
  case 0x28: // Enter
    iso7Code = 0x0D;
    return true;
  case 0x29: // Escape -> BREAK
  case 0x48: // Pause -> BREAK
    iso7Code = 0x03;
    return true;
  case 0x2A: // Backspace -> cursor left
  case 0x50: // Cursor left
    iso7Code = 0x08;
    return true;
  case 0x2B: // Tab -> cursor right
  case 0x4F: // Cursor right
    iso7Code = 0x09;
    return true;
  case 0x2C: // Space
    iso7Code = 0x20;
    return true;
  case 0x39: // Caps Lock -> SHIFT LOCK
    iso7Code = 0x16;
    return true;
  case 0x49: // Insert
    iso7Code = 0x1A;
    return true;
  case 0x4A: // Home
    iso7Code = 0x10;
    return true;
  case 0x4C: // Delete
    iso7Code = 0x1F;
    return true;
  case 0x51: // Cursor down
    iso7Code = 0x0A;
    return true;
  case 0x52: // Cursor up
    iso7Code = 0x0B;
    return true;
  case 0x54: // Keypad /
    iso7Code = '/';
    return true;
  case 0x55: // Keypad *
    iso7Code = '*';
    return true;
  case 0x56: // Keypad -
    iso7Code = '-';
    return true;
  case 0x57: // Keypad +
    iso7Code = '+';
    return true;
  case 0x58: // Keypad Enter
    iso7Code = 0x0D;
    return true;
  case 0x63: // Keypad .
    iso7Code = '.';
    return true;
  default:
    break;
  }

  // F1 through F12 use the KC85's base and shifted function-key planes.
  if (usage >= 0x3A && usage <= 0x45)
  {
    iso7Code = static_cast<uint8_t>(0xF1 + usage - 0x3A);
    return true;
  }

  // Keypad 1 through 9 and 0.
  if (usage >= 0x59 && usage <= 0x61)
  {
    iso7Code = static_cast<uint8_t>('1' + usage - 0x59);
    return true;
  }
  if (usage == 0x62)
  {
    iso7Code = '0';
    return true;
  }

  return false;
}
