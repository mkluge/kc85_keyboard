#include <Arduino.h>

constexpr uint8_t GPIO1_PIN = 1;
constexpr unsigned long BLINK_DELAY_MS = 500;

void blinkGpio1();

void setup() {
  pinMode(GPIO1_PIN, OUTPUT);
}

void loop() {
  blinkGpio1();
}

void blinkGpio1() {
  digitalWrite(GPIO1_PIN, HIGH);
  delay(BLINK_DELAY_MS);
  digitalWrite(GPIO1_PIN, LOW);
  delay(BLINK_DELAY_MS);
}