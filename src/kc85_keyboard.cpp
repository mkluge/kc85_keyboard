#include "kc85_keyboard.h"

namespace
{
struct Iso7Pair
{
  uint8_t base;
  uint8_t shifted;
};

// ISO-7 translation table from table 1. The DEL entry is 1F as confirmed by
// the KC85 system handbook; the scan of the keyboard document drops the F.
constexpr Iso7Pair Iso7ByMatrixPosition[64] = {
    {0x57, 0x77}, {0x41, 0x61}, {0x32, 0x22}, {0x08, 0x19},
    {0x10, 0x0C}, {0x2D, 0x3D}, {0xF2, 0xF8}, {0x59, 0x79},
    {0x45, 0x65}, {0x53, 0x73}, {0x33, 0x23}, {0x5E, 0x5D},
    {0x01, 0x0F}, {0x3A, 0x2A}, {0xF3, 0xF9}, {0x58, 0x78},
    {0x54, 0x74}, {0x46, 0x66}, {0x35, 0x25}, {0x50, 0x70},
    {0x1F, 0x02}, {0x30, 0x40}, {0xF5, 0xFB}, {0x56, 0x76},
    {0x55, 0x75}, {0x48, 0x68}, {0x37, 0x27}, {0x4F, 0x6F},
    {0x1A, 0x1A}, {0x39, 0x29}, {0x03, 0x03}, {0x4E, 0x6E},
    {0x49, 0x69}, {0x4A, 0x6A}, {0x38, 0x28}, {0x20, 0x5B},
    {0x4B, 0x6B}, {0x2C, 0x3C}, {0x13, 0x13}, {0x4D, 0x6D},
    {0x5A, 0x7A}, {0x47, 0x67}, {0x36, 0x26}, {0x00, 0x00},
    {0x4C, 0x6C}, {0x2E, 0x3E}, {0xF6, 0xFC}, {0x42, 0x62},
    {0x52, 0x72}, {0x44, 0x64}, {0x34, 0x24}, {0x5F, 0x5C},
    {0x2B, 0x3B}, {0x2F, 0x3F}, {0xF4, 0xFA}, {0x43, 0x63},
    {0x51, 0x71}, {0x16, 0x16}, {0x31, 0x21}, {0x0A, 0x12},
    {0x0B, 0x11}, {0x09, 0x1E}, {0xF1, 0xF7}, {0x0D, 0x0D},
};
} // namespace

Kc85Keyboard::Kc85Keyboard(uint8_t dataPin) : dataPin_(dataPin) {}

void Kc85Keyboard::begin()
{
  digitalWrite(dataPin_, HIGH);
  pinMode(dataPin_, OUTPUT);
}

void Kc85Keyboard::service()
{
  if (!pressed_ || !deadlineReached(micros(), nextFrameAt_))
  {
    return;
  }

  const uint8_t ibusCode = ibusForKey(activeKey_, shiftPlane_);
  lastBoundaryAt_ = sendFrame(ibusCode);
  haveBoundary_ = true;
  nextFrameAt_ = lastBoundaryAt_ + WordSpacingUs;
}

void Kc85Keyboard::setShiftPlane(bool shifted)
{
  if (shiftPlane_ == shifted)
  {
    return;
  }

  shiftPlane_ = shifted;
  if (pressed_)
  {
    scheduleChangedWord();
  }
}

bool Kc85Keyboard::pressKey(KcKey key)
{
  return pressKey(key, shiftPlane_);
}

bool Kc85Keyboard::pressKey(KcKey key, bool shifted)
{
  const uint8_t position = static_cast<uint8_t>(key);
  if (position >= 64 || key == KcKey::Unused)
  {
    return false;
  }

  selectKey(key, shifted);
  return true;
}

bool Kc85Keyboard::pressIbus(uint8_t ibusCode)
{
  if ((ibusCode & 0x40U) != 0)
  {
    return false;
  }

  const KcKey key = static_cast<KcKey>(ibusCode & 0x3FU);
  return pressKey(key, (ibusCode & 0x80U) != 0);
}

bool Kc85Keyboard::pressIso7(uint8_t iso7Code)
{
  KcKey key;
  bool shifted;
  if (!keyForIso7(iso7Code, key, shifted))
  {
    return false;
  }

  return pressKey(key, shifted);
}

void Kc85Keyboard::releaseKey()
{
  pressed_ = false;
  activeKey_ = KcKey::Unused;
}

bool Kc85Keyboard::isPressed() const
{
  return pressed_;
}

bool Kc85Keyboard::isShifted() const
{
  return shiftPlane_;
}

uint8_t Kc85Keyboard::ibusForKey(KcKey key, bool shifted)
{
  return static_cast<uint8_t>(static_cast<uint8_t>(key) |
                              (shifted ? 0x80U : 0x00U));
}

bool Kc85Keyboard::keyForIso7(uint8_t iso7Code, KcKey &key, bool &shifted)
{
  for (uint8_t position = 0; position < 64; ++position)
  {
    if (position == static_cast<uint8_t>(KcKey::Unused))
    {
      continue;
    }

    if (Iso7ByMatrixPosition[position].base == iso7Code)
    {
      key = static_cast<KcKey>(position);
      shifted = false;
      return true;
    }
    if (Iso7ByMatrixPosition[position].shifted == iso7Code)
    {
      key = static_cast<KcKey>(position);
      shifted = true;
      return true;
    }
  }

  return false;
}

bool Kc85Keyboard::deadlineReached(uint32_t now, uint32_t deadline)
{
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint8_t Kc85Keyboard::wireWordForIbus(uint8_t ibusCode)
{
  // The U807 transmits the plane/start bit first, followed by the six matrix
  // bits. Packing it into bit 0 lets sendFrame emit the wire order LSB-first.
  return static_cast<uint8_t>(((ibusCode & 0x3FU) << 1U) |
                              ((ibusCode & 0x80U) >> 7U));
}

void Kc85Keyboard::selectKey(KcKey key, bool shifted)
{
  if (pressed_ && activeKey_ == key && shiftPlane_ == shifted)
  {
    return;
  }

  activeKey_ = key;
  shiftPlane_ = shifted;
  pressed_ = true;
  scheduleChangedWord();
}

void Kc85Keyboard::scheduleChangedWord()
{
  const uint32_t now = micros();
  if (!haveBoundary_)
  {
    nextFrameAt_ = now;
    return;
  }

  const uint32_t earliest = lastBoundaryAt_ + DoubleWordSpacingUs;
  nextFrameAt_ = deadlineReached(now, earliest) ? now : earliest;
}

uint32_t Kc85Keyboard::sendFrame(uint8_t ibusCode)
{
  const uint8_t wireWord = wireWordForIbus(ibusCode);
  for (uint8_t bit = 0; bit < 7; ++bit)
  {
    const uint32_t burstAt = micros();
    sendBurst();
    const uint32_t spacing = (wireWord & (1U << bit)) != 0
                                 ? OneSpacingUs
                                 : ZeroSpacingUs;
    waitUntil(burstAt + spacing);
  }

  const uint32_t boundaryAt = micros();
  sendBurst();
  return boundaryAt;
}

void Kc85Keyboard::sendBurst()
{
  // Five low pulses consist of nine 16 us half-periods: L-H-L-H-L-H-L-H-L.
  // The final transition to HIGH ends the documented 144 us burst.
  for (uint8_t pulse = 0; pulse < 5; ++pulse)
  {
    digitalWrite(dataPin_, LOW);
    delayMicroseconds(16);
    digitalWrite(dataPin_, HIGH);
    if (pulse < 4)
    {
      delayMicroseconds(16);
    }
  }
}

void Kc85Keyboard::waitUntil(uint32_t deadline)
{
  while (!deadlineReached(micros(), deadline))
  {
    const uint32_t remaining = deadline - micros();
    if (remaining > 32)
    {
      delayMicroseconds(remaining - 16);
    }
  }
}
