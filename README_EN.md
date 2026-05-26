# ⚡ STM IPS — STM32 Image Processing System

> **SPDX-License-Identifier: AGPL-3.0-or-later**
>
> **An STM32F401RC-driven 160×80 IPS embedded multimedia platform featuring image display, video playback, animation engine, USB CDC communication, and Web-based file management.**

![STM32](https://img.shields.io/badge/MCU-STM32F401RCT6-03234B)
![Display](https://img.shields.io/badge/Display-160×80_IPS-00BCD4)
![Protocol](https://img.shields.io/badge/Protocol-USB_CDC_Custom-7C4DFF)
![License](https://img.shields.io/badge/License-AGPLv3-blue)
![Status](https://img.shields.io/badge/Status-Active-success)

---

## 📋 Table of Contents

- [System Overview](#-system-overview)
- [Hardware Architecture](#-hardware-architecture)
- [Software Architecture](#-software-architecture)
- [Core Features & Code Examples](#-core-features--code-examples)
  - [Display System](#-display-system)
  - [Animation Engine](#-animation-engine)
  - [MJPEG Video Playback](#-mjpeg-video-playback)
  - [Storage Manager](#-storage-manager)
  - [USB Controller](#-usb-controller)
  - [SPI DMA Transfer](#-spi-dma-transfer)
- [USB Communication Protocol](#-usb-communication-protocol)
- [File System Design](#-file-system-design)
- [Host Tools](#-host-tools)
- [Environment Setup](#-environment-setup)
- [Quick Start](#-quick-start)
- [Development Guide](#-development-guide)
- [License](#-license)

---

## 🔭 System Overview

**STM IPS** is an embedded image processing & multimedia system running on the STM32F401RC. It drives a 160×80 IPS color TFT display through **SPI DMA** for high-speed framebuffer refresh, decodes MJPEG video via the open-source picojpeg library in software, and communicates with the host through a custom USB CDC protocol for file transfer and real-time data streaming.

The project consists of four sub-projects spanning firmware to host tools:

| Component | Tech Stack | Purpose |
|:---|:---|:---|
| `Core/` | C (STM32 HAL) | Firmware: LCD driver, animation engine, video decode, storage mgmt |
| `USB_DEVICE/` | C (STM32 USB Device Lib) | USB CDC virtual COM port layer |
| `lcd_host_web/` | Python Flask + HTML5 | Web-based transcoding & file management |
| `feature_tester/` | C + Python | Serial loopback test & RGB565 decode verification |

For a Chinese version of this README, see **[README.md](README.md)**.

---

## ⚙️ Hardware Architecture

### MCU

| Parameter | Spec |
|:---|:---|
| **MCU** | STM32F401RCT6 (ARM Cortex-M4, 84MHz) |
| **Flash** | 256KB (internal) |
| **SRAM** | 64KB |
| **Package** | LQFP64 |

### Peripheral Connections

```
┌────────────────────────────────────────────────────────────┐
│                     STM32F401RC                            │
│                                                            │
│   SPI1 (DMA) ────→ 160×80 IPS LCD                          │
│   ├─ PA5 (SCK) · PA6 (MISO) · PA7 (MOSI)                   │
│   ├─ PB0 (RES) · PB1 (DC) · PB2 (CS)                       │
│   └─ PA3 (Backlight PWM)                                   │
│                                                            │
│   SPI2 (DMA) ────→ W25Q128 (16MB SPI Flash)                │
│   ├─ PB13 (SCK) · PB14 (MISO) · PB15 (MOSI)                │
│   └─ PA8 (CS)                                              │
│                                                            │
│   I2C1 ────────→ AT24C64 (64Kb EEPROM)                     │
│   └─ PB8 (SCL) · PB9 (SDA)                                 │
│                                                            │
│   USB_OTG_FS ──→ USB CDC (VCP)                             │
│   └─ PB12 (USB EN)                                         │
│                                                            │
│   TIM2 / TIM3 / TIM4 / TIM9 (PWM · Timing)                 │
│                                                            │
│  (Unused) Encoder: PA0 (CH1) · PA1 (CH2) · PA2             │
│  (Unused) Button: PC15 · LED: PC13 / PC14 / PA15           │
└────────────────────────────────────────────────────────────┘
```

### Memory Layout

| Region | Sectors | Size | Purpose |
|:---|:---:|:---:|:---|
| **Reserved** | 0 ~ 1 | 8 KB | Reserved |
| **Small File** | 2 ~ 63 | 248 KB | Text & small data (byte-level packing) |
| **Large File** | 64 ~ 4031 | 15.5 MB | Images & video (sector-aligned bitmap alloc) |
| **User** | 4032 ~ 4095 | 256 KB | User-defined |

> W25Q128 total capacity 16MB, sector size 4KB. FAT is persisted in AT24C64 EEPROM.

---

## 🧩 Software Architecture

### Module Hierarchy

```
                    main.c
         ┌───────────┼───────────┐
     LCD UI      Storage      USB
   (lcd_ui.c)   Manager    Controller
         │           │           │
    ┌────▼────┐ ┌───▼────┐ ┌───▼───────┐
    │ LCD     │ │ W25Q   │ │  USB CDC  │
    │ Driver  │ │ SPI2   │ │  (HAL)    │
    │ + Anim  │ │ DMA    │ │           │
    └────┬────┘ └───┬────┘ └───┬───────┘
         │          │          │
    ┌────▼────┐ ┌───▼────┐ ┌──▼───────┐
    │ LCD PCD │ │ CRC16  │ │ AT24C    │
    │picojpeg │ │        │ │ (EEPROM) │
    │ MJPEG   │ │        │ │          │
    └─────────┘ └────────┘ └──────────┘

         STM32 HAL / SPI / I2C / DMA / TIM
```

### Main Loop Flow

```
main()
 ├── HAL_Init() + SystemClock_Config()     // 84MHz HSE + PLL
 ├── Peripheral init:
 │     MX_GPIO_Init() · MX_DMA_Init()
 │     MX_I2C1_Init() · MX_SPI1_Init() · MX_SPI2_Init()
 │     MX_USB_DEVICE_Init() · MX_TIMx_Init()
 │
 ├── usb_controller_init(&g_usb_controller)
 ├── lcd_init()
 ├── w25q_init()
 ├── storage_manager_init()
 ├── lcd_ui_init()
 │
 └── while(1)
      ├── lcd_ui_updater()          // UI render update
      ├── w25q_dma_task()           // SPI Flash DMA state machine
      ├── storage_manager_task()    // USB file transfer handling
      └── usb_controller_task()     // USB TX/RX management
```

---

## ✨ Core Features & Code Examples

### 🖥️ Display System

The 160×80 IPS display is driven over **SPI1 + DMA**. The framebuffer `lcd_frame_buffer[sizeof(uint16_t) * 160 * 80 + 4]` is flushed via a single DMA transfer.

**Core API** (from `lcd.h` & `lcd_driver.h`):

```c
// ── Basic Drawing ──
void lcd_fill_screen(uint16_t color);              // Fill entire screen
void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2,
                   uint16_t y2, uint16_t color);
void lcd_draw_rectangle(uint16_t x1, uint16_t y1,
                        uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x0, uint16_t y0,
                     uint8_t r, uint16_t color);
void lcd_draw_string(int16_t x, int16_t y, uint16_t fc,
                     uint16_t bc, uint8_t sizey, const char *p);

// ── DMA-Accelerated Drawing ──
void lcd_screen_update_dma(void);                  // Full-screen DMA flush
void lcd_dma_draw_filled_rect(int16_t x, int16_t y,
                              int16_t w, int16_t h, uint16_t color);
void lcd_dma_draw_label(const lcd_label_t *label);

// ── Picture & Video ──
void lcd_draw_picture_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height, uint32_t w25q_addr);
void lcd_play_video_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);

// ── Performance Monitoring ──
void lcd_calculate_fps(void);       // DWT clock-cycle-based FPS
void lcd_calculate_usage(void);     // CPU usage percentage

extern uint16_t lcd_fps;            // Current FPS
extern uint8_t  cpu_usage_percent;  // CPU usage %
```

**Usage Example**:

```c
// Draw a scene with circle, rectangle, and text
lcd_fill_screen(BLACK);
lcd_draw_string(10, 10, WHITE, BLACK, 8, "STM IPS");
lcd_draw_circle(80, 40, 30, RED);
lcd_draw_rectangle(20, 20, 60, 60, GREEN);
lcd_draw_line(0, 0, LCD_W - 1, LCD_H - 1, BLUE);
lcd_screen_update_dma();            // DMA flush

// Performance data (call periodically in the main loop)
lcd_calculate_fps();
lcd_calculate_usage();
// Read: lcd_fps, cpu_usage_percent

// Adjust backlight brightness
set_lcd_brightness(128);  // 0~255
```

**Color Constants**:

| Name | Value | Name | Value |
|:---|:---:|:---|:---:|
| `WHITE` | `0xFFFF` | `BLACK` | `0x0000` |
| `RED` | `0xF800` | `GREEN` | `0x07E0` |
| `BLUE` | `0x001F` | `CYAN` | `0x7FFF` |
| `YELLOW` | `0xFFE0` | `MAGENTA` | `0xF81F` |
| `GRAY` | `0x8430` | `DARKBLUE` | `0x01CF` |

> **Design Notes**:
> - Single-buffer architecture, `lcd_frame_buffer[160*80 + 4]`, designed for future double-buffer upgrade
> - USB stream mode (`LCD_USB_STREAM_ENABLE`): waits for USB TX completion before rendering to prevent single-buffer tearing
> - FPS counter uses DWT clock cycle for precise measurement without an extra timer
> - Backlight brightness controlled via PWM on PA3 (TIM)

---

### 🎞️ Animation Engine

A complete animation framework supporting up to **16 concurrent animations** and **16 render layers**. Easing paths, execution callbacks, and completion callbacks are fully pluggable.

**Core Structs** (from `lcd_driver.h`):

```c
// ── Animation Configuration ──
typedef struct {
    void *target;                    // Pointer to target variable
    int32_t start_value;             // Start value
    int32_t end_value;               // End value
    uint32_t duration_ms;            // Duration (ms)
    uint32_t delay_ms;               // Start delay (ms)
    bool repeat;                     // Repeat flag
    bool yoyo;                       // Yo-yo (reverse at end)
    lcd_anim_exec_cb_t exec_cb;      // Execution callback (update target per frame)
    lcd_anim_done_cb_t done_cb;      // Completion callback
    lcd_anim_path_cb_t path_cb;      // Easing function (NULL = linear)
} lcd_anim_config_t;

// ── Layer Types ──
typedef struct { int16_t x, y, w, h; uint16_t color; } lcd_rect_t;
typedef struct { int16_t x, y; uint8_t radius; uint16_t color; } lcd_circle_t;
typedef struct { int16_t x, y; uint16_t fg_color, bg_color;
                 uint8_t size; const char *text; } lcd_label_t;
typedef struct { int16_t x, y, width, height; uint32_t addr; } lcd_picture_t;
typedef struct { int16_t x, y, width, height;
                 uint32_t start_addr, end_addr; } lcd_video_t;
```

**API Quick Reference**:

```c
// Animation Manager
void lcd_anim_manager_init(void);
void lcd_anim_manager_set_bg(uint16_t color);
void lcd_anim_manager_task(void);       // Call in main loop - update all animations
void lcd_anim_manager_render(void);     // Call in main loop - render all layers

// Layer Management
int8_t lcd_anim_manager_add_layer(void *ctx, lcd_layer_draw_cb_t draw_cb);
bool   lcd_anim_manager_remove_layer(int8_t layer_id);
void   lcd_anim_manager_clear_layers(void);

// Animation Control
int8_t lcd_anim_start(const lcd_anim_config_t *config);
bool   lcd_anim_stop(int8_t anim_id);
void   lcd_anim_stop_all(void);

// Easing Paths
lcd_anim_path_cb_t lcd_anim_get_path(lcd_anim_ease_t ease);
```

**Easing Functions** (14 total):

| Enum | Effect |
|:---|:---|
| `LCD_ANIM_EASE_LINEAR` | Linear |
| `LCD_ANIM_EASE_IN_QUAD` | Quad ease-in |
| `LCD_ANIM_EASE_OUT_QUAD` | Quad ease-out |
| `LCD_ANIM_EASE_IN_OUT_QUAD` | Quad ease-in-out |
| `LCD_ANIM_EASE_IN_SINE` | Sine ease-in |
| `LCD_ANIM_EASE_OUT_SINE` | Sine ease-out |
| `LCD_ANIM_EASE_IN_OUT_SINE` | Sine ease-in-out |
| `LCD_ANIM_EASE_IN_EXPO` | Exponential ease-in |
| `LCD_ANIM_EASE_OUT_EXPO` | Exponential ease-out |
| `LCD_ANIM_EASE_IN_OUT_EXPO` | Exponential ease-in-out |
| `LCD_ANIM_EASE_IN_CIRC` | Circular ease-in |
| `LCD_ANIM_EASE_OUT_CIRC` | Circular ease-out |
| `LCD_ANIM_EASE_IN_OUT_CIRC` | Circular ease-in-out |
| `LCD_ANIM_EASE_IN_OUT_BACK` | Back ease-in-out (overshoots then rebounds) |
| `LCD_ANIM_EASE_OUT_ELASTIC` | Elastic ease-out (bounces at end) |

**Usage Example**:

```c
// ── Example 1: Rect sliding back and forth on X axis ──
static lcd_rect_t g_rect = {10, 30, 28, 15, CYAN};

int8_t layer_id = lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);

lcd_anim_config_t anim = {
    .target       = &g_rect.x,
    .start_value  = 0,
    .end_value    = LCD_W - 28,
    .duration_ms  = 1300,
    .repeat       = true,
    .yoyo         = true,
    .exec_cb      = lcd_anim_exec_set_i16,
    .path_cb      = lcd_anim_get_path(LCD_ANIM_EASE_IN_OUT_SINE),
};
lcd_anim_start(&anim);

// ── Example 2: Text label with elastic bounce ──
static lcd_label_t g_label = {40, 35, WHITE, BLACK, 8, "STM IPS"};

lcd_anim_manager_add_layer(&g_label, lcd_dma_draw_label);

lcd_anim_config_t bounce = {
    .target       = &g_label.x,
    .start_value  = 40,
    .end_value    = 100,
    .duration_ms  = 800,
    .repeat       = true,
    .yoyo         = true,
    .exec_cb      = lcd_anim_exec_set_i16,
    .path_cb      = lcd_anim_get_path(LCD_ANIM_EASE_OUT_ELASTIC),
};
lcd_anim_start(&bounce);

// ── Update (call in main loop) ──
// lcd_anim_manager_task();    // update animation state
// lcd_anim_manager_render();  // render all layers to framebuffer
```

> **Design Notes**:
> - Core implementation in `lcd_driver.c` using static `s_anim_slots[]` and `s_layer_slots[]` arrays
> - Each frame: `lcd_anim_manager_task()` updates progress, `lcd_anim_manager_render()` draws layers in order
> - Internal easing uses Q10 fixed-point (`lcd_anim_mix_q10()`) to avoid floating-point overhead
> - Supports `delay_ms`, `repeat`, and `yoyo` for complex animation patterns
> - Max animations and layers configurable via `LCD_ANIM_MAX_COUNT` and `LCD_LAYER_MAX_COUNT`

---

### 🎬 MJPEG Video Playback

Uses the **picojpeg** library to software-decode JPEG frames on the STM32F401, reading from W25Q Flash and decoding each frame to RGB565 into the framebuffer.

**File Format**:

```
Header (14 Bytes):
  [0..3]  Magic "MJPG" (0x47504A4D LE)
  [4..5]  frame_count (uint16 LE)
  [6..7]  width        (uint16 LE)
  [8..9]  height       (uint16 LE)
  [10..13] reserved

Body (repeats frame_count times):
  [frame_size (uint32 LE)] [JPEG data (frame_size bytes)]
```

**Core API** (from `lcd_mjpeg.h`):

```c
// Decode state (can be inspected via UI diagnostics)
typedef struct {
    uint8_t  active;                    // Active flag
    int16_t  width;                     // Image width
    int16_t  height;                    // Image height
    uint32_t start_addr;                // W25Q file start address
    uint32_t end_addr;                  // W25Q file end address
    uint16_t frame_count;               // Total frame count
    uint32_t cur_frame_idx;             // Current frame index
    uint32_t frame_size;                // Current frame JPEG data size
    int16_t  lcd_x;                     // LCD X offset
    int16_t  lcd_y;                     // LCD Y offset
    int8_t   last_error;                // Last error code
    uint8_t  pjpeg_ret;                 // picojpeg return value
    uint8_t  frame_dump[44];            // First 44 bytes of frame data (debug)
} mjpeg_state_t;

// ── Play MJPEG video ──
// Decodes one frame per call. Auto-loops after the last frame.
void lcd_play_mjpeg_video(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);

// ── Diagnostics ──
int8_t lcd_mjpeg_last_error(void);
const mjpeg_state_t *lcd_mjpeg_get_state(void);
```

**Decode Flow**:

1. Read 14-byte header, verify Magic `0x47504A4D`
2. Read 4-byte frame size prefix + JPEG data
3. Use a **512-byte smart read cache** from W25Q (cache hits skip Flash reads)
4. Call `pjpeg_decode_mcu()` per MCU to decode
5. Write RGB565 pixels into `lcd_write_ptr` framebuffer
6. Auto-loop from start after the last frame

**Usage Example**:

```c
// Play MJPEG video from W25Q (fullscreen loop)
lcd_play_mjpeg_video(0, 0, 160, 80,
    video_info.start_sector * 4096,                   // start address
    video_info.start_sector * 4096 + video_info.size  // end address
);

// Check decode state
const mjpeg_state_t *state = lcd_mjpeg_get_state();
if (state->last_error != 0) {
    switch (state->last_error) {
        case MJPEG_ERR_BAD_MAGIC:  // 254 — invalid MJPEG file
        case MJPEG_ERR_DECODE_INIT: // 251 — decode init failed
        case MJPEG_ERR_ZERO_FRAMES: // 253 — zero frames
            break;
    }
}
```

> **Design Notes**:
> - Error codes defined in `lcd_mjpeg.h`: `MJPEG_ERR_DMA_BUSY(255)`, `MJPEG_ERR_BAD_MAGIC(254)`, etc.
> - `mjpeg_state_t` fully exposed to the UI layer for diagnostic display
> - First 44 bytes of each frame dumped into the state struct for serial debugging
> - 512-byte read cache reduces W25Q SPI reads, improving decode performance

---

### 💾 Storage Manager

A lightweight embedded filesystem running on **W25Q128 (16MB SPI Flash) + AT24C64 (8KB EEPROM)**. Small files use byte-level packing; large files use 4KB sector-aligned allocation.

**FAT Structure** (stored in AT24C64, from `storage_manager.c`):

```c
#define FAT_MAGIC_NUMBER 0x0D000721
#define W25Q_SECTOR_SIZE 4096
#define MAX_FILENAME_LEN 16
#define MAX_SMALL_FILES 32
#define MAX_LARGE_FILES 32

typedef struct {
    uint32_t magic;                    // Magic number 0x0D000721
    uint32_t small_next_addr;          // Small file area next alloc addr
    uint16_t small_file_count;
    small_file_info_t small_files[32];
    uint32_t large_next_sector;        // Large file area next alloc sector
    uint16_t large_file_count;
    large_file_info_t large_files[32];
} storage_fat_t;
```

**Core API** (from `storage_manager.h`):

```c
bool storage_manager_init(void);
void storage_manager_task(void);

int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);
bool get_small_file_info(uint8_t file_id, small_file_info_t *info);
bool get_large_file_info(uint8_t file_id, large_file_info_t *info);
void clear_large_file(void);
void clear_small_file(void);
```

**Usage Example**:

```c
// ── Find and read a large file ──
int16_t idx = find_large_file_by_name("pic_mp");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    // Start address = info.start_sector * 4096
    // File size = info.size (bytes)
    // Display image: lcd_draw_picture_from_w25q(0, 0, 160, 80,
    //     info.start_sector * 4096);
}

// ── Find a video file ──
int16_t vid = find_large_file_by_name("qwq");
if (vid >= 0) {
    large_file_info_t vinfo;
    get_large_file_info((uint8_t)vid, &vinfo);
    // Play MJPEG: lcd_play_mjpeg_video(0, 0, 160, 80,
    //     vinfo.start_sector * 4096,
    //     vinfo.start_sector * 4096 + vinfo.size);
}
```

**File Type Comparison**:

| Feature | Small File | Large File |
|:---|:---|:---|
| Allocation granularity | Byte-level | 4KB sector |
| Max count | 32 | 32 |
| Use case | Text, config | Image, video |
| Fragmentation on delete | Allowed | None (bitmap clear) |
| Filename length | ≤ 16 bytes | ≤ 16 bytes |

> **Design Notes**:
> - Partition map in `storage_manager.c`: Reserved(0~1) → Small File(2~63, 248KB) → Large File(64~4031, 15.5MB) → User(4032~4095, 256KB)
> - FAT persisted in AT24C64, saved via `storage_fat_save()` after each file operation
> - USB protocol commands are parsed by `storage_manager_task()` in the main loop

---

### 🔌 USB Controller

A custom protocol over **USB CDC (Virtual COM Port)**, using a single-slot pending-send mechanism to conserve SRAM.

**Core API** (from `usb_controller.h`):

```c
// ── Init & Main Loop ──
void usb_controller_init(usb_controller_t *controller);
void usb_controller_task(usb_controller_t *controller);

// ── Transmit ──
// cmd: command byte, data: payload ptr, len: payload length
// Returns: USB_SEND_OK / USB_SEND_QUEUED / USB_SEND_DROPPED_PREVIOUS
usb_send_status_t usb_controller_send(
    usb_controller_t *controller,
    uint8_t cmd, const uint8_t *data, uint16_t len);

// ── Receive ──
uint16_t usb_controller_receive(
    usb_controller_t *controller,
    uint8_t *buf, uint16_t len);
uint16_t usb_controller_get_rx_free_space(void);

// ── HAL Callback Bridge ──
void usb_controller_on_tx_complete(void);
void usb_controller_on_rx_received(uint8_t *buf, uint32_t len);
```

**Usage Example**:

```c
// ── Send data to host ──
uint8_t response[] = {0xA0, 0x00, 0x00};  // Success response
usb_controller_send(&g_usb_controller,
    0xA0, response, sizeof(response));
// Returns USB_SEND_OK or USB_SEND_QUEUED

// ── Main loop USB task ──
// while(1) {
//     usb_controller_task(&g_usb_controller);
// }
```

**Key Design Decisions**:

| Feature | Description |
|:---|:---|
| **Single-slot pending** | Pins pointer, not copies payload — saves 64KB SRAM |
| **Ring buffer RX** | 2560 bytes, covers max protocol frame (2055B) + margin |
| **Send fragmentation** | 6144 bytes per call (`USB_SEND_BYTES_PER_CALL`) |
| **Timeout protection** | 2s (`USB_TX_STUCK_TIMEOUT_MS`) soft recovery |
| **LCD stream sync** | Render waits for USB TX completion to prevent tearing |

**Protocol Frame Format**:

```
┌─────────┬─────────┬──────────┬─────────────┬──────────────────┐
│  0xAA   │  0x55   │ Command  │  Length      │  Payload + CRC16 │
│ (1 Byte)│ (1 Byte)│ (1 Byte) │ (2 Byte LE)  │   (Variable)     │
└─────────┴─────────┴──────────┴─────────────┴──────────────────┘
```

---

### 🔗 SPI DMA Transfer

SPI1 drives the LCD display, SPI2 communicates with the W25Q Flash — both support DMA transfer.

**Core API** (from `w25q_controller.h`):

```c
// SPI Flash Commands
#define W25Q_WriteEnable     0x06
#define W25Q_PageProgram     0x02
#define W25Q_SectorErase     0x20   // 4KB
#define W25Q_BlockErase64K   0xD8   // 64KB
#define W25Q_ChipErase       0xC7
#define W25Q_ReadData        0x03
#define W25Q_FastReadData    0x0B
#define W25Q_JedecDeviceID   0x9F

// ── Basic R/W ──
bool w25q_init(void);
void w25q_read_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_write_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_erase_sector(uint32_t address);
uint32_t w25q_read_id(void);

// ── DMA Transfer ──
bool w25q_read_data_dma(uint32_t address, uint8_t *data, uint32_t size);
bool w25q_write_data_dma(uint32_t address, uint8_t *data, uint32_t size);
bool w25q_fast_read_data_dma(uint32_t address, uint8_t *data, uint32_t size);
void w25q_dma_task(void);              // DMA state machine (call in main loop)
bool w25q_dma_is_busy(void);
bool w25q_dma_is_done(void);
bool w25q_dma_is_error(void);
```

**Usage Example**:

```c
// ── Initialize W25Q ──
if (!w25q_init()) {
    lcd_draw_string(10, 10, RED, BLACK, 8, "W25Q FAIL");
}

// ── Sector erase & write ──
uint8_t data[256] = { /* ... */ };
w25q_erase_sector(64 * 4096);          // Erase sector 64
w25q_write_data(64 * 4096, data, 256);

// ── DMA read ──
uint8_t buf[256];
w25q_fast_read_data_dma(64 * 4096, buf, sizeof(buf));

// ── DMA state machine in main loop ──
// while (1) {
//     w25q_dma_task();  // handle DMA completion/error callbacks
// }
```

> **Design Notes**:
> - `w25q_check_busy()` waits for W25Q internal operation with timeout
> - DMA transfers managed by `w25q_dma_task()` state machine polled in main loop
> - `W25Q_CS_LOW()` / `W25Q_CS_HIGH()` macros directly control chip-select GPIO
> - SPI handles: `hspi1` (LCD) and `hspi2` (W25Q), defined in `spi.h`

---

## 📡 USB Communication Protocol

### Command Set

| Command | Code | Direction | Description |
|:---|:---:|:---|:---|
| **Start Large Download** | `0x11` | Host → Device | Transfer image/video data |
| **Start Small Download** | `0x45` | Host → Device | Transfer text/small data |
| **End Download** | `0x14` | Host → Device | File transfer complete, register to FAT |
| **Delete File** | `0x19` | Host → Device | Delete by file type + index |
| **Query File List** | `0x20` | Host → Device | Query FAT file directory |
| **Success Response** | `0xA0` | Device → Host | Operation successful |
| **Error Response** | `0xE0` | Device → Host | Transfer error notification |

### Transfer Flow

```
Host                                    Device
  │                                        │
  ├── [0x11] Start Large Download ──────→  │
  ├── [0x11] Data Frame + CRC16 ────────→  │
  ├── [0x11] Data Frame + CRC16 ────────→  │
  │       ......                           │
  ├── [0x14] End Download (Name+CRC) ──→   │
  │                                        ├── Register to FAT
  │                                        ├── Save to EEPROM
  │                                        │
  │  ← ── [0xA0] OK / [0xE0] Error ────── │
```

### USB Communication Example

```c
// ── Host sends command frame ──
// Frame: 0xBB 0x44 [CMD] [LEN_LO] [LEN_HI] [PAYLOAD...] [CRC16_LO] [CRC16_HI]
//
// Start Large Download:
// BB 44 11 04 00 31 32 89 C6
//   └─ Command: 0x11 (Start Large Download)
//      └─ Length: 0x0004 (2 bytes data + 2 bytes CRC16)
//         └─ Data: 0x31 0x32
//            └─ CRC16: 0xC689 (little-endian)
//
// Query File List:
// BB 44 20 02 00 2B BE
//
// Delete File (type=0, index=5):
// BB 44 19 04 00 00 05 DF CD
```

---

## 🗂️ File System Design

```
W25Q128 (16MB) Partition Layout
═══════════════════════════════════════════
Sector 0-1     │ Reserved (8KB)
───────────────┼────────────────────────────
Sector 2-63    │ Small File Area (248KB)
               │ Byte-level packing
───────────────┼────────────────────────────
Sector 64-4031 │ Large File Area (15.5MB)
               │ 4KB sector-aligned bitmap
───────────────┼────────────────────────────
Sector 4032-4095│ User Area (256KB)
═══════════════════════════════════════════

AT24C64 (8KB EEPROM) Usage
═══════════════════════
Address 0x0000 → storage_fat_t (File Allocation Table)
```

---

## 🛠️ Host Tools

### 🌐 Web Host — `lcd_host_web/`

A modern Flask-based web application for transcoding and managing image/video content.

**Features**:

| Feature | Description |
|:---|:---|
| Image transcoding | PNG/JPG/BMP/GIF → RGB565 / BL compressed |
| Video transcoding | MP4/WEBM/MKV/AVI/MOV → MJPEG / BL compressed |
| BL block compression | 4×4 pixel blocks, 2 base + 2 interpolated colors, 2-bit index |
| Preview generation | Real-time preview of conversion results |
| Parameter tuning | Resolution, frame rate, brightness, quality, endianness |
| Auto cleanup | 5-minute TTL automatic deletion |
| Dark/Light theme | One-click switch |

```bash
cd lcd_host_web
pip install -r requirements.txt
python server.py
# Open http://localhost:5000 in browser
```

**API Endpoints**:

| Route | Method | Purpose |
|:---|:---:|:---|
| `/convert` | POST | Upload & convert to STM IPS format |
| `/preview/<id>` | GET | Get preview of converted file |
| `/download/<id>` | GET | Download converted binary file |
| `/info/<id>` | GET | Get conversion metadata |

### 🔧 Feature Tester — `feature_tester/`

Development & debugging tools:

| Tool | Description |
|:---|:---|
| `sender.c` | Windows serial sender — sends data and verifies echo byte-by-byte |
| `receiver.c` | Windows serial receiver — receives LCD frame stream and renders in window |
| `image_decoder.py` | Python tool — decodes RGB565 binary to PNG, supports endianness selection |

---

## 🔧 Environment Setup

### Hardware Requirements

- **STM32F401RCT6** dev board
- **160×80 IPS TFT** display (ST7735S-compatible)
- **W25Q64** SPI Flash module
- **AT24C64** EEPROM module
- USB data cable (power + CDC communication)
- Optional: Encoder, LEDs, Button

### Toolchain

| Tool | Version |
|:---|:---|
| STM32CubeIDE | ≥ 1.15 (recommended) / Makefile also supported |
| ARM GCC | arm-none-eabi-gcc ≥ 10.3 |
| Python | ≥ 3.9 |
| FFmpeg | ≥ 5.0 (required by Web Host) |
| OpenOCD | Optional (for flashing) |

### Firmware Build

```bash
# Option 1: STM32CubeIDE
# Open stm_ips.ioc → Generate Code → Build All

# Option 2: Makefile
cd Debug && make -j4
# Output: stm_ips.elf / stm_ips.bin
```

### Flashing

```bash
# OpenOCD (ST-Link)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program Debug/stm_ips.elf verify reset exit"

# STM32CubeProgrammer
STM32_Programmer_CLI -c port=SWD -w Debug/stm_ips.elf -rst
```

---

## 🚀 Quick Start

### Step 1: Flash Firmware

Flash the compiled `stm_ips.elf` onto the STM32F401RC board.

### Step 2: Power On

On power-on, the system automatically:

1. Initializes all peripherals (SPI · I2C · USB · Timers)
2. Detects and initializes W25Q Flash
3. Loads FAT file allocation table from AT24C EEPROM
4. Starts the LCD UI animation engine
5. Enters the main loop, continuously updating USB and storage management tasks

### Step 3: Upload Content

**Via Web Host**:
```bash
python lcd_host_web/server.py
# Open http://localhost:5000
# Drag image/video → Adjust resolution/quality → Convert → Download
```

### Step 4: Playback

Firmware looks for the following filenames in W25Q on startup:

- `pic_mp` → Decode & display image
- `qwq` → Play MJPEG video (looping)

> Customize startup content by modifying filenames in `lcd_ui.c` and logic in `main.c`.

---

## 👨‍💻 Development Guide

### Adding a New UI Element

```c
// 1️⃣ Declare element instance
static lcd_rect_t g_my_rect = {10, 30, 20, 15, CYAN};

// 2️⃣ Register with animation manager
int8_t layer_id = lcd_anim_manager_add_layer(&g_my_rect, lcd_draw_rect_layer);

// 3️⃣ Add animation
lcd_anim_config_t anim = {
    .target      = &g_my_rect.x,
    .start_value = 10,
    .end_value   = LCD_W - 20,
    .duration_ms = 2000,
    .repeat      = true,
    .yoyo        = true,
    .exec_cb     = lcd_anim_exec_set_i16,
    .path_cb     = lcd_anim_get_path(LCD_ANIM_EASE_OUT_ELASTIC),
};
lcd_anim_start(&anim);
```

### File System Operations

```c
// Find large file by name
int16_t idx = find_large_file_by_name("my_image");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    // Start addr = info.start_sector * 4096
    // Size = info.size
    // Display image: lcd_draw_picture_from_w25q(0, 0, 160, 80,
    //     info.start_sector * 4096);
}

// Clear all files (use with caution — recommended via USB protocol)
// clear_large_file();
// clear_small_file();
```

### CRC16 Checksum

```c
// Initialize CRC16 lookup table
crc16_usb_init_table();

// Calculate CRC16 for USB protocol frame
// has_crc = true means data already has 2-byte CRC placeholder
uint16_t crc = crc16_usb_packing(data, len, false);

// Append to frame (little-endian)
// data[len] = crc & 0xFF;
// data[len+1] = (crc >> 8) & 0xFF;
```

### System Initialization Sequence

```c
// Complete initialization flow in main()
void main(void)
{
    HAL_Init();
    SystemClock_Config();            // 84MHz HSE + PLL

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_I2C1_Init();
    MX_SPI1_Init();                  // LCD SPI
    MX_SPI2_Init();                  // W25Q SPI
    MX_USB_DEVICE_Init();            // USB CDC
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_TIM9_Init();
    MX_TIM4_Init();

    // Application Init
    usb_controller_init(&g_usb_controller);
    lcd_init();

    bool w25q_ok = w25q_init();
    bool storage_init_ok = storage_manager_init();

    // clear_large_file();           // Optional: clear all files
    // clear_small_file();

    lcd_ui_init();

    while (1) {
        lcd_ui_updater();
        w25q_dma_task();
        storage_manager_task();
        usb_controller_task(&g_usb_controller);
    }
}
```

### Performance Tips

- **DMA Preferred**: Use `lcd_screen_update_dma()` instead of polling — frees the CPU
- **Buffer Strategy**: With `LCD_USB_STREAM_ENABLE`, LCD renders only after USB TX completes to avoid tearing
- **Animation Count**: `LCD_ANIM_MAX_COUNT` defaults to 16; reduce to save RAM and CPU
- **SPI Clock**: Adjust SPI1/SPI2 clock in MX config for higher frame rates and Flash throughput
- **Decode Cache**: MJPEG uses 512-byte smart read cache; adjust size to balance RAM vs. performance

---

## 📜 License

Copyright (C) 2026 **UnikoZera**

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.**

This program is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.

---

**Third-Party Components**:

| Component | License | Author |
|:---|:---|:---|
| **picojpeg** | Public domain | [Rich Geldreich](https://github.com/richgel999/picojpeg) |
| **STM32 HAL** | STMicroelectronics SLA | STMicroelectronics |
| **CMSIS** | Apache 2.0 | ARM |

> This project is based on the STM32CubeMX generated framework.

---

<p align="center">
  <sub>Built with by UnikoZera · STM32F401RC · 160×80 IPS · 2026</sub>
</p>
