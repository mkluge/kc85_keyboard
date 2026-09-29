Import("env")

# The PlatformIO board package bundles an obsolete TinyUF2 bootloader and
# partition table. Keep the repaired board bootloader intact and upload only
# the application image at ESP32_APP_OFFSET (0x10000).
board = env.BoardConfig()
board.update("upload.use_1200bps_touch", False)
board.update("upload.wait_for_upload_port", False)
env.Replace(
    FLASH_EXTRA_IMAGES=[],
    UPLOADERFLAGS=[
        "--chip",
        env.subst("$BOARD_MCU"),
        "--port",
        '"$UPLOAD_PORT"',
        "--baud",
        "$UPLOAD_SPEED",
        "--before",
        board.get("upload.before_reset", "default_reset"),
        "--after",
        board.get("upload.after_reset", "hard_reset"),
        "write_flash",
        "-z",
        "--flash_mode",
        "${__get_board_flash_mode(__env__)}",
        "--flash_freq",
        "${__get_board_f_image(__env__)}",
        "--flash_size",
        board.get("upload.flash_size", "detect"),
    ],
    UPLOADCMD=(
        '"$PYTHONEXE" "$PROJECT_DIR/upload_firmware.py" "$UPLOADER" '
        '"$UPLOAD_PORT" "$UPLOAD_SPEED" '
        '"${__get_board_flash_mode(__env__)}" '
        '"${__get_board_f_image(__env__)}" '
        '"%s" "$ESP32_APP_OFFSET" "$SOURCE"'
        % board.get("upload.flash_size", "detect")
    ),
)
