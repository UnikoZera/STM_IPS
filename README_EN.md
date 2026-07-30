# ⚡ STM IPS

> **STM32F401RC-driven 160×80 IPS Embedded Multimedia Platform**  
> **Display · Animation · MJPEG Video · USB CDC · Embedded File System**

![STM32](https://img.shields.io/badge/MCU-STM32F401RCT6-03234B?logo=arm)
![Display](https://img.shields.io/badge/Display-160%C3%9780_IPS-00BCD4)
![Protocol](https://img.shields.io/badge/Protocol-USB_CDC_Custom-7C4DFF)
![License](https://img.shields.io/badge/License-AGPLv3-blue)
![Status](https://img.shields.io/badge/Status-Active-success)

- [Chinese Version](./README.md)

---

## 📋 Table of Contents

- [System Overview](#-system-overview)
- [Hardware Architecture](#-hardware-architecture)
- [Software Architecture](#-software-architecture)
- [Core Features](#-core-features)
  - [Display System](#-display-system)
  - [Animation Engine](#-animation-engine)
  - [MJPEG Video Playback](#-mjpeg-video-playback)
  - [BL Compressed Video](#-bl-compressed-video)
  - [Embedded File System](#-embedded-file-system)
  - [USB Controller](#-usb-controller)
  - [SPI DMA Transport Layer](#-spi-dma-transport-layer)
  - [Performance Monitoring](#-performance-monitoring)
- [USB Communication Protocol](#-usb-communication-protocol)
- [File System Design](#-file-system-design)
- [Host Tools](#-host-tools)
- [Environment Setup](#-environment-setup)
- [Quick Start](#-quick-start)
- [Development Guide](#-development-guide)
- [Docs & Closed-Loop Notes](#-docs--closed-loop-notes)
- [License](#-license)

---

## 🔭 System Overview

**STM IPS** runs on **STM32F401RC** (Cortex-M4, 84 MHz) with a **160×80 IPS TFT** as the main output, refreshed over **SPI DMA**. It includes:

- A custom **animation engine** — multiple easings, concurrent animations, layered rendering
- **picojpeg** soft-decode for MJPEG plus a custom **BL 4×4** compressed video path
- A **lightweight file system** — linear small-file allocation / sector bitmap for large files; FAT on EEPROM
- A **USB CDC custom protocol** — file burn, delete, list, bitmap query, LCD live stream, transfer abort/rollback

| Component | Stack | Role |
|:---|:---|:---|
| `Core/` | C (STM32 HAL) | LCD, animation, video decode, storage manager, USB protocol |
| `USB_DEVICE/`  STM32 USB Device | USB CDC virtual COM |
| `lcd_host_web/` | Electron + Svelte + local FFmpeg | Transcoding, serial burn, file management, bitmap and LCD preview |
| `feature_tester/` | C + Python | Serial loopback / RGB565 checks |
| `docs/` | Markdown | Host↔MCU storage protocol & flow notes |

---

## ⚙️ Hardware Architecture

### MCU

| Parameter | Spec |
|:---|:---|
| **MCU** | STM32F401RCT6, Cortex-M4 FPU, 84 MHz |
| **Flash / SRAM** | 256 KB / 64 KB |
| **Package** | LQFP64 |

### Peripherals

```
┌──────────────────────────────────────────────────────────────────┐
│                         STM32F401RC                              │
│                                                                  │
│   SPI1 ── DMA ───→ ST7735S 160×80 IPS LCD                        │
│   ├─ SCK/MISO/MOSI · RES/DC/CS · BL_PWM(TIM)                     │
│                                                                  │
│   SPI2 ── DMA ───→ W25Q128 16MB Flash                            │
│   └─ 4096 × 4KB sectors, page program 256B                       │
│                                                                  │
│   I2C1 ──────────→ AT24C64 EEPROM (FAT persistence)               │
│                                                                  │
│   USB_OTG_FS ────→ USB CDC Virtual COM Port                      │
│                                                                  │
│   TIM2 / TIM3 / TIM4 / TIM9 ── timers / PWM                      │
└──────────────────────────────────────────────────────────────────┘
```

### W25Q layout

| Region | Sectors | Size | Policy |
|:---|:---:|:---:|:---|
| **Reserved** | 0 ~ 1 | 8 KB | Staging for small-file compact |
| **Small files** | 2 ~ 63 | 248 KB | Byte-linear bump allocator + conditional compact |
| **Large files** | 64 ~ 4031 | 15.5 MB | 4 KB sector bitmap (496-byte bitmap) |
| **User** | 4032 ~ 4095 | 256 KB | Reserved |

> FAT (`storage_fat_t`) lives at AT24C address `0x0000`, magic `0x0D000722`.

---

## 🧩 Software Architecture

### Layers

```
                    main.c
         ┌───────────┼───────────┐
     LCD UI      Storage      USB
   (lcd_ui.c)   Manager    Controller
         │           │           │
    ┌────▼────┐ ┌───▼────┐ ┌───▼───────┐
    │ LCD     │ │ W25Q   │ │  USB CDC  │
    │ Driver  │ │ SPI2   │ │           │
    │ + Anim  │ │ DMA    │ │           │
    └────┬────┘ └───┬────┘ └───┬───────┘
         │          │          │
    ┌────▼────┐ ┌───▼────┐ ┌──▼───────┐
    │picojpeg │ │ CRC16  │ │ AT24C    │
    │ MJPEG   │ │        │ │ (FAT)    │
    │ BL Dec  │ │        │ │          │
    └─────────┘ └────────┘ └──────────┘
```

### Main loop

```
main()
 ├── HAL + clock + GPIO/DMA/SPI/I2C/USB/TIM init
 ├── usb_controller_init()
 ├── lcd_init()
 ├── w25q_init()                 // on failure: show W25Q FAIL
 ├── storage_manager_init()      // load FAT from AT24C; fail → STORAGE FAIL
 ├── lcd_ui_init()               // bind assets by filename + layers
 └── while(1)
      ├── lcd_ui_updater()
      ├── w25q_dma_task()        // only if w25q_ok
      ├── storage_manager_task() // only if storage_ok: parse host frames
      └── usb_controller_task()
```

> On first invalid FAT magic the firmware writes a default table but may return failure, so **storage tasks might not run until the next reset**. For debug, `clear_all_files()` erases file regions—use carefully.

---

## ✨ Core Features

### 🖥️ Display

160×80 IPS over **SPI1 + DMA**; default landscape `LCD_W=160`, `LCD_H=80` (`USE_HORIZONTAL`).

```c
void lcd_fill_screen(uint16_t color);
void lcd_draw_point / line / rectangle / circle / string(...);
void lcd_screen_update_dma(void);
void lcd_draw_picture_dma(...);
void lcd_draw_picture_from_w25q(...);
void lcd_play_video_from_w25q(...);   // auto: MJPEG / BL / raw RGB565

// USB live stream when lcd_usb_stream_enabled and not downloading (cmd 0xA0)
void lcd_calculate_fps(void);
void lcd_calculate_usage(void);
```

While `storage_is_downloading()==true`, LCD USB streaming is suppressed to free bandwidth for file transfer.

### 🎞️ Animation engine

Concurrent animations + layer rendering with pluggable easings (linear / Quad / Sine / Expo / Circ / Back / Elastic, etc.).

```c
lcd_anim_manager_init();
lcd_anim_manager_add_layer(ctx, draw_cb);
lcd_anim_start(&config);     // target / duration / yoyo / path_cb ...
lcd_anim_manager_task();
lcd_anim_manager_render();
```

Default `lcd_ui_init()` mounts **FPS/usage labels** and a **video layer**; picture layer can be enabled as needed.

### 🎬 MJPEG playback

**picojpeg** soft decode + Flash read cache. 14-byte container header: `MJPG` + frame_count / width / height.

### 📦 BL compressed video

4×4 blocks: two base colors + index bits. `lcd_play_video_from_w25q()` routes by magic.

### 💾 Embedded file system

| Feature | Small zone (`0x45`) | Large zone (`0x11`) |
|:---|:---|:---|
| Allocation | bump `small_next_addr` | contiguous free sectors + bitmap |
| Slots | max 32 | max 32 |
| Filename | ≤16 bytes | ≤16 bytes |
| Delete | `is_valid=0`; compact when space low | erase sectors + clear bits |
| Abort/fail | **do not** rewind next (avoid dirty tail) | erase + free bitmap |
| Typical use | configs, small assets | images, video |

```c
bool storage_manager_init(void);
void storage_manager_task(void);
bool storage_fat_load(void);
void storage_fat_save(void);
int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);
bool get_small_file_info(uint8_t id, small_file_info_t *info);
bool get_large_file_info(uint8_t id, large_file_info_t *info);
bool compact_small_files(void);
void clear_all_files(void);
bool storage_is_downloading(void);
```

**Asset binding (see `lcd_ui.c`; editable):**

```c
// large name "photo_t" → still image (default)
int16_t idx = find_large_file_by_name("photo_t");
// video name: repo example is "fff"; host VP often defaults to "vp_vid"
// names must match or the list will show the file but UI will not bind it
int16_t vid = find_large_file_by_name("fff");
// address = start_sector * 4096; bound once in lcd_ui_init
```

**Small-file compact:**

- Trigger: after small-file delete, if `END - small_next_addr < 4096`
- Flow: sort valid files by address → batch by shared 4 KB sectors → copy into 8 KB reserved area → erase sources → rewrite compacted from zone start → erase old high-water tail → update `small_next_addr`
- Batch total size must be ≤ 8 KB or compact fails

### 🔌 USB controller

Custom frames over CDC. Device → host:

```
[0xAA][0x55][CMD][LEN LE 2B][PAYLOAD...]
```

Host → device (storage manager):

```
[0xBB][0x44][CMD][TOTAL_SIZE LE 4B][PAYLOAD_LEN LE 2B][DATA...][CRC16 LE]
```

CRC: **CRC-16/USB** (same as the Electron host in `lcd_host_web/src/lib/protocol.js`).

### 🔗 SPI DMA / EEPROM / CRC

- W25Q: sync R/W + DMA write/read state machine (`w25q_dma_task`)
  - SPI goes through wrappers: `w25q_spi_transmit` / `transmit_receive` / DMA helpers — **flush RX FIFO first**
  - Sync reads prefer full-duplex `TransmitReceive` (command + dummy together)
  - Page program waits SPI BSY before CS high to reduce incomplete latch
- AT24C: paged buffer API (no DMA)
- `crc16_usb_packing(data, len, has_crc)`

### 📊 Performance

FPS and CPU load estimates via **DWT_CYCCNT**.

---

## 📡 USB Communication Protocol

### Host → device frame

```
[0][1]  0xBB 0x44
[2]     CMD
[3..6]  total_file_size uint32 LE   // full size on first data packet; else often 0
[7..8]  payload_len uint16 LE       // = data_len + 2
[9..]   payload
[tail]  CRC16/USB little-endian (from BB through data, excluding CRC)
```

### Commands

| Command | Code | Dir | Description |
|:---|:---:|:---|:---|
| Large-file data | `0x11` | H→D | First packet allocates sectors and writes; later packets continue |
| Small-file data | `0x45` | H→D | First packet advances `small_next_addr` and writes |
| End download | `0x14` | H→D | Filename ≤16 B, register FAT; full slots → `0x06` / `0x07` |
| **Abort download** | **`0x15`** | H→D | Cancel/timeout: large files erase and free bitmap |
| Delete file | `0x19` | H→D | `[file_type, file_index]`, type=`0x11`/`0x45` |
| Query list | `0x20` | H→D | TLV: entry_count + slot_count + records |
| Query bitmap | `0x21` | H→D | Large-zone bitmap 496 B |
| LCD stream | `0x10` | H→D | `0x01` on / `0x00` off / empty = query |
| Continue | `0xA1` | D→H | Ready for next packet / abort ack |
| Error | `0xE0` | D→H | 1-byte error code |
| LCD frame | `0xA0` | D→H | RGB565 stream |

### Error codes

| Code | Meaning |
|:---:|:---|
| `0x01` | CRC error |
| `0x02` | Unknown delete type |
| `0x03` | No contiguous large space |
| `0x04` | Small zone full |
| `0x05` | Continue type mismatch |
| `0x06` | Large file slots full |
| `0x07` | Small file slots full |
| `0x08` | Invalid delete index |
| `0x09` | Unknown command |
| `0x0B` | Flash write failed |

### Burn sequence (sketch)

```
Host                              Device
 |-- 0x11/0x45 first + size ----->| allocate + write → 0xA1
 |-- more data packets ---------->| write → 0xA1
 |-- 0x14 filename -------------->| register FAT → 0xA1
 |-- 0x20 list ------------------>| TLV directory
 |  (cancel) 0x15 --------------->| rollback (erase large) → 0xA1
```

Device also auto-`abort_download_common()` after **~5 s** without a new download packet.

### 0x20 list TLV (summary)

```
[entry_count][slot_count][slot_records...][file_records...]
slot: rLen=10, tag=0xFF, start_sector, sector_count   // used runs in large zone
file: rLen, tag(bit7: 0=small/1=large), index, name_len, name, addr/sector, size
      large also has sector_count; small rLen=12+name_len, large rLen=16+name_len
```

---

## 🗂️ File System Design

```
W25Q128 (16MB)
═══════════════════════════════════════════
S0~1      8KB    Reserved (compact staging)
S2~63     248KB  Small zone — linear next + conditional compact
S64~4031  15.5MB Large zone — bitmap contiguous allocation
S4032~4095 256KB User zone
═══════════════════════════════════════════
AT24C @0x0000  storage_fat_t (magic / next / counts / tables / bitmap)
```

**Design invariants & write path (current code — update this when code changes):**

- Large free sectors are **erased then cleared in the bitmap** on **delete/abort**.
- **Download/burn path does not erase-before-write by default** (assumes delete erased sectors; dirty reuse or brown-out can corrupt playback).
- `flash_write_and_verify`: prefers **`w25q_write_data_dma`**; on DMA start success the packet **returns OK without verify**; next packet waits DMA at entry; only sync fallback runs `memcmp`. Failures report `0x0B`.
- Small files only append after `small_next_addr`; delete does not rewind next.
- Large abort/slot-full: free allocated sectors; small abort/slot-full: keep next; holes reclaimed by later compact.
- ~**5 s** without a new download packet auto-aborts.

---

## 🛠️ Host Tools

### 🖥️ `lcd_host_web/`

Electron + Svelte desktop host. Its main process invokes local FFmpeg; Python, Flask, and a local HTTP service are not runtime dependencies.

| Capability | Notes |
|:---|:---|
| Image/video convert | → RGB565 / MJPEG |
| Serial burn | `0x11`/`0x45` chunks + `0x14` end |
| Cancel transfer | `0x15` → MCU rollback |
| List / delete | `0x20` / `0x19` |
| Flash bitmap | `0x21` large-zone occupancy UI |
| LCD stream | `0x10` + receive `0xA0` preview |

```bash
cd lcd_host_web
pnpm install
pnpm dev
```

Build and start the desktop application:

```bash
pnpm build
pnpm start
```

> FFmpeg is required; set `STM_IPS_FFMPEG` to select its executable. Protocol details: `docs/storage_host_mcu_flow.md`.

### 🔧 `feature_tester/`

Serial TX/RX helpers and RGB565 decode utilities.

---

## 🔧 Environment Setup

| Item | Requirement |
|:---|:---|
| IDE | STM32CubeIDE ≥ 1.15 (or Makefile + arm-none-eabi-gcc) |
| Node.js + pnpm | Electron host development/build |
| FFmpeg | Desktop-host video transcoding |
| Debugger | ST-Link / OpenOCD / CubeProgrammer |

```bash
# Build (CubeIDE Build, or)
cd Debug && make -j4

# Flash example
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program Debug/stm_ips.elf verify reset exit"
```

---

## 🚀 Quick Start

1. **Flash firmware** to STM32F401RC  
2. **Power up**: init peripherals → load FAT → `lcd_ui` looks up configured image/video names  
3. **Start the desktop host** in `lcd_host_web`, then connect the CDC serial port
4. **Convert and burn** (`0x11` large, `0x45` small); filename must match `lcd_ui.c`  
5. **Refresh list**; **reset once** before expecting playback (bind only in `lcd_ui_init`)  

If the list does not respond: check `storage_ok` (reset if FAT was just created) and free the COM port.  
If listed but not playing: check bind name, container magic (`MJPG`/`BL`), and whether Flash really programmed.

---

## 👨‍💻 Development Guide

### Change boot asset names

Edit `find_large_file_by_name("...")` in `Core/Src/lcd_ui.c` to match host burn names.

### Keep the main loop responsive

Long erase/program runs on the protocol path; Flash DMA progress depends on `w25q_dma_task()`; while waiting DMA, keep pumping `usb_controller_task()`. Avoid heavy work in ISRs.

### Format

```c
// clear_all_files();  // erase used large sectors + all small sectors + reset FAT
```

### CRC

```c
uint16_t crc = crc16_usb_packing(data, len, false); // compute
// crc16_usb_packing(frame, frame_len, true);       // verify → 1/0
```

### For automation / AI

See root [`AGENT.md`](./AGENT.md): build entry points, protocol pitfalls, fault table, verification checklist.

---

## 📘 Docs & Closed-Loop Notes

| Doc | Content |
|:---|:---|
| [`AGENT.md`](./AGENT.md) | Operator/AI handbook and change boundaries |
| [`docs/storage_host_mcu_flow.md`](./docs/storage_host_mcu_flow.md) | Host↔MCU storage protocol, burn/delete/abort flows, write-path notes |

Current firmware covers chunked ACK, end registration, slot-full errors, write fail `0x0B` (sync verify path), abort `0x15`, download timeout, list/bitmap query.

**Known edges / risks (as implemented):**

- DMA write success path **does not per-packet verify** — bad data may still get `0xA1`
- Download path **does not erase-before-write by default**
- Same-name overwrite is not automatic; delete has no dedicated success ACK (rely on list)
- Host end-frame timeout policy is implementation-defined

When docs disagree with code, **`Core/Src` wins**.

---

## 📜 License

Copyright (C) 2026 **UnikoZera**

This program is free software under the **GNU Affero General Public License v3** (or later).

**Third party:**

| Component | License | Notes |
|:---|:---|:---|
| picojpeg | Public domain | Rich Geldreich |
| STM32 HAL | ST SLA | STMicroelectronics |
| CMSIS | Apache 2.0 | ARM |

> Built on an STM32CubeMX-generated framework.

---

<p align="center">
  <sub>Built with ❤️ by UnikoZera · STM32F401RC · 160×80 IPS · 2026</sub>
</p>
