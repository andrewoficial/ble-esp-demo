# Firmware releases

1. Set the release number in `version.txt` (for example, `1.0.1`).
2. Run `pio run` or `pio run -t upload`.
3. Give users the generated `release/ble-esp32-v<version>.bin` for initial USB
   flashing, or `release/ble-esp32-app-v<version>.bin` for web OTA updates.

The generated file is a complete image containing the bootloader, partition
table, and application. Flash it to an ESP32 with the flashing tool's binary
address set to `0x0`. It is built for a 4 MB ESP32 flash chip.

The `ver` command and web status endpoint report the version embedded in the
running application image.

For BLE scanner testing, `GETADVDESC` reports the configured advertising and
scan-response fields, including the raw manufacturer bytes. `GETADVSVC` reports
the NUS GATT service and its RX/TX characteristic UUIDs and properties. These
commands are available over Serial and BLE NUS. In `GETADVDESC`, the first two
manufacturer bytes are company ID `0xFFFF`, followed by four IPv4 bytes and a
four-byte little-endian float temperature. The classic ESP32 target in ESP-IDF
6.1 does not expose the temperature sensor driver, so that temperature is
currently reported as zero.

Web OTA accepts only the application image, not the combined image. The web
progress indicator reports bytes confirmed written to flash, not just bytes
sent by the browser. The device reads the upload in blocks of up to 5 KB and
prints OTA stage and 5% progress diagnostics to the serial terminal. OTA
diagnostic messages are printed directly to the serial terminal during the
update, so they remain visible even when `LOGOFF` is active. If the browser
stops sending data, the terminal reports the wait every 15 seconds and the
upload is aborted after 60 seconds without data. Before
using OTA, configure an 8- to 63-character password in the Serial Monitor at
115200 baud with `SETOTAPASS <password>`. Select the `ble-esp32-app-v<version>.bin`
file in the web page, enter that password, and start the update. The device
restarts after successful validation. OTA uses the local HTTP server; use it
only on a trusted network because HTTP does not encrypt the password or image.

Devices using the previous single-app partition table need one initial USB
flash with the combined image before web OTA is available.
