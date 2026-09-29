# Bluetooth-to-KC85 keyboard adapter

This firmware turns an Adafruit QT Py ESP32-S3 into a Bluetooth Low Energy
keyboard adapter for the KC85. It connects to a Logitech MX Keys Mini, flashes
the onboard NeoPixel white for 100 ms on key-down reports, translates standard
HID keyboard usages to KC85 keys, and sends the original KC85 pulse protocol
through the QT Py pin labeled A0 (ESP32-S3 GPIO 18).

## Hardware

The QT Py pin labeled A0 (ESP32-S3 GPIO 18) drives the input of the external
inverting transistor used by the KC85 keyboard interface. The KC85 protocol
output driver expects that inversion and keeps the GPIO low while idle.

## Pairing

1. Upload with `pio run --target upload`.
2. Open the 115200-baud serial monitor with `pio device monitor`.
3. Hold one of the three Easy-Switch keys until its LED blinks rapidly.
4. When the serial monitor prints a six-digit pairing code, type it on the
   MX Keys Mini and press Enter.
5. Wait for `Ready; listening to ...` in the serial monitor.

The ESP32 retains the bond and recognizes the keyboard by its bonded BLE
address on later reconnections, even when it no longer advertises its name.

## Key translation

The adapter reads the keyboard's HID Report Map and supports both key-code
arrays and one-bit-per-key (NKRO) input reports. It uses the USB HID US logical
layout. Letters, digits, KC85-supported punctuation, Enter, Space, Backspace,
Tab, Caps Lock, F1-F12, Insert, Home, Delete, arrow keys, Escape/Pause (BREAK),
and keypad digits/operators are translated.
Left/Right Shift select uppercase letters and shifted symbols. Ctrl, Alt,
GUI, media keys, and characters unavailable on the KC85 are not forwarded.
When several normal keys are held, the first supported key in the report is
sent because the KC85 protocol represents one active key at a time.
