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
5. Wait for `[STATUS] connected; listening to ...` in the serial monitor.

Pairing requires authenticated Secure Connections with MITM protection and
a maximum encryption key size of 16 bytes. The host displays the passkey;
passkey-entry requests and numeric-comparison requests are rejected. If
authentication fails or does not finish within 15 seconds, the adapter
disconnects and retries. An older bond without the required security may
require pairing again.

The ESP32 retains bonding keys in Bluedroid's storage and saves the last
successfully connected keyboard's identity address and address type in NVS.
It selects advertisements using the MX Keys Mini name, known bond/identity
addresses, or (when bonds exist) both the HID service UUID and keyboard
appearance. It connects using the current advertised address and its type,
allowing Bluedroid to resolve private addresses using stored identity keys.

Reconnection does not depend exclusively on a fixed advertising address.
However, a nameless keyboard using a private address must advertise both HID
and keyboard appearance to be selected. These advertisement hints cannot
uniquely identify an MX Keys Mini: another keyboard with those hints may be
selected and prompt for pairing. A saved identity is a recognition hint,
not an exclusive device whitelist.

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

## Firmware structure

`src/main.cpp` owns scanning, authentication, service discovery, report decoding,
and console/LED updates. BLE callbacks copy events or raw input into FreeRTOS
queues; they never change the connection state or search the subscription table.
Every reconnect rediscovers GATT services and rebuilds the Report Map and Report
Reference mappings. The keyboard becomes ready only after authentication and
verified notification registration.

Each keyboard input report ID has a one-element overwrite mailbox and its own
decoded state. The owner combines those states, including modifiers, removes
duplicate keys, and retains the first six distinct key usages. A release from
one report ID therefore does not release keys still held in another. The
firmware supports up to 16 keyboard input reports of up to 512 bytes each;
invalid reports disconnect the keyboard and release the output.

`src/kc85_output.cpp` owns the KC85 transmitter in a task on the other CPU core.
It receives the latest translated key through another overwrite mailbox, while
`src/kc85_keyboard.cpp` generates the original pulse protocol. A frame already
on the wire completes in roughly 36–50 ms; the next frame uses the newest state.
Short presses superseded by a release before transmission may be skipped.

`src/hid_report_map.cpp` parses HID layouts and `src/hid_keyboard.cpp` merges
keyboard states and translates usages to ISO-7 values.

## Validation

Build the adapter with `pio run`. Host-side report decoding, state merging,
and key translation checks can be run without the board:

```sh
clang++ -std=c++11 -Wall -Wextra -Werror -Iinclude \
  tests/keyboard_reports.cpp src/hid_keyboard.cpp src/hid_report_map.cpp \
  -o /tmp/kc85_keyboard_reports
/tmp/kc85_keyboard_reports
```

On hardware, check initial pairing, a wrong or timed-out passkey, reconnecting
after a power cycle/address change, and rapid press/release bursts. Confirm
KC85 pulse timing with a logic analyzer while Bluetooth notifications arrive.
