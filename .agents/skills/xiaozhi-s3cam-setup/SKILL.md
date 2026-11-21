---
name: xiaozhi-s3cam-setup
description: Help builders bring up WangZi's Xiaozhi ESP32-S3 N16R8 CAM firmware, check board-specific wiring and ESP-IDF settings, configure Mandarin wake words, and diagnose startup logs.
license: MIT
---

# Xiaozhi ESP32-S3 CAM setup

Use this guide for the 44-pin N16R8 CAM build maintained in [wangzifan396-wzf/xiaozhi-esp32-s3cam](https://github.com/wangzifan396-wzf/xiaozhi-esp32-s3cam), derived from 78/xiaozhi-esp32. Reply in the builder's language. Start at the step they need. For a log question, use the first relevant error and the board profile before suggesting changes.

## Identify the hardware

Match the board marking and peripherals to the [hardware photos and project notes](https://github.com/wangzifan396-wzf/xiaozhi-esp32-s3cam#readme). This profile uses 16 MB flash, 8 MB octal PSRAM, a 240×240 SPI ST7789, OV3660 camera, simplex I²S audio and an onboard MicroSD slot. If someone has a different S3 CAM or an AI-Thinker ESP32-CAM, ask for its model or schematic before giving GPIO assignments.

| Module | Signal → ESP32-S3 GPIO on this board |
| --- | --- |
| ST7789 | MOSI 20, CLK 19, DC 47, RST 21, CS 45, BL 14 |
| ZTS6672 microphone | WS 1, SCK 2, mic data out → MCU input 42 |
| MAX98357A amplifier | MCU output 46 → amp DIN, BCLK 3, LRCK 41 |
| OV3660 | D0–D7 in order: 11, 9, 8, 10, 12, 18, 17, 16. XCLK 15, PCLK 13, VSYNC 6, HREF 7, SIOC 5, SIOD 4 |
| Onboard SDMMC | CMD 38, CLK 39, D0 40, 1-bit mode |

For a different checkout, look up the board selected in `main/CMakeLists.txt` and its `main/boards/bread-compact-wifi-s3cam/config.h` before reusing these values.

## Get a matching build

1. Use the builder's existing checkout of this fork, or obtain it in a fresh directory. Start an ESP-IDF v5.5.4 shell and run `idf.py --version` from the project root. On a fresh checkout run `idf.py set-target esp32s3`. On an existing build, check `CONFIG_IDF_TARGET` first. Changing targets regenerates configuration, so preserve existing settings if a change is necessary.
2. Run `idf.py menuconfig`. Select **Bread Compact WiFi + LCD + Camera** and **ST7789 240*240** for the display documented above. Check the saved `sdkconfig` for `CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_CAM=y`, `CONFIG_LCD_ST7789_240X240=y`, 16 MB flash, octal PSRAM and the project's `partitions/v2/16m.csv`. The separate LCD-only board entry omits the camera implementation.
3. Run `idf.py build`. If it stops, capture the first build error and the selected target, board and ESP-IDF version. A successful compile establishes that the firmware builds; device behavior needs a board test.
4. For a requested flash, identify the connected board and its serial port first. Substitute the actual port in `idf.py -p PORT flash monitor`. Flashing writes firmware to that board. If the user only asked to build, finish at the build result. The monitor exits with Ctrl+].

## Configure two Mandarin wake words

With ESP32-S3 and PSRAM enabled, select **Multinet model (Custom Wake Word)** in the project's Wake Word Type menu. In ESP-SR select a Chinese MultiNet model, for example `CONFIG_SR_MN_CN_MULTINET6_QUANT=y`. For the default asset build, keep `CONFIG_FLASH_DEFAULT_ASSETS=y`. The built-in AFE/WakeNet mode uses a different model path.

| menuconfig field | Example input |
| --- | --- |
| Custom Wake Word | `xiao zhi xiao zhi;xiao zhi tong xue` |
| Custom Wake Word Display | `小智小智;小智同学` |

Use spaces between pinyin syllables and an ASCII semicolon between phrases. Keep the two lists equal in length and in the same order. Rebuild. The fork's `scripts/build_default_assets.py` prints `custom wake words: 2 commands` and each command/display pair when generating default assets. If assets are already up to date this line may only appear in the earlier asset-build log. A “no multinet models” error means the mode was enabled without selecting a model. A custom asset pack supplies its own `index.json`; check its `multinet_model.commands` if the running device still uses different words.

After flashing the matching app and assets, let the device return to idle between phrases. Say each phrase separately and look for two distinct command IDs in the serial monitor's `CustomWakeWord` detection lines. Report which phrases the board actually recognized when a device or logs are available.

## Narrow down bring-up failures

- **Display lights up, camera fails** — Check the CAM board selection and camera initialization log. Compare the OV3660 signal names to the map above. A working display only exercises the SPI pins.
- **SD card fails, photos cannot be saved** — Check for `SD card mounted: /sdcard` or the error following `SD card mount failed:`. Check card seating and the three SDMMC signals. The firmware disables automatic formatting on mount failure. Preserve photos while diagnosing the card.
- **Speaker works, neither wake word fires** — Verify the microphone on the separate input pins and check the startup list of active speech commands. If one phrase already fires, start by checking the second pinyin/display pair and the assets that were flashed.
- **No USB board is available** — Give the exact configuration and build checks the user can perform. Identify which remaining observations need a physical board or a short serial-log excerpt.
