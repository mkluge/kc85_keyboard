#!/usr/bin/env python3
"""Reset the running firmware into ROM download mode and upload its app."""

import subprocess
import sys
import time

from serial import Serial, SerialException
from serial.tools.list_ports import comports


BOOTLOADER_COMMAND = b"PIO_ENTER_BOOTLOADER\n"
ROM_USB_ID = (0x303A, 0x1001)


def rom_port():
    for port in comports():
        if (port.vid, port.pid) == ROM_USB_ID:
            return port.device
    return None


def enter_rom_bootloader(port):
    existing_rom_port = rom_port()
    if existing_rom_port:
        return existing_rom_port

    print("Requesting ROM download mode from the running firmware...")
    try:
        with Serial(port, 115200, timeout=1, write_timeout=1) as serial:
            serial.write(BOOTLOADER_COMMAND)
            serial.flush()
    except (OSError, SerialException) as error:
        raise SystemExit(f"Could not request download mode on {port}: {error}")

    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        detected_port = rom_port()
        if detected_port:
            print(f"ROM downloader ready on {detected_port}")
            return detected_port
        time.sleep(0.1)

    raise SystemExit(
        "The firmware did not enter download mode. Hold BOOT, press RESET, "
        "release BOOT, and retry the upload."
    )


def main():
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: upload_firmware.py ESPTOOL PORT BAUD FLASH_MODE "
            "FLASH_FREQ FLASH_SIZE APP_OFFSET IMAGE"
        )

    esptool, port, baud, flash_mode, flash_freq, flash_size, offset, image = (
        sys.argv[1:]
    )
    port = enter_rom_bootloader(port)
    command = [
        sys.executable,
        esptool,
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        baud,
        "--before",
        "no_reset",
        "--after",
        "watchdog_reset",
        "write_flash",
        "-z",
        "--flash_mode",
        flash_mode,
        "--flash_freq",
        flash_freq,
        "--flash_size",
        flash_size,
        offset,
        image,
    ]
    raise SystemExit(subprocess.call(command))


if __name__ == "__main__":
    main()
