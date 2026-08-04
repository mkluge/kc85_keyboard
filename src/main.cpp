#include <Arduino.h>

#include "kc85_keyboard.h"

// GPIO connected to the input of the external inverting output transistor.
constexpr uint8_t KCDATA_PIN = 1;

// Time between the scheduled starts of complete test-key sequences.
constexpr uint32_t TEST_INTERVAL_MS = 5000;

// Time each synthetic key remains logically pressed before it is released.
constexpr uint32_t TEST_KEY_HOLD_MS = 100;

// Released-key pause inserted between two characters in the test sequence.
constexpr uint32_t TEST_KEY_GAP_MS = 50;

// ISO-7 values sent by the test generator: "KC85 TEST" followed by ENTER.
constexpr uint8_t TEST_SEQUENCE[] = {
    'K', 'C', '8', '5', ' ', 'T', 'E', 'S', 'T', 0x0D,
};

// Keyboard protocol emulator that drives KCDATA_PIN.
Kc85Keyboard kcKeyboard(KCDATA_PIN);

enum class TestState : uint8_t
{
  // No test key is active; wait for the next five-second sequence deadline.
  Waiting,

  // Current test key is held so the emulator may emit repeat frames.
  KeyPressed,

  // Current key was released; wait before pressing the next sequence key.
  KeyGap,
};

// Current phase of the automatic test-sequence state machine.
TestState testState = TestState::Waiting;

// Index of the current ISO-7 value within TEST_SEQUENCE.
uint8_t testPosition = 0;

// millis() deadline for starting the next complete test sequence.
uint32_t nextTestSequenceAt = 0;

// millis() deadline for releasing or advancing the current test key.
uint32_t nextTestActionAt = 0;

// now is the current millis() value; deadline is the scheduled action time.
bool timeReached(uint32_t now, uint32_t deadline)
{
  return static_cast<int32_t>(now - deadline) >= 0;
}

// now is reused as the time origin for this key's hold interval.
void pressCurrentTestKey(uint32_t now)
{
  if (kcKeyboard.pressIso7(TEST_SEQUENCE[testPosition]))
  {
    testState = TestState::KeyPressed;
    nextTestActionAt = now + TEST_KEY_HOLD_MS;
  }
}

void setup()
{
  kcKeyboard.begin();
  nextTestSequenceAt = millis() + TEST_INTERVAL_MS;
}

void loop()
{
  kcKeyboard.service();

  // Current millisecond timestamp used for all test-state decisions this loop.
  const uint32_t now = millis();
  switch (testState)
  {
  case TestState::Waiting:
    if (timeReached(now, nextTestSequenceAt))
    {
      // Keep sequence starts on a stable five-second cadence.
      do
      {
        nextTestSequenceAt += TEST_INTERVAL_MS;
      } while (timeReached(now, nextTestSequenceAt));

      testPosition = 0;
      pressCurrentTestKey(now);
    }
    break;

  case TestState::KeyPressed:
    if (timeReached(now, nextTestActionAt))
    {
      kcKeyboard.releaseKey();
      testState = TestState::KeyGap;
      nextTestActionAt = now + TEST_KEY_GAP_MS;
    }
    break;

  case TestState::KeyGap:
    if (timeReached(now, nextTestActionAt))
    {
      ++testPosition;
      if (testPosition < sizeof(TEST_SEQUENCE))
      {
        pressCurrentTestKey(now);
      }
      else
      {
        testState = TestState::Waiting;
      }
    }
    break;
  }
}
