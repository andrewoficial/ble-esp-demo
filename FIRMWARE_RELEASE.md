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
manufacturer bytes are company ID `0xFFFF`, followed by four IPv4 bytes, a
two-byte little-endian unsigned PID reading in millivolts (saturated at
3500 mV), and a two-byte little-endian signed BMP180 temperature in hundredths
of a degree Celsius (`0x7FFF` means no data). The scan response carries the
10-byte manufacturer structure plus the device name and fits the 31-byte BLE
limit exactly at the maximum 17-byte name. The web dashboard shows the firmware
version and hardware revision `HW 0.1.1`.

The PID sensor uses GPIO32 (ADC1), which remains available while Wi-Fi is
active. Its measured voltage is averaged over 60 samples by default, then
calculated as `(ADC_mV + offset_mV) * multiplier`. Defaults are offset `0` and
multiplier `1`. Configure and inspect it over Serial, BLE NUS, or the web
command endpoint:

| Command | Description |
| --- | --- |
| `GETPID` | Read the current calibrated value in mV |
| `SETOFSETPID<n>` / `GETOFFSETPID` | Set/read offset in mV |
| `SETAMPPID<n>` / `GETAMPPID` | Set/read multiplier |
| `SETAVGPIONTSPID<n>` / `GETAVGPIONTSPID` | Set/read averaging sample count |

Offset and multiplier accept values from `0` to `999999`; averaging accepts
`1` to `999999`. Values are stored in NVS. Check the sensor output voltage
before connecting it: do not exceed the ESP32 GPIO input limits. GPIO12 is an
ADC2 pin on the classic ESP32 and cannot provide reliable conversions while
Wi-Fi is active; use GPIO32/ADC1 as configured.

The BMP180 (GY-68) is read over I2C at address `0x77` using SDA GPIO21 and SCL
GPIO22. It reports temperature and pressure; BMP180 does not measure humidity.
The temperature multiplier defaults to `1` and is stored in NVS:

| Command | Description |
| --- | --- |
| `GETBMXTERM` | Read BMP180 temperature in °C |
| `GETBMXPRESS` | Read BMP180 pressure in Pa |
| `GETBMXTERMAMP` / `SETBMXTERMAMP<n>` | Read/set the temperature multiplier |
| `GETBMXHYDM` / `SETBMXHYDMAMP<n>` | Return unsupported: BMP180 has no humidity sensor |

The web dashboard shows the live temperature and pressure and allows changing
the temperature multiplier. Pressure is shown in hPa on the dashboard.

The display prototype supports the LCDWiki MSP3526 (ST7796, 320x480) using
LVGL. It cycles through three demo pages automatically; touch input is not
enabled yet. Proposed landscape wiring for the current ESP32-WROOM board:

| MSP3526 pin | ESP32 GPIO |
| --- | --- |
| SCK | 18 |
| SDI (MOSI) | 23 |
| LCD_CS | 27 |
| LCD_RS (D/C) | 16 |
| LCD_RST | 17 |
| VCC / GND | 3.3V / GND |
| SDO (MISO), SD_CS | Not connected for the display demo |
| CTP_SCL / CTP_SDA | Touch not enabled; these will share BMP180 I2C GPIO22/21 |
| CTP_RST / CTP_INT | Touch not enabled |

The backlight pin is not GPIO-driven in this prototype. Do not connect the
backlight LED directly to an ESP32 GPIO. Pin orientation and backlight behavior
should be checked against the actual module before wiring it.

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
