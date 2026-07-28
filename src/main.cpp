#include <Arduino.h>

constexpr uint8_t KCDATA_PIN = 1;

void put_kc(uint8_t y);
void send_kc(uint8_t i);
void bith(void);
void bitl(void);
void burst(void); // Burst, ist beim Original so, einfaches Low tut es aber auch

// -----Senderoutine zum KC-----
void put_kc(uint8_t y)
{
  send_kc(y);
  delay(12);
  send_kc(y);
} // send twice
// Byte Ausgabe
void send_kc(uint8_t i) // send
{
  uint8_t z; // Bitzähler
  for (z = 0; z < 7; z++)
  {
    (i & (1 << z)) ? bith() : bitl();
  } // Einzelbits auswerten
  burst();
}

void bith(void)
{
  burst();
  delay(7);
} // Hi Impulsabstand
void bitl(void)
{
  burst();
  delay(4);
} // Low Impulsabstand

void burst(void) // Burst, ist beim Original so, einfaches Low tut es aber auch
{
  uint8_t t = 5; // ca 140 uS
  do
  {
    digitalWrite(KCDATA_PIN, LOW);
    delayMicroseconds(14);
    digitalWrite(KCDATA_PIN, HIGH);
    delayMicroseconds(14);
    t--;
  } while (t);
}

void setup()
{
  pinMode(KCDATA_PIN, OUTPUT);
}

void loop()
{
  send_kc(85);
  delay(500);
}