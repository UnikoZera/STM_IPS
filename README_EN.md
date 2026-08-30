# STM IPS

[中文说明](README.md) · [Protocol contract](lcd_host_web/protocol/README.md) · [Contributor notes](AGENT.md)

STM IPS is an embedded media-display project for an **STM32F401RCT6** and a **160 × 80 IPS LCD**. The firmware receives files over USB CDC, stores them in external W25Q128 flash, and renders RGB565 images or RAW5/MJPEG media from flash. This repository also includes a browser-based Host for conversion, Web Serial transfer, device-file management, and LCD-stream preview.

> This README follows the current source, `lcd_host_web/protocol/contract.json`, and tests. It documents the default configuration; update it together with any change to wiring, asset names, or protocol behavior.

## Highlights

- SPI1 + DMA display path for a landscape 160 × 80 IPS LCD, with primitives, text, picture, animation-layer, and performance rendering.
- SPI2 W25Q128 (16 MiB) flash and I2C1 AT24C64 EEPROM, which persists the file allocation table (FAT).
- USB OTG FS CDC virtual COM port with a custom CRC-16/USB binary protocol.
- Contiguous-sector/bitmap allocation for large files; append-only small-file allocation with conditional compaction.
- FastAPI and Web Serial Host for RAW5 RGB565 and MJPEG conversion, transfer/abort, list/delete, bitmap inspection, and LCD streaming.
- The home screen currently looks for the large file `photo_t` as its still-image fallback. The default video name is empty, so no video is auto-bound. Both settings are in `Core/Src/lcd_ui.c`.

## Hardware and default pinout

| Item | Current configuration |
| --- | --- |
| MCU | STM32F401RCT6, Cortex-M4F, 84 MHz, LQFP64 |
| LCD | 160 × 80 IPS, `USE_HORIZONTAL = 2` (landscape) |
| LCD bus | SPI1: PA5 SCK, PA6 MISO, PA7 MOSI; PB0 RES, PB1 DC, PB2 CS; PA3 / TIM9 CH2 backlight PWM |
| External flash | W25Q128 on SPI2: PB13 SCK, PB14 MISO, PB15 MOSI, PA8 CS |
| EEPROM | AT24C64 on I2C1: PB8 SCL, PB9 SDA |
| USB | USB OTG FS Device / CDC: PA11 DM, PA12 DP; PB12 `USB_EN` |
| Debug | SWD: PA13 SWDIO, PA14 SWCLK |
| User input | TIM2 encoder on PA0 / PA1, encoder button PA2, button PC15 |

The 25 MHz HSE is PLL-derived to 84 MHz SYSCLK/HCLK, with a 48 MHz USB clock. CubeMX configuration is in [`stm_ips.ioc`](stm_ips.ioc).

## Repository layout

| Path | Purpose |
| --- | --- |
| `Core/` | Application firmware: LCD/UI, animation, RAW5/MJPEG playback, W25Q, AT24C, storage, and USB protocol |
| `USB_DEVICE/` | STM32 USB Device CDC configuration and callbacks |
| `Drivers/`, `Middlewares/` | STM32 HAL, CMSIS, USB Device, ARM DSP, and other dependencies |
| `lcd_host_web/` | FastAPI conversion service, Web Serial UI, protocol contract, Python/TypeScript tests |
| `feature_tester/` | Serial send/receive and RGB565 utilities |
| `Debug/` | STM32CubeIDE-generated GNU Make build directory and artifacts; do not edit generated files manually |

## Firmware execution model

After GPIO, DMA, I2C, SPI, USB, and timer initialization, the firmware initializes the USB controller, LCD, W25Q, storage manager, and UI. The main loop continuously runs:

```text
lcd_ui_updater()
  -> w25q_dma_task()          # W25Q DMA / async erase state machine
  -> storage_manager_task()   # Parse Host frames and perform storage work
  -> usb_controller_task()    # USB CDC I/O
```

`w25q_dma_task()` and `usb_controller_task()` must continue running while transfers are in progress. Do not put long erase, program, or wait operations in interrupts.

If W25Q initialization fails, the display shows `W25Q FAIL`. If the AT24C has no valid FAT, the firmware creates a default table but `storage_manager_init()` returns failure; **reset once** after this first initialization before using file-management functions.

## Flash layout and file system

W25Q128 has 4096 sectors of 4 KiB each:

| Region | Sectors | Capacity | Use |
| --- | ---: | ---: | --- |
| Reserved | 0–3 | 16 KiB | Small-file compaction staging |
| Small files | 4–63 | 240 KiB | Linear append allocation |
| Large files | 64–4031 | 15.5 MiB | Contiguous sectors managed by a 496-byte bitmap |
| User | 4032–4095 | 256 KiB | Not currently used by the storage manager |

The FAT is stored at AT24C64 address `0x0000`; its current magic is `0x0D000721`. It records at most 40 small files and 35 large files. Filenames have 16 bytes of storage: the end-download command truncates to 15 bytes and appends `\0`.

Transfer data is persisted in CRC blocks: every complete block is **1022 data bytes + 2 CRC-16/USB bytes**, making a 1024-byte physical flash block. The final block is padded with `0xFF` to 1022 data bytes and its storage CRC is recalculated. A large-file first packet reserves contiguous sectors from its declared raw size; small files only move the high-water pointer forward. Deleting or aborting a large file erases and releases its sectors. Deleting a small file does not rewind the pointer; compaction is attempted once fewer than 16 KiB remain.

> The download path assumes reusable storage sectors have already been erased; it does not erase before every write. Do not call `clear_all_files_manual()` unless you understand the consequences: it clears managed file regions and resets the FAT.

## Media formats and boot assets

The Host currently produces containers with this 14-byte header:

```text
magic[4] | frame_count[u16le] | width[u16le] | height[u16le] | reserved[4]
```

- `RAW5`: continuous **big-endian RGB565** frames after the header, for images or raw video-frame sequences.
- `MJPG`: repeated `jpeg_length[u32le] | JPEG bytes` records after the header; decoded by picojpeg on the MCU.

The firmware also accepts headerless raw RGB565. Playback auto-detects `MJPG` and `RAW5`; use files and dimensions appropriate for the 160 × 80 display. A successful `0x14` end-download command reruns `lcd_ui_init()`, so a matching asset is rebound immediately.

Change startup binding in `Core/Src/lcd_ui.c`:

```c
#define UI_VIDEO_NAME    ""        /* empty: no default video */
#define UI_PICTURE_NAME  "photo_t" /* image fallback */
```

The home view prefers video, then the picture, then `NO MEDIA`. Host quick defaults may use `vp_vid` / `vp_img`; rename the transfer to match these macros when automatic display is desired.

## USB CDC protocol

The machine-readable source of truth is [`lcd_host_web/protocol/contract.json`](lcd_host_web/protocol/contract.json). Summary:

Host → MCU:

```text
BB 44 | command[u8] | total_size[u32le] | payload_length[u16le] | data | crc16[u16le]
```

The header is 9 bytes and `payload_length = data_length + 2`. CRC-16/USB covers **only `data`**—not magic, command, size fields, or the CRC. Empty data uses `0x0000`. A Host frame carries at most 1024 data bytes.

MCU → Host:

```text
AA 55 | command[u8] | payload_length[u16le] | payload
```

| Command | Direction | Meaning |
| --- | --- | --- |
| `0x11` | Host → MCU | Large-file data |
| `0x45` | Host → MCU | Small-file data |
| `0x14` | Host → MCU | Finish download and commit filename |
| `0x15` | Host → MCU | Abort and roll back a download |
| `0x19` | Host → MCU | Delete: `[file_type, file_index]` |
| `0x20` | Host → MCU | File list and allocated large-region runs |
| `0x21` | Host → MCU | Large-region bitmap |
| `0x10` | Bidirectional | LCD stream: `0x00` stop, `0x01` start |
| `0xA1` | MCU → Host | Ready for next packet / operation success |
| `0xE0` | MCU → Host | Error with a one-byte error code |
| `0xA0` | MCU → Host | LCD RGB565 stream |

LCD streaming is disabled temporarily during a download and restored after completion or abort. A download with no new data packet for 15 seconds is automatically aborted with `0x0B`. Protocol changes must update firmware, `contract.json`, the generated frontend contract, and tests together.

## Build and flash firmware

Open this directory in STM32CubeIDE and build the Debug configuration, or use the generated GNU Make files. They currently target GNU Tools for STM32 14.3.rel1.

```powershell
where arm-none-eabi-gcc
where make
make -C Debug -j4
```

The build produces `Debug/stm_ips.elf`. One OpenOCD flashing example:

```powershell
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program Debug/stm_ips.elf verify reset exit"
```

STM32CubeProgrammer and CubeIDE download tools work as well. `Debug/` is generated by CubeIDE; if the project is regenerated, treat `stm_ips.ioc` and source files as authoritative.

## Run the Host

The Host requires Python, FFmpeg, and a Chromium browser with Web Serial support (Chrome or Edge). This project uses the **`py312-classic`** Conda environment; do not install project dependencies into Conda `base`.

```powershell
conda activate py312-classic
cd lcd_host_web
python -m pip install -r requirements.txt
python server.py
```

The default URL is <http://127.0.0.1:5000>. The server first looks for system `ffmpeg` / `ffprobe`, then falls back to the executable supplied by `imageio-ffmpeg` when available. `launcher.py` chooses a free local port starting from 5000 and opens the browser; `STM_IPS_Host.bat` is the Windows launcher.

Typical flow:

1. Build and flash the firmware; reset once after a first-time FAT creation.
2. Start the Host in `py312-classic`, then open the page in Chrome or Edge.
3. Select the device CDC port via Web Serial.
4. Convert an image or video and set its filename to the desired `lcd_ui.c` asset name.
5. Select large files for media in normal use (or small files as appropriate), send data, and wait for the per-packet `0xA1` acknowledgements and final `0x14` completion.
6. Use list/delete/bitmap tools to inspect storage; enable LCD streaming to preview device output.

## Development and verification

Runtime and development Python dependencies are in `lcd_host_web/requirements.txt` and `lcd_host_web/requirements-dev.txt`; frontend checks come from `lcd_host_web/package.json`.

```powershell
conda activate py312-classic
python -m pytest -q .\lcd_host_web\tests

cd lcd_host_web
npm ci
npm run typecheck
npm test
npm run build
```

Changes to storage, USB, or protocol also need hardware validation: reset after initial FAT creation, burn a large and a small file, abort a transfer, delete and reallocate, query list/bitmap, and verify LCD-stream suspension and restoration during transfers. Static tests do not substitute for Flash, DMA, and USB testing on hardware.

## License

This project is licensed under the [MIT License](LICENSE), Copyright (c) 2026 UnikoZera. STM32 HAL, CMSIS, USB Device, ARM DSP, and other third-party components retain their included licenses.
