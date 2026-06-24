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
- [License](#-license)

---

## 🔭 System Overview

**STM IPS** is an embedded image processing & multimedia system running on an STM32F401RC (Cortex-M4, 84MHz). It drives a **160×80 IPS color TFT** display through **SPI DMA** for tear-free full-screen refresh, and packs:

- A custom **Animation Engine** — 15 easing functions, 16 concurrent animations, 16-layer render pipeline
- **picojpeg-based** MJPEG software decoder
- A **lightweight file system** — dual-zone allocation (byte-level small files / bitmap-managed large files), FAT persisted in EEPROM
- A **USB CDC custom protocol** — file transfer, LCD live streaming, and device diagnostics

The project spans firmware to host tools across four sub-projects:

| Component | Tech Stack | Purpose |
|:---|:---|:---|
| `Core/` | C (STM32 HAL) | Firmware: LCD driver, animation engine, video decode, storage mgmt, USB protocol |
| `USB_DEVICE/` | C (STM32 USB Device Lib) | USB CDC virtual COM port layer |
| `lcd_host_web/` | Python Flask + HTML5 | Web-based image/video transcoding & file management |
| `feature_tester/` | C + Python | Serial loopback test / RGB565 decode verification |

For the Chinese version, see **[README.md](README.md)**.

---

## ⚙️ Hardware Architecture

### MCU

| Parameter | Spec |
|:---|:---|
| **MCU** | STM32F401RCT6, ARM Cortex-M4 FPU, 84MHz |
| **Flash** | 256KB (internal) |
| **SRAM** | 64KB |
| **Package** | LQFP64 |

### Peripheral Connections

```
┌──────────────────────────────────────────────────────────────────┐
│                         STM32F401RC                              │
│                                                                  │
│   SPI1 ── DMA2 Stream3 ───→ ST7735S 160×80 IPS LCD               │
│   ├─ SCK(PA5) · MISO(PA6) · MOSI(PA7)                           │
│   ├─ RES(PB0) · DC(PB1) · CS(PB2)                               │
│   └─ BL_PWM(PA3, TIM9_CH2)                                      │
│                                                                  │
│   SPI2 ── DMA1 Stream3(RX) / Stream4(TX) ──→ W25Q128 16MB Flash │
│   ├─ SCK(PB13) · MISO(PB14) · MOSI(PB15) · CS(PA8)              │
│   └─ 4096 sectors × 4KB, 256B page program                      │
│                                                                  │
│   I2C1 ───────────────────────→ AT24C64 8KB EEPROM               │
│   ├─ SCL(PB8) · SDA(PB9)                                        │
│   └─ 32B page, I2C addr 0xA0, stores FAT                        │
│                                                                  │
│   USB_OTG_FS ─────────────────→ USB CDC Virtual COM Port         │
│   └─ EN(PB12)                                                   │
│                                                                  │
│   TIM2 · TIM3 · TIM4 · TIM9 ── PWM / Timing                     │
│   (Unused) Encoder PA0/PA1 · LED PC13/PC14/PA15 · Button PC15    │
└──────────────────────────────────────────────────────────────────┘
```

### Storage Layout

| Region | Sectors | Size | Allocation Strategy |
|:---|:---:|:---:|:---|
| **Reserved** | 0 ~ 1 | 8 KB | Compaction scratch |
| **Small File** | 2 ~ 63 | 248 KB | Byte-level linear squeeze + compaction |
| **Large File** | 64 ~ 4031 | 15.5 MB | 4KB sector-aligned bitmap |
| **User** | 4032 ~ 4095 | 256 KB | User-defined |

> W25Q128: 16MB total. FAT (File Allocation Table) persisted in AT24C64 EEPROM at address `0x0000`.

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
    │picojpeg │ │ CRC16  │ │ AT24C    │
    │ MJPEG   │ │        │ │ (EEPROM) │
    │ BL Dec  │ │        │ │          │
    └─────────┘ └────────┘ └──────────┘

         STM32 HAL / SPI / I2C / DMA / TIM / DWT
```

### Main Loop Flow

```
main()
 ├── HAL_Init() + SystemClock_Config()       // 84MHz HSE + PLL
 ├── Peripheral init:
 │     MX_GPIO_Init() · MX_DMA_Init()
 │     MX_I2C1_Init() · MX_SPI1_Init() · MX_SPI2_Init()
 │     MX_USB_DEVICE_Init() · MX_TIMx_Init()
 │
 ├── Application init:
 │     usb_controller_init()   // USB transceiver
 │     lcd_init()              // ST7735 init sequence
 │     w25q_init()             // W25Q128 JEDEC ID check
 │     storage_manager_init()  // Load FAT from AT24C
 │     lcd_ui_init()           // UI layers + animation start
 │
 └── while(1)  // ~60 FPS @ 84MHz
      ├── lcd_ui_updater()          // Update UI + animation + render
      ├── w25q_dma_task()           // SPI Flash DMA state machine
      ├── storage_manager_task()    // Parse USB host command frames
      └── usb_controller_task()     // USB TX queue + timeout recovery
```

---

## ✨ Core Features

### 🖥️ Display System

The 160×80 IPS display is driven over **SPI1 + DMA2 Stream3** with a single-buffer architecture (tail bytes reserved for dual-buffer migration).

**Orientation**: Configured via `USE_HORIZONTAL` (0=portrait, 1=portrait+BGR, 2=landscape, 3=landscape+BGR). Default landscape: `LCD_W=160` × `LCD_H=80`.

```c
// ── Basic Drawing ──
void lcd_fill_screen(uint16_t color);
void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2,
                   uint16_t y2, uint16_t color);   // Bresenham
void lcd_draw_rectangle(uint16_t x1, uint16_t y1,
                        uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x0, uint16_t y0,
                     uint8_t r, uint16_t color);
void lcd_draw_string(int16_t x, int16_t y, uint16_t fc,
                     uint16_t bc, uint8_t sizey, const char *p);

// ── DMA-Accelerated (buffer → SPI DMA → LCD) ──
void lcd_screen_update_dma(void);                  // Single DMA full-screen flush
void lcd_draw_point_dma(int16_t x, int16_t y, uint16_t color);
void lcd_draw_point_dma_swapped(int16_t x, int16_t y, uint16_t color);
void lcd_fill_screen_dma(uint16_t color);
void lcd_set_area_color(int16_t x, int16_t y, int16_t w,
                        int16_t h, uint16_t color);
void lcd_draw_char(int16_t x, int16_t y, const char ch,
                   uint16_t fc, uint16_t bc, uint8_t sizey);
void lcd_draw_picture_dma(int16_t x, int16_t y,
                          int16_t width, int16_t height,
                          const uint16_t *data);

// ── W25Q-backed rendering ──
void lcd_draw_picture_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height, uint32_t w25q_addr);
void lcd_play_video_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_addr, uint32_t file_size);
// ↑ Auto-detects format: MJPEG(magic "MJPG") / BL compressed(magic "BL") / raw RGB565

// ── USB Live Streaming ──
// When lcd_usb_stream_enabled=true, every DMA update also sends the
// framebuffer via USB (command 0xA0), throttled at 30ms.
// Tail sync bytes: {0x0D, 0x00, 0x07, 0x21}

// ── Hardware Performance Counters ──
void lcd_calculate_fps(void);       // DWT_CYCCNT-based frame rate
void lcd_calculate_usage(void);     // DWT cycle counter CPU load estimation

extern uint16_t lcd_fps;
extern uint8_t  cpu_usage_percent;
```

**Example**:

```c
lcd_fill_screen(BLACK);
lcd_draw_string(10, 10, WHITE, BLACK, 8, "STM IPS");
lcd_draw_circle(80, 40, 30, RED);
lcd_draw_rectangle(20, 20, 60, 60, GREEN);
lcd_draw_line(0, 0, LCD_W - 1, LCD_H - 1, BLUE);
lcd_screen_update_dma();

set_lcd_brightness(128);  // PWM 0~255
```

**Predefined Colors** (20 RGB565 constants):

| Name | Value | Name | Value |
|:---|:---:|:---|:---:|
| `WHITE` | `0xFFFF` | `BLACK` | `0x0000` |
| `RED` | `0xF800` | `GREEN` | `0x07E0` |
| `BLUE` | `0x001F` | `CYAN` | `0x7FFF` |
| `YELLOW` | `0xFFE0` | `MAGENTA` | `0xF81F` |
| `GRAY` | `0x8430` | `DARKBLUE` | `0x01CF` |

> **Design Notes**: Single framebuffer `lcd_frame_buffer[160*80 + 4]`. `lcd_write_ptr` points into it; tail bytes are for USB stream sync. DMA busy flag prevents concurrent flush collisions.

---

### 🎞️ Animation Engine

A complete animation framework with **16 concurrent slots** × **16 render layers**. All easing functions, execution callbacks, and completion callbacks are pluggable. Q10 fixed-point arithmetic avoids FPU overhead.

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
    lcd_anim_exec_cb_t exec_cb;      // Per-frame update callback
    lcd_anim_done_cb_t done_cb;      // Completion callback
    lcd_anim_path_cb_t path_cb;      // Easing function (NULL = linear)
} lcd_anim_config_t;

// ── Layer Types ──
typedef struct { int16_t x, y, w, h; uint16_t color; } lcd_rect_t;
typedef struct { int16_t x, y; uint8_t r; uint16_t color; } lcd_circle_t;
typedef struct { int16_t x, y; uint16_t fg, bg;
                 uint8_t size; const char *text; } lcd_label_t;
typedef struct { int16_t x, y, w, h; uint32_t addr; } lcd_picture_t;
typedef struct { int16_t x, y, w, h;
                 uint32_t start, end; } lcd_video_t;
```

**API Quick Reference**:

```c
void  lcd_anim_manager_init(void);
void  lcd_anim_manager_set_bg(uint16_t color);
void  lcd_anim_manager_task(void);         // Advance all animations
void  lcd_anim_manager_render(void);       // Render layers → framebuffer → DMA

int8_t lcd_anim_manager_add_layer(void *ctx, lcd_layer_draw_cb_t draw_cb);
bool   lcd_anim_manager_remove_layer(int8_t id);
void   lcd_anim_manager_clear_layers(void);

int8_t lcd_anim_start(const lcd_anim_config_t *config);
bool   lcd_anim_stop(int8_t anim_id);
void   lcd_anim_stop_all(void);
```

**15 Easing Functions**:

| Enum | Effect |
|:---|:---|
| `LCD_ANIM_EASE_LINEAR` | Linear |
| `LCD_ANIM_EASE_IN_QUAD` / `OUT_QUAD` / `IN_OUT_QUAD` | Quadratic ease-in/out/in-out |
| `LCD_ANIM_EASE_IN_SINE` / `OUT_SINE` / `IN_OUT_SINE` | Sine ease-in/out/in-out |
| `LCD_ANIM_EASE_IN_EXPO` / `OUT_EXPO` / `IN_OUT_EXPO` | Exponential ease-in/out/in-out |
| `LCD_ANIM_EASE_IN_CIRC` / `OUT_CIRC` / `IN_OUT_CIRC` | Circular ease-in/out/in-out |
| `LCD_ANIM_EASE_IN_OUT_BACK` | Back ease-in-out (overshoots then rebounds) |
| `LCD_ANIM_EASE_OUT_ELASTIC` | Elastic ease-out (bounces at end) |

**Example**:

```c
static lcd_rect_t g_rect = {10, 30, 28, 15, CYAN};
lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);

lcd_anim_config_t anim = {
    .target      = &g_rect.x,
    .start_value = 0,
    .end_value   = LCD_W - 28,
    .duration_ms = 1300,
    .repeat      = true,
    .yoyo        = true,
    .exec_cb     = lcd_anim_exec_set_i16,
    .path_cb     = lcd_anim_get_path(LCD_ANIM_EASE_IN_OUT_SINE),
};
lcd_anim_start(&anim);
// In main loop: lcd_anim_manager_task() + lcd_anim_manager_render()
```

---

### 🎬 MJPEG Video Playback

Software JPEG decoding using the **picojpeg** library. A **1024B smart read cache** minimizes W25Q SPI reads. Reads from Flash → decodes MCU-by-MCU to RGB565 → writes to framebuffer.

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

**Decode Flow**:

1. Read 14B header, verify Magic
2. Read 4B frame size + JPEG data
3. Smart cache → `pjpeg_decode_mcu()` per 8×8 block
4. Inline `rgb565()` conversion → framebuffer
5. Auto-loop from start after last frame

```c
void lcd_play_mjpeg_video(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);
    // Decodes one frame per call, loops on completion

int8_t lcd_mjpeg_last_error(void);
const mjpeg_state_t *lcd_mjpeg_get_state(void);
```

**Error Codes**:

| Value | Constant | Meaning |
|:---:|:---|:---|
| 255 | `MJPEG_ERR_DMA_BUSY` | DMA busy, can't read |
| 254 | `MJPEG_ERR_BAD_MAGIC` | Not an MJPEG file |
| 253 | `MJPEG_ERR_ZERO_FRAMES` | Zero frames |
| 251 | `MJPEG_ERR_DECODE_INIT` | picojpeg init failed |
| 250 | `MJPEG_ERR_NOT_3_COMP` | Not 3-component JPEG |

---

### 📦 BL Compressed Video

A custom **Block-Local 4×4 lossy compression** format designed for embedded use. Each 4×4 block encodes: 2 base colors + 2 interpolated colors + 2-bit index. Significantly reduces storage vs. raw RGB565.

**Auto-detection**: `lcd_play_video_from_w25q()` reads the first 2 magic bytes and dispatches to BL decoder, MJPEG decoder, or raw RGB565 render path.

```c
void lcd_play_compressed_video_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);
```

---

### 💾 Embedded File System

A lightweight file system on **W25Q128 (16MB SPI Flash) + AT24C64 (8KB EEPROM)**. The FAT is persisted in EEPROM — survives unexpected power loss.

**Dual-Zone Design**:

| Feature | Small File Zone | Large File Zone |
|:---|:---|:---|
| Allocation | Byte-level linear squeeze | 4KB sector bitmap |
| Max files | 32 | 32 |
| Filename | ≤ 16 bytes | ≤ 16 bytes |
| Fragmentation | Auto-compaction (batched) | Freed on delete (bitmap clear) |
| Use case | Text, config, metadata | Image, MJPEG video |

**FAT Structure** (stored in AT24C64 at `0x0000`):

```c
#define FAT_MAGIC_NUMBER 0x0D000722
#define W25Q_SECTOR_SIZE 4096
#define MAX_SMALL_FILES  32
#define MAX_LARGE_FILES  32

typedef struct {
    uint32_t magic;                              // Magic 0x0D000722
    uint32_t small_next_addr;                    // Small zone next alloc addr
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    uint8_t  large_sector_bitmap[496];           // 3968 bits → one bit per sector
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
} storage_fat_t;
```

**Small File Compaction** (`compact_small_files`):

Auto-triggered when free space drops below `SMALL_FILE_COMPACT_THRESHOLD` (4KB):

1. Sort all valid files by address
2. Form batches of files sharing sectors (to avoid data loss on erase)
3. Copy batch to reserved area (Sector 0~1, 8KB)
4. Erase source sectors → read back from reserved → write compactly to zone start
5. Repeat → FAT persisted after every batch

```c
bool storage_manager_init(void);
void storage_manager_task(void);       // Poll in main loop, parse USB commands

bool storage_fat_load(void);
void storage_fat_save(void);           // Persist to AT24C

int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);
bool get_small_file_info(uint8_t id, small_file_info_t *info);
bool get_large_file_info(uint8_t id, large_file_info_t *info);

bool compact_small_files(void);        // Garbage collection
void clear_all_files(void);            // Format: erase all + reset FAT

bool storage_is_downloading(void);
```

**Usage Example**:

```c
// Find picture "pic_mp" → display
int16_t idx = find_large_file_by_name("pic_mp");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    lcd_draw_picture_from_w25q(0, 0, 160, 80,
        info.start_sector * W25Q_SECTOR_SIZE);
}

// Find video "qwq" → play
int16_t vid = find_large_file_by_name("qwq");
if (vid >= 0) {
    large_file_info_t vinfo;
    get_large_file_info((uint8_t)vid, &vinfo);
    lcd_play_video_from_w25q(0, 0, 160, 80,
        vinfo.start_sector * W25Q_SECTOR_SIZE, vinfo.size);
}
```

---

### 🔌 USB Controller

A custom protocol over **USB CDC (Virtual COM Port)** using a **single-slot pending-send** mechanism to conserve SRAM. RX uses a 2560B ring buffer; TX fragments at 6144B per call.

**Protocol Frame** (bidirectional):

```
┌─────────┬─────────┬──────────┬─────────────┬──────────────────┐
│  0xAA   │  0x55   │ Command  │  Length LE   │  Payload + CRC16 │
│ (1 Byte)│ (1 Byte)│ (1 Byte) │ (2 Byte)     │   (Variable)     │
└─────────┴─────────┴──────────┴─────────────┴──────────────────┘
```

```c
void usb_controller_init(usb_controller_t *ctl);
void usb_controller_task(usb_controller_t *ctl);

usb_send_status_t usb_controller_send(
    usb_controller_t *ctl, uint8_t cmd,
    const uint8_t *data, uint16_t len);

uint16_t usb_controller_receive(usb_controller_t *ctl,
    uint8_t *buf, uint16_t len);
uint16_t usb_controller_get_rx_free_space(void);
```

**Key Design Decisions**:

| Feature | Description |
|:---|:---|
| **Single-slot pending** | Pins pointer, doesn't copy — saves SRAM |
| **RX ring buffer** | 2560 bytes, covers max frame + margin |
| **TX fragmentation** | 6144 bytes/chunk, maintains LCD stream stability |
| **Timeout protection** | 2s no-progress triggers endpoint soft recovery |
| **LCD stream sync** | Render waits for USB TX completion to prevent tearing |
| **IRQ safety** | `__disable_irq()` guards ring buffer pointers |

---

### 🔗 SPI DMA Transport Layer

Both SPI1 (LCD) and SPI2 (W25Q Flash) support DMA. The W25Q driver includes a **10-state DMA state machine**: IDLE → WRITE_PENDING → STARTING → WAIT_TX_DONE → WAIT_FLASH_READY → DONE/ERROR.

```c
bool w25q_init(void);
uint32_t w25q_read_id(void);           // JEDEC ID 0x9F: 0xEF4018

// ── Synchronous ──
void w25q_read_data(uint32_t addr, uint8_t *data, uint32_t size);
void w25q_fast_read_data(uint32_t addr, uint8_t *data, uint32_t size);
void w25q_write_data(uint32_t addr, uint8_t *data, uint32_t size);
void w25q_page_program(uint32_t addr, uint8_t *data, uint16_t size);
void w25q_erase_sector(uint32_t addr);  // 4KB
void w25q_erase_chip(void);

// ── DMA Async ──
bool w25q_write_data_dma(uint32_t addr, uint8_t *data, uint32_t size);
bool w25q_read_data_dma(uint32_t addr, uint8_t *data, uint32_t size);
bool w25q_fast_read_data_dma(uint32_t addr, uint8_t *data, uint32_t size);

void w25q_dma_task(void);              // Poll in main loop
bool w25q_dma_is_busy(void);
bool w25q_dma_is_done(void);
bool w25q_dma_is_error(void);
```

**AT24C64 EEPROM** (I2C, no DMA):

```c
bool at24c_write_byte(uint16_t memAddr, uint8_t *data);
bool at24c_read_byte(uint16_t memAddr, uint8_t *data);
bool at24c_write_buffer(uint16_t memAddr, uint8_t *pData, uint16_t size);
bool at24c_read_buffer(uint16_t memAddr, uint8_t *pData, uint16_t size);
// Auto-handles 32B page writes + ready polling (ACK poll, max 30ms)
```

**CRC16-USB**:

```c
// CRC-16/USB: poly=0x8005, init=0xFFFF, final XOR=0xFFFF
uint16_t crc16_usb_packing(const uint8_t *data, uint16_t len, bool has_crc);
// has_crc=false → returns CRC16
// has_crc=true  → validates last 2 bytes as CRC, returns 1(pass)/0(fail)
```

---

### 📊 Performance Monitoring

Based on the **DWT_CYCCNT** hardware cycle counter built into Cortex-M4:

```c
void lcd_calculate_fps(void);       // Frames per second, no extra timer needed
void lcd_calculate_usage(void);     // CPU usage (idle vs. non-idle cycle ratio)

extern uint16_t lcd_fps;
extern uint8_t  cpu_usage_percent;
```

Typical performance at 84MHz:
- Full-screen DMA flush: ~0.3ms
- MJPEG decode & play: ~10-20 FPS (depends on compression ratio)
- UI animation rendering: ~60 FPS
- CPU idle: ~40-60% (includes USB polling + Flash state machine)

---

## 📡 USB Communication Protocol

### Host Command Frame Format

```
  [0][1]: Frame header 0xBB 0x44 (2B)
  [2]:   Command (1B)
  [3-6]: Total file size uint32 LE (4B) — first packet only; 0 for non-data cmds
  [7-8]: Packet length uint16 LE (2B) = payload_len + 2 (CRC16)
  [9+]:  Payload data
  [last-2][last-1]: CRC16 (2B) over header + payload (before CRC)
```

### Command Set

| Command | Code | Direction | Description |
|:---|:---:|:---|:---|
| **Start/Continue Large Download** | `0x11` | Host → Device | Image/video data, sector bitmap alloc |
| **Start/Continue Small Download** | `0x45` | Host → Device | Text/small data, linear byte alloc |
| **End Download** | `0x14` | Host → Device | Filename (≤16B), register to FAT |
| **Delete File** | `0x19` | Host → Device | file_type(1B) + file_index(1B) |
| **Query File List** | `0x20` | Host → Device | TLV-format directory + sector fragmentation |
| **Send Bitmap** | `0x21` | Host → Device | Returns large-zone bitmap (496B) |
| **LCD Stream Control** | `0x10` | Host → Device | sub_cmd=0x01 enable / 0x00 disable |
| **Continue** | `0xA1` | Device → Host | Download ack, host may send next packet |
| **Error** | `0xE0` | Device → Host | error_type(1B): CRC fail / no space / invalid idx |

### Transfer Flow

```
Host                                     Device
  │                                         │
  ├── [0x45] Data Frame + CRC16 ──────────→ │
  │                                         ├── Alloc, erase sector, write data
  │  ← ── [0xA1] Continue ──────────────── │
  ├── [0x45] Data Frame + CRC16 ──────────→ │
  │                                         ├── Expand alloc if needed, write
  │  ← ── [0xA1] Continue ──────────────── │
  ├── [0x14] End Download (filename+CRC) ─→ │
  │                                         ├── Register to FAT → EEPROM persist
  │  ← ── [0xA1] OK / [0xE0] Error ─────── │
```

---

## 🗂️ File System Design

```
W25Q128 (16MB) Partition Layout
═══════════════════════════════════════════
Sector    0 ~    1 (   8KB) │ Reserved (compaction scratch)
────────────────────────────┼────────────────────────────
Sector    2 ~   63 ( 248KB) │ Small File — byte-level linear squeeze
                            │ Auto-compaction on delete threshold
────────────────────────────┼────────────────────────────
Sector   64 ~ 4031 (15.5MB) │ Large File — 4KB sector-aligned bitmap
                            │ 496-byte bitmap manages 3968 sectors
────────────────────────────┼────────────────────────────
Sector 4032 ~ 4095 ( 256KB) │ User-defined
═══════════════════════════════════════════

AT24C64 (8KB EEPROM) Allocation
═══════════════════════
Address 0x0000 ──→ storage_fat_t (~2KB)
═══════════════════════
```

---

## 🛠️ Host Tools

### 🌐 Web Host — `lcd_host_web/`

A Flask-based web application for transcoding images/videos and managing device files.

| Feature | Description |
|:---|:---|
| Image transcoding | PNG/JPG/BMP/GIF → RGB565 / BL compressed |
| Video transcoding | MP4/WEBM/MKV/AVI/MOV → MJPEG / BL compressed |
| BL compression | 4×4 blocks, base+interpolated colors, 2-bit index |
| Real-time preview | See conversion results instantly |
| Parameter tuning | Resolution, FPS, brightness, quality, endianness |
| Auto cleanup | 5-minute TTL on converted files |
| Dark/Light theme | One-click toggle |

```bash
cd lcd_host_web
pip install -r requirements.txt
python server.py
# http://localhost:5000
```

**API Endpoints**:

| Route | Method | Purpose |
|:---|:---:|:---|
| `/convert` | POST | Upload & convert |
| `/preview/<id>` | GET | Preview converted result |
| `/download/<id>` | GET | Download binary file |
| `/info/<id>` | GET | Conversion metadata |

### 🔧 Feature Tester — `feature_tester/`

| Tool | Description |
|:---|:---|
| `sender.c` | Windows serial sender — bulk data loopback verification |
| `receiver.c` | Windows serial receiver — LCD frame stream window renderer |
| `image_decoder.py` | Python tool — RGB565 binary → PNG decode |

---

## 🔧 Environment Setup

### Hardware Requirements

- **STM32F401RCT6** dev board
- **160×80 IPS TFT** (ST7735S-compatible)
- **W25Q128** SPI Flash module
- **AT24C64** EEPROM module
- USB data cable (power + CDC)
- Optional: Encoder, LED, Button

### Toolchain

| Tool | Version |
|:---|:---|
| STM32CubeIDE | ≥ 1.15 (recommended) / Makefile also supported |
| ARM GCC | `arm-none-eabi-gcc` ≥ 10.3 |
| Python | ≥ 3.9 |
| FFmpeg | ≥ 5.0 (required by Web Host) |
| OpenOCD | Optional (for flashing) |

### Firmware Build

```bash
# STM32CubeIDE: open stm_ips.ioc → Generate Code → Build All
# Makefile:
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

### 1️⃣ Flash Firmware

Flash the compiled `stm_ips.elf` onto the STM32F401RC via ST-Link.

### 2️⃣ Power On

On startup, the system:
1. Initializes all peripherals (SPI · I2C · USB · TIM)
2. Reads W25Q128 JEDEC ID for chip verification
3. Loads FAT from AT24C64 EEPROM
4. Starts LCD UI → looks for "okay"(image) and "teest"(video) in filesystem
5. Enters main loop

### 3️⃣ Upload Content

```bash
python lcd_host_web/server.py
# http://localhost:5000
# Drag & drop → adjust → convert → download
# Send via USB CDC serial to device
```

### 4️⃣ Playback

Firmware auto-loads files from W25Q at startup. Customize by editing filenames in `lcd_ui.c` and logic in `main.c`.

---

## 👨‍💻 Development Guide

### Adding a New UI Element

```c
// 1. Declare element
static lcd_rect_t g_rect = {10, 30, 20, 15, CYAN};

// 2. Register with animation manager
lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);

// 3. Start animation
lcd_anim_config_t anim = {
    .target      = &g_rect.x,
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
int16_t idx = find_large_file_by_name("my_image");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    // info.start_sector × 4096 = W25Q address
    // info.size = file size
}

// Format (use with caution — prefer USB protocol):
// clear_all_files();
```

### CRC16 Checksum

```c
crc16_usb_init_table();  // Static const table, no-op

uint16_t crc = crc16_usb_packing(data, len, false);
// data[len]   = crc & 0xFF;
// data[len+1] = (crc >> 8) & 0xFF;

// Verify:
// bool ok = crc16_usb_packing(frame, frame_len, true);
```

### System Initialization

```c
void main(void) {
    HAL_Init();  SystemClock_Config();

    MX_GPIO_Init();  MX_DMA_Init();
    MX_I2C1_Init();  MX_SPI1_Init();  MX_SPI2_Init();
    MX_USB_DEVICE_Init();  MX_TIM2_Init();  MX_TIM3_Init();
    MX_TIM9_Init();  MX_TIM4_Init();

    usb_controller_init(&g_usb_controller);
    lcd_init();
    w25q_init();
    storage_manager_init();
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

| Strategy | Approach |
|:---|:---|
| **DMA Preferred** | Use `lcd_screen_update_dma()` instead of polling |
| **Reduce animations** | Lower `LCD_ANIM_MAX_COUNT` / `LCD_LAYER_MAX_COUNT` |
| **SPI clock** | Increase in MX config for higher throughput |
| **Decode cache** | Adjust 512B MJPEG cache to balance RAM vs. speed |
| **USB streaming** | `LCD_USB_STREAM_ENABLE` — render waits for USB TX done |

---

## 📜 License

Copyright (C) 2026 **UnikoZera**

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

**Third-Party Components**:

| Component | License | Author |
|:---|:---|:---|
| picojpeg | Public domain | Rich Geldreich |
| STM32 HAL | STMicroelectronics SLA | STMicroelectronics |
| CMSIS | Apache 2.0 | ARM |

> Based on STM32CubeMX generated framework.

---

<p align="center">
  <sub>Built with ❤️ by UnikoZera · STM32F401RC · 160×80 IPS · 2026</sub>
</p>
