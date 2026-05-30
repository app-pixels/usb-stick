# usb-stick

**USB Stick** · v1.0.0

Plug in over USB-C; the SD card appears as a removable drive.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#tool` `#offline`

USB Mass Storage. Connect to a PC and the SD card mounts as a regular drive — copy files in and out at full speed.

## Controls
- Touch — browse files on screen
- **BOOT** — toggle write-protect (host sees the drive read-only)

## Setup
No `setup.txt` needed.

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/usb-stick_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/usb-stick_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/usb-stick).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/usb-stick
