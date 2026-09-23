# Logitech MX Keys Mini keypress indicator

This firmware makes the Adafruit QT Py ESP32-S3 a Bluetooth Low Energy HID
host. It finds a nearby Logitech MX Keys Mini, bonds with it, and flashes the
onboard NeoPixel white for 100 ms for every non-empty keyboard report.

## Pairing

1. Upload with `pio run --target upload`.
2. Open the 115200-baud serial monitor with `pio device monitor`.
3. Hold one of the three Easy-Switch keys until its LED blinks rapidly.
4. When the serial monitor prints a six-digit pairing code, type it on the
   MX Keys Mini and press Enter.
5. Wait for `Ready; listening to ...` in the serial monitor.

During each scan, the serial monitor lists every visible BLE device with its
name, address, signal strength (RSSI), and an `HID` marker when advertised.

The ESP32 retains the bond and recognizes the keyboard by its bonded BLE
address on later reconnections, even when it no longer advertises its name.
The ESP32-S3 connects to the keyboard over Bluetooth Low Energy.
