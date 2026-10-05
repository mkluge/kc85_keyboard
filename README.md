# PERIBOARD-805 keypress indicator

This firmware makes the Adafruit QT Py ESP32-S3 a Bluetooth Low Energy HID
host. It finds a nearby PERIBOARD keyboard, bonds with it, and flashes the
onboard NeoPixel white for 100 ms for every non-empty keyboard report.

## Pairing

1. Upload with `pio run --target upload`.
2. Open the 115200-baud serial monitor with `pio device monitor`.
3. Select a Bluetooth slot on the keyboard, then start pairing. On current
   PERIBOARD-805 Ergo models, use `Fn+Q`, `Fn+W`, or `Fn+E` for a slot and then
   hold `Fn+T` for about two seconds.
4. Wait for `Ready; listening to ...` in the serial monitor.

During each scan, the serial monitor lists every visible BLE device with its
name, address, signal strength (RSSI), and an `HID` marker when advertised.

The ESP32 retains the bond for later reconnections. The ESP32-S3 supports BLE
only. An older PERIBOARD-805L offering only Bluetooth 3.0 Classic HID cannot
connect to this board.
