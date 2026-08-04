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
  // Burst-start spacing, in microseconds, that represents a logical zero.
  static constexpr uint32_t ZeroSpacingUs = 5120;

  // Burst-start spacing, in microseconds, that represents a logical one.
  static constexpr uint32_t OneSpacingUs = 7168;

  // Boundary-to-next-word spacing used while the same key remains pressed.
  static constexpr uint32_t WordSpacingUs = 14336;

  // Boundary-to-next-word spacing used after a key or shift-plane change.
  static constexpr uint32_t DoubleWordSpacingUs = 19456;

  // dataPin is the GPIO connected to the input of the inverting transistor.
  explicit Kc85Keyboard(uint8_t dataPin);

  void begin();

  // Call service() continuously from loop(). Transmission of one word is
  // synchronous, because its seven burst spacings are timing-critical.
  void service();

  // SHIFT is an electrical plane selector in the original keyboard, not a
  // separately transmitted matrix key. ShiftLock is a normal matrix key.
  // shifted selects false for the base plane or true for the SHIFT plane.
  void setShiftPlane(bool shifted);

  // key is the matrix position to press using the current shift plane.
  bool pressKey(KcKey key);

  // key is the matrix position; shifted explicitly selects its output plane.
  bool pressKey(KcKey key, bool shifted);

  // ibusCode is the documented matrix code, including shift in bit 7.
  bool pressIbus(uint8_t ibusCode);

  // iso7Code is looked up in the base and shifted translation table.
  bool pressIso7(uint8_t iso7Code);
  void releaseKey();

  bool isPressed() const;
  bool isShifted() const;

  // Documented IBUS notation uses bit 7 for the shift plane and bits 0..5
  // for the matrix position. Bit 6 is unused.
  // key and shifted are converted into that documented notation.
  static uint8_t ibusForKey(KcKey key, bool shifted);

  // iso7Code is searched in the table; key and shifted receive the result.
  static bool keyForIso7(uint8_t iso7Code, KcKey &key, bool &shifted);

private:
  // now is the current timer value; deadline is the scheduled timer value.
  static bool deadlineReached(uint32_t now, uint32_t deadline);

  // ibusCode is rearranged from documented notation into transmission order.
  static uint8_t wireWordForIbus(uint8_t ibusCode);

  // key and shifted replace the currently selected matrix key and plane.
  void selectKey(KcKey key, bool shifted);
  void scheduleChangedWord();

  // ibusCode is sent as one complete seven-bit frame.
  uint32_t sendFrame(uint8_t ibusCode);
  void sendBurst();

  // deadline is the absolute micros() value at which transmission continues.
  static void waitUntil(uint32_t deadline);

  // GPIO that drives the base/input of the external inverting transistor.
  uint8_t dataPin_;

  // Matrix position currently being transmitted, or Unused while released.
  KcKey activeKey_ = KcKey::Unused;

  // True while a logical key is held and frames must continue repeating.
  bool pressed_ = false;

  // False selects the base key plane; true selects the SHIFT key plane.
  bool shiftPlane_ = false;

  // Records whether a previous frame boundary exists for gap calculation.
  bool haveBoundary_ = false;

  // micros() timestamp at which the most recent terminal burst began.
  uint32_t lastBoundaryAt_ = 0;

  // Earliest micros() timestamp at which the next frame may begin.
  uint32_t nextFrameAt_ = 0;
};
