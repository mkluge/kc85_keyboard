#pragma once

#include <Arduino.h>

// Matrix positions from table 1 of the KC85 keyboard documentation.
enum class KcKey : uint8_t
{
  W = 0,
  A,
  Digit2,
  CursorLeft,
  Home,
  Minus,
  F2,
  Y,
  E,
  S,
  Digit3,
  Neg,
  Clear,
  Colon,
  F3,
  X,
  T,
  F,
  Digit5,
  P,
  Delete,
  Digit0,
  F5,
  V,
  U,
  H,
  Digit7,
  O,
  Insert,
  Digit9,
  Break,
  N,
  I,
  J,
  Digit8,
  Space,
  K,
  Comma,
  Stop,
  M,
  Z,
  G,
  Digit6,
  Unused,
  L,
  Period,
  F6,
  B,
  R,
  D,
  Digit4,
  Underscore,
  Plus,
  Slash,
  F4,
  C,
  Q,
  ShiftLock,
  Digit1,
  CursorDown,
  CursorUp,
  CursorRight,
  F1,
  Enter,
};

class Kc85Keyboard
{
public:
  static constexpr uint32_t ZeroSpacingUs = 5120;
  static constexpr uint32_t OneSpacingUs = 7168;
  static constexpr uint32_t WordSpacingUs = 14336;
  static constexpr uint32_t DoubleWordSpacingUs = 19456;

  explicit Kc85Keyboard(uint8_t dataPin);

  void begin();

  // Call service() continuously from loop(). Transmission of one word is
  // synchronous, because its seven burst spacings are timing-critical.
  void service();

  // SHIFT is an electrical plane selector in the original keyboard, not a
  // separately transmitted matrix key. ShiftLock is a normal matrix key.
  void setShiftPlane(bool shifted);
  bool pressKey(KcKey key);
  bool pressKey(KcKey key, bool shifted);
  bool pressIbus(uint8_t ibusCode);
  bool pressIso7(uint8_t iso7Code);
  void releaseKey();

  bool isPressed() const;
  bool isShifted() const;

  // Documented IBUS notation uses bit 7 for the shift plane and bits 0..5
  // for the matrix position. Bit 6 is unused.
  static uint8_t ibusForKey(KcKey key, bool shifted);
  static bool keyForIso7(uint8_t iso7Code, KcKey &key, bool &shifted);

private:
  static bool deadlineReached(uint32_t now, uint32_t deadline);
  static uint8_t wireWordForIbus(uint8_t ibusCode);

  void selectKey(KcKey key, bool shifted);
  void scheduleChangedWord();
  uint32_t sendFrame(uint8_t ibusCode);
  void sendBurst();
  static void waitUntil(uint32_t deadline);

  uint8_t dataPin_;
  KcKey activeKey_ = KcKey::Unused;
  bool pressed_ = false;
  bool shiftPlane_ = false;
  bool haveBoundary_ = false;
  uint32_t lastBoundaryAt_ = 0;
  uint32_t nextFrameAt_ = 0;
};
