# ⚡ STM IPS — STM32 Image Processing System

> **SPDX-License-Identifier: AGPL-3.0-or-later**
>
> **STM32F401RC 驱动的 160×80 IPS 嵌入式多媒体平台，集图像显示、视频播放、动画引擎、USB 通信与 Web 文件管理于一体。**
>
> **An STM32F401RC-driven 160×80 IPS embedded multimedia platform featuring image display, video playback, animation engine, USB CDC communication, and Web-based file management.**

![STM32](https://img.shields.io/badge/MCU-STM32F401RCT6-03234B)
![Display](https://img.shields.io/badge/Display-160×80_IPS-00BCD4)
![Protocol](https://img.shields.io/badge/Protocol-USB_CDC_Custom-7C4DFF)
![License](https://img.shields.io/badge/License-AGPLv3-blue)
![Status](https://img.shields.io/badge/Status-Active-success)

---

## 📋 目录 | Table of Contents

- [系统概览 | System Overview](#-系统概览--system-overview)
- [硬件架构 | Hardware Architecture](#-硬件架构--hardware-architecture)
- [软件架构 | Software Architecture](#-软件架构--software-architecture)
- [核心功能与代码示例 | Core Features & Code Examples](#-核心功能与代码示例--core-features--code-examples)
  - [🖥️ 显示系统 | Display System](#-显示系统--display-system)
  - [🎞️ 动画引擎 | Animation Engine](#-动画引擎--animation-engine)
  - [🎬 MJPEG 视频播放 | MJPEG Video Playback](#-mjpeg-视频播放--mjpeg-video-playback)
  - [💾 存储管理器 | Storage Manager](#-存储管理器--storage-manager)
  - [🔌 USB 控制器 | USB Controller](#-usb-控制器--usb-controller)
  - [🔗 SPI DMA 传输 | SPI DMA Transfer](#-spi-dma-传输--spi-dma-transfer)
- [USB 通信协议 | USB Communication Protocol](#-usb-通信协议--usb-communication-protocol)
- [文件系统设计 | File System Design](#-文件系统设计--file-system-design)
- [上位机工具 | Host Tools](#-上位机工具--host-tools)
- [环境搭建 | Environment Setup](#-环境搭建--environment-setup)
- [快速上手 | Quick Start](#-快速上手--quick-start)
- [开发指南 | Development Guide](#-开发指南--development-guide)
- [协议与许可 | License](#-协议与许可--license)

---

## 🔭 系统概览 | System Overview

**STM IPS** 是一个运行于 STM32F401RC 上的嵌入式图像处理与多媒体系统。它以一块 160×80 像素的 IPS 彩色 TFT 显示屏为核心输出设备，通过 **SPI DMA** 实现高速帧缓冲刷新，配合 picojpeg 开源解码器实现软解视频播放，并通过自研的 USB CDC 协议与上位机进行文件传输和实时数据流通信。

**STM IPS** is an embedded image processing & multimedia system running on the STM32F401RC. It drives a 160×80 IPS color TFT display through **SPI DMA** for high-speed framebuffer refresh, decodes MJPEG video via the open-source picojpeg library in software, and communicates with the host through a custom USB CDC protocol.

系统包含四个子项目，覆盖从硬件固件到上位机工具的完整链路  
The project consists of four sub-projects spanning firmware to host tools:

| 组件 / Component | 技术栈 / Tech Stack | 用途 / Purpose |
|:---|:---|:---|
| `Core/` | C (STM32 HAL) | 嵌入式固件：LCD 驱动、动画引擎、视频解码、存储管理 / Firmware: LCD driver, animation engine, video decode, storage mgmt |
| `USB_DEVICE/` | C (STM32 USB Device Lib) | USB CDC 虚拟串口层 / USB CDC virtual COM port layer |
| `lcd_host_web/` | Python Flask + HTML5 | Web 端图像/视频转码与文件管理 / Web-based transcoding & file management |
| `feature_tester/` | C + Python | 串口数据回环测试与 RGB565 解码验证 / Serial loopback test & RGB565 decode verification |

---

## ⚙️ 硬件架构 | Hardware Architecture

### 主控芯片 | MCU

| 参数 / Parameter | 规格 / Spec |
|:---|:---|
| **MCU** | STM32F401RCT6 (ARM Cortex-M4, 84MHz) |
| **Flash** | 256KB (内部 / internal) |
| **SRAM** | 64KB |
| **封装 / Package** | LQFP64 |

### 外设连接 | Peripheral Connections

```
┌────────────────────────────────────────────────────────────┐
│                     STM32F401RC                            │
│                                                            │
│   SPI1 (DMA) ────→ 160×80 IPS LCD                          │
│   ├─ PA5 (SCK) · PA6 (MISO) · PA7 (MOSI)                   │
│   ├─ PB0 (RES) · PB1 (DC) · PB2 (CS)                       │
│   └─ PA3 (背光 PWM / Backlight PWM)                         │
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
│   TIM2 / TIM3 / TIM4 / TIM9 (PWM · 定时 / Timing)           │
│                                                            │
│  (未使用 / Unused) 编码器: PA0 (CH1) · PA1 (CH2) · PA2       │
│  (未使用 / Unused) 按钮: PC15 · LED: PC13 / PC14 / PA15      │
└────────────────────────────────────────────────────────────┘
```

### 存储布局 | Memory Layout

| 区域 / Region | 扇区范围 / Sectors | 大小 / Size | 用途 / Purpose |
|:---|:---:|:---:|:---|
| **保留区 / Reserved** | 0 ~ 1 | 8 KB | 保留 / Reserved |
| **小文件区 / Small File** | 2 ~ 63 | 248 KB | 文本/小型数据（字节级紧凑分配）/ Text & small data (byte-level packing) |
| **大文件区 / Large File** | 64 ~ 4031 | 15.5 MB | 图片/视频（扇区对齐位图分配）/ Images & video (sector-aligned bitmap alloc) |
| **用户区 / User** | 4032 ~ 4095 | 256 KB | 用户自定义 / User-defined |

> W25Q128 总容量 16MB，扇区大小 4KB。FAT 持久化存储在 AT24C64 EEPROM 中。
> W25Q128 total capacity 16MB, sector size 4KB. FAT is persisted in AT24C64 EEPROM.

---

## 🧩 软件架构 | Software Architecture

### 模块层次 | Module Hierarchy

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

### 主循环流程 | Main Loop Flow

```
main()
 ├── HAL_Init() + SystemClock_Config()     // 84MHz HSE + PLL
 ├── 外设初始化 / Peripheral init:
 │     MX_GPIO_Init() · MX_DMA_Init()
 │     MX_I2C1_Init() · MX_SPI1_Init() · MX_SPI2_Init()
 │     MX_USB_DEVICE_Init() · MX_TIMx_Init()
 │
 ├── usb_controller_init(&g_usb_controller)   // USB 控制器
 ├── lcd_init()                               // LCD 初始化配置
 ├── w25q_init()                              // W25Q Flash 检测
 ├── storage_manager_init()                   // 从 EEPROM 加载 FAT
 ├── lcd_ui_init()                            // UI 图层 + 动画启动
 │
 └── while(1)
      ├── lcd_ui_updater()          // UI 渲染更新
      ├── w25q_dma_task()           // SPI Flash DMA 状态机
      ├── storage_manager_task()    // USB 文件传输处理
      └── usb_controller_task()     // USB 收发管理
```

---

## ✨ 核心功能与代码示例 | Core Features & Code Examples

### 🖥️ 显示系统 | Display System

160×80 IPS 显示器通过 **SPI1 + DMA** 驱动，帧缓冲区 `lcd_frame_buffer[sizeof(uint16_t) * 160 * 80 + 4]`。全屏刷新通过 `lcd_screen_update_dma()` 一次 DMA 传输完成。

The 160×80 IPS display is driven over **SPI1 + DMA**. The framebuffer `lcd_frame_buffer[sizeof(uint16_t) * 160 * 80 + 4]` is flushed via a single DMA transfer in `lcd_screen_update_dma()`.

**核心 API / Core API**（来自 `lcd.h` & `lcd_driver.h`）：

```c
// ── 基本绘图 / Basic Drawing ──
void lcd_fill_screen(uint16_t color);              // 全屏填充
void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2,
                   uint16_t y2, uint16_t color);
void lcd_draw_rectangle(uint16_t x1, uint16_t y1,
                        uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x0, uint16_t y0,
                     uint8_t r, uint16_t color);
void lcd_draw_string(int16_t x, int16_t y, uint16_t fc,
                     uint16_t bc, uint8_t sizey, const char *p);

// ── DMA 加速绘制 / DMA-Accelerated Drawing ──
void lcd_screen_update_dma(void);                  // DMA 全屏刷新
void lcd_dma_draw_pixel(int16_t x, int16_t y, uint16_t color);
void lcd_dma_draw_filled_rect(int16_t x, int16_t y,
                              int16_t w, int16_t h, uint16_t color);
void lcd_dma_draw_circle(int16_t x0, int16_t y0,
                         uint8_t r, uint16_t color);
void lcd_dma_draw_label(const lcd_label_t *label);

// ── 图片与视频 / Picture & Video ──
void lcd_draw_picture_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height, uint32_t w25q_addr);
void lcd_play_video_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);

// ── 性能监控 / Performance Monitoring ──
void lcd_calculate_fps(void);       // 基于 DWT 时钟周期精确测量
void lcd_calculate_usage(void);     // CPU 使用率百分比

extern uint16_t lcd_fps;            // 当前帧率 (FPS)
extern uint8_t  cpu_usage_percent;  // CPU 占用百分比
```

**使用示例 / Usage Example**：

```c
// 绘制一个包含圆、矩形和文本的场景
lcd_fill_screen(BLACK);                            // 清屏
lcd_draw_string(10, 10, WHITE, BLACK, 8, "STM IPS");  // 标题
lcd_draw_circle(80, 40, 30, RED);                  // 红色圆
lcd_draw_rectangle(20, 20, 60, 60, GREEN);         // 绿色矩形
lcd_draw_line(0, 0, LCD_W - 1, LCD_H - 1, BLUE);   // 蓝色对角线
lcd_screen_update_dma();                           // DMA 刷新

// 性能数据显示（在主循环中周期调用）
lcd_calculate_fps();
lcd_calculate_usage();
// 读取: lcd_fps, cpu_usage_percent

// 改变背光亮度
set_lcd_brightness(128);  // 0~255
```

**颜色常量 / Color Constants**：

| 名称 / Name | 值 / Value | 名称 / Name | 值 / Value |
|:---|:---:|:---|:---:|
| `WHITE` | `0xFFFF` | `BLACK` | `0x0000` |
| `RED` | `0xF800` | `GREEN` | `0x07E0` |
| `BLUE` | `0x001F` | `CYAN` | `0x7FFF` |
| `YELLOW` | `0xFFE0` | `MAGENTA` | `0xF81F` |
| `GRAY` | `0x8430` | `DARKBLUE` | `0x01CF` |

> **设计详解 / Design Notes**：
> - 单缓冲架构，`lcd_frame_buffer[160*80 + 4]`，预留了升级到双缓冲的空间
> - USB 流模式（`LCD_USB_STREAM_ENABLE`）：启用后在 USB 发送完成后再渲染，防止单缓冲区撕裂
> - FPS 计数器基于 DWT 时钟周期精确计算，不占用额外定时器
> - LCD PA3 引脚输出 PWM，通过 TIM 控制背光亮度

---

### 🎞️ 动画引擎 | Animation Engine

一套完整的动画框架，支持多达 **16 个并发动画** 和 **16 个渲染图层**。缓动路径、执行回调、完成回调完全可插拔。

A complete animation framework supporting up to **16 concurrent animations** and **16 render layers**. Easing paths, execution callbacks, and completion callbacks are fully pluggable.

**核心结构体 / Core Structs**（来自 `lcd_driver.h`）：

```c
// ── 动画配置 / Animation Configuration ──
typedef struct {
    void *target;                    // 动画目标变量指针
    int32_t start_value;             // 起始值
    int32_t end_value;               // 结束值
    uint32_t duration_ms;            // 持续时间 (ms)
    uint32_t delay_ms;               // 延迟启动 (ms)
    bool repeat;                     // 是否重复
    bool yoyo;                       // 是否往返（到达终点后反向回到起点）
    lcd_anim_exec_cb_t exec_cb;      // 执行回调（每帧更新目标值）
    lcd_anim_done_cb_t done_cb;      // 完成回调（单次动画结束）
    lcd_anim_path_cb_t path_cb;      // 路径/缓动函数（NULL = 线性）
} lcd_anim_config_t;

// ── 图层类型 / Layer Types ──
typedef struct { int16_t x, y, w, h; uint16_t color; } lcd_rect_t;
typedef struct { int16_t x, y; uint8_t radius; uint16_t color; } lcd_circle_t;
typedef struct { int16_t x, y; uint16_t fg_color, bg_color;
                 uint8_t size; const char *text; } lcd_label_t;
typedef struct { int16_t x, y, width, height; uint32_t addr; } lcd_picture_t;
typedef struct { int16_t x, y, width, height;
                 uint32_t start_addr, end_addr; } lcd_video_t;
```

**API 速览 / API Quick Reference**：

```c
// 动画管理器 / Animation Manager
void lcd_anim_manager_init(void);
void lcd_anim_manager_set_bg(uint16_t color);
void lcd_anim_manager_task(void);       // 主循环调用 - 更新所有动画
void lcd_anim_manager_render(void);     // 主循环调用 - 渲染所有图层

// 图层管理 / Layer Management
int8_t lcd_anim_manager_add_layer(void *ctx, lcd_layer_draw_cb_t draw_cb);
bool   lcd_anim_manager_remove_layer(int8_t layer_id);
void   lcd_anim_manager_clear_layers(void);

// 动画控制 / Animation Control
int8_t lcd_anim_start(const lcd_anim_config_t *config);
bool   lcd_anim_stop(int8_t anim_id);
void   lcd_anim_stop_all(void);

// 缓动路径 / Easing Paths
lcd_anim_path_cb_t lcd_anim_get_path(lcd_anim_ease_t ease);
```

**缓动函数类型 / Easing Functions**（共 14 种）：

| 枚举 / Enum | 效果 / Effect |
|:---|:---|
| `LCD_ANIM_EASE_LINEAR` | 匀速线性 / Linear |
| `LCD_ANIM_EASE_IN_QUAD` | 二次方缓入 / Quad ease-in |
| `LCD_ANIM_EASE_OUT_QUAD` | 二次方缓出 / Quad ease-out |
| `LCD_ANIM_EASE_IN_OUT_QUAD` | 二次方缓入缓出 / Quad ease-in-out |
| `LCD_ANIM_EASE_IN_SINE` | 正弦缓入 / Sine ease-in |
| `LCD_ANIM_EASE_OUT_SINE` | 正弦缓出 / Sine ease-out |
| `LCD_ANIM_EASE_IN_OUT_SINE` | 正弦缓入缓出 / Sine ease-in-out |
| `LCD_ANIM_EASE_IN_EXPO` | 指数缓入 / Expo ease-in (极慢→极快) |
| `LCD_ANIM_EASE_OUT_EXPO` | 指数缓出 / Expo ease-out (极快→极慢) |
| `LCD_ANIM_EASE_IN_OUT_EXPO` | 指数缓入缓出 / Expo ease-in-out |
| `LCD_ANIM_EASE_IN_CIRC` | 圆形缓入 / Circular ease-in |
| `LCD_ANIM_EASE_OUT_CIRC` | 圆形缓出 / Circular ease-out |
| `LCD_ANIM_EASE_IN_OUT_CIRC` | 圆形缓入缓出 / Circular ease-in-out |
| `LCD_ANIM_EASE_IN_OUT_BACK` | 回弹缓动 / Back ease-in-out (超出目标再回弹) |
| `LCD_ANIM_EASE_OUT_ELASTIC` | 弹性缓出 / Elastic ease-out (末端弹跳) |

**使用示例 / Usage Example**：

```c
// ── 示例 1：矩形在 X 轴来回弹性滑动 ──
static lcd_rect_t g_rect = {10, 30, 28, 15, CYAN};

// 注册到图层管理器
int8_t layer_id = lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);

// 启动动画
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

// ── 示例 2：文字标签淡入淡出 ──
static lcd_label_t g_label = {40, 35, WHITE, BLACK, 8, "Loading"};

int8_t l2 = lcd_anim_manager_add_layer(&g_label, lcd_dma_draw_label);

// 透明度可以从颜色分量计算，此处示意往返动画
lcd_anim_config_t fade = {
    .target       = &g_label.x,
    .start_value  = 40,
    .end_value    = 100,
    .duration_ms  = 800,
    .repeat       = true,
    .yoyo         = true,
    .exec_cb      = lcd_anim_exec_set_i16,
    .path_cb      = lcd_anim_get_path(LCD_ANIM_EASE_OUT_ELASTIC),
};
lcd_anim_start(&fade);

// ── 更新（在主循环中调用） ──
// lcd_anim_manager_task();    // 更新动画状态
// lcd_anim_manager_render();  // 渲染所有图层到帧缓冲
```

> **设计详解 / Design Notes**：
> - 动画引擎核心在 `lcd_driver.c` 中实现，使用 `s_anim_slots[]` 和 `s_layer_slots[]` 静态数组管理
> - 每一帧通过 `lcd_anim_manager_task()` 更新动画进度，`lcd_anim_manager_render()` 按图层顺序绘制
> - 动画路径函数内部使用 Q10 定点数（`lcd_anim_mix_q10()`），避免浮点运算开销
> - 支持 `delay_ms` 延迟启动，`repeat` 和 `yoyo` 实现循环和往返效果
> - 最大动画数和图层数可通过 `LCD_ANIM_MAX_COUNT` 和 `LCD_LAYER_MAX_COUNT` 调整

---

### 🎬 MJPEG 视频播放 | MJPEG Video Playback

使用 **picojpeg** 开源库在 STM32F401 上软解 JPEG 帧，从 W25Q Flash 读取数据，逐帧解码为 RGB565 写入帧缓冲。

Uses the **picojpeg** library to software-decode JPEG frames on the STM32F401, reading from W25Q Flash and decoding each frame to RGB565 into the framebuffer.

**文件格式 / File Format**：

```
Header (14 Bytes):
  [0..3]  Magic "MJPG" (0x47504A4D LE)
  [4..5]  frame_count (uint16 LE)
  [6..7]  width        (uint16 LE)
  [8..9]  height       (uint16 LE)
  [10..13] reserved

Body (重复 frame_count 次):
  [frame_size (uint32 LE)] [JPEG data (frame_size bytes)]
```

**核心 API / Core API**（来自 `lcd_mjpeg.h`）：

```c
// 解码状态（可通过 UI 诊断查看）
typedef struct {
    uint8_t  active;                    // 是否激活
    int16_t  width;                     // 图像宽度
    int16_t  height;                    // 图像高度
    uint32_t start_addr;                // W25Q 中文件的起始地址
    uint32_t end_addr;                  // W25Q 中文件的结束地址
    uint16_t frame_count;               // 总帧数
    uint32_t cur_frame_idx;             // 当前帧序号
    uint32_t current_frame_pos;         // 当前数据指针位置
    uint32_t frame_size;                // 当前帧 JPEG 数据大小
    uint32_t frame_data_addr;           // 当前帧 JPEG 数据起始地址
    int16_t  lcd_x;                     // LCD X 偏移
    int16_t  lcd_y;                     // LCD Y 偏移
    int8_t   last_error;                // 最后的错误码
    uint8_t  pjpeg_ret;                 // picojpeg 返回值
    uint8_t  frame_dump[44];            // 前 44 字节帧数据（调试输出）
} mjpeg_state_t;

// ── 播放 MJPEG 视频 ──
// 每次调用解码一帧，播完最后一帧自动回到文件头循环
void lcd_play_mjpeg_video(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);

// ── 诊断 / Diagnostics ──
int8_t lcd_mjpeg_last_error(void);
const mjpeg_state_t *lcd_mjpeg_get_state(void);  // 用于 UI 显示
```

**解码流程 / Decode Flow**：

1. 读取 14 字节文件头，验证 Magic 是否为 `0x47504A4D`
2. 读取 4 字节帧大小前缀 + JPEG 数据
3. 使用 **512 字节智能读缓存** 从 W25Q 读取（缓存命中跳过 Flash 读取）
4. 逐 MCU 调用 `pjpeg_decode_mcu()` 解码
5. 将 RGB565 像素写入 `lcd_write_ptr` 帧缓冲区
6. 播完最后一帧自动回到文件头循环播放

**使用示例 / Usage Example**：

```c
// 从 W25Q 播放 MJPEG 视频（全屏循环播放）
lcd_play_mjpeg_video(0, 0, 160, 80,
    video_info.start_sector * 4096,        // 起始地址
    video_info.start_sector * 4096 + video_info.size  // 结束地址
);

// 在 UI 上显示解码状态
const mjpeg_state_t *state = lcd_mjpeg_get_state();
if (state->last_error != 0) {
    // 处理错误
    switch (state->last_error) {
        case MJPEG_ERR_BAD_MAGIC:  // 254 — 非法的 MJPEG 文件
        case MJPEG_ERR_DECODE_INIT: // 251 — 解码初始化失败
        case MJPEG_ERR_ZERO_FRAMES: // 253 — 0 帧
            break;
    }
}
```

> **设计详解 / Design Notes**：
> - 错误码定义见 `lcd_mjpeg.h`：`MJPEG_ERR_DMA_BUSY(255)`、`MJPEG_ERR_BAD_MAGIC(254)` 等
> - 解码状态结构体 `mjpeg_state_t` 完整暴露给 UI 层，支持诊断显示
> - 帧数据使用前 44 字节 dump 字段保留在状态中，便于串口调试
> - 512 字节读缓存减少 W25Q SPI 读取次数，提升解码性能

---

### 💾 存储管理器 | Storage Manager

一个运行在 **W25Q128 (16MB SPI Flash) + AT24C64 (8KB EEPROM)** 上的轻量级嵌入式文件系统。小文件按字节级紧凑分配，大文件按 4KB 扇区对齐。

A lightweight embedded filesystem running on **W25Q128 (16MB SPI Flash) + AT24C64 (8KB EEPROM)**. Small files use byte-level packing; large files use 4KB sector-aligned allocation.

**FAT 结构 / FAT Structure**（存储在 AT24C64 中，来自 `storage_manager.c`）：

```c
#define FAT_MAGIC_NUMBER 0x0D000721
#define W25Q_SECTOR_SIZE 4096
#define MAX_FILENAME_LEN 16
#define MAX_SMALL_FILES 32
#define MAX_LARGE_FILES 32

typedef struct {
    uint32_t magic;                    // 魔数 0x0D000721
    uint32_t small_next_addr;          // 小文件区下一分配地址
    uint16_t small_file_count;
    small_file_info_t small_files[32]; // 最多 32 个小文件
    uint32_t large_next_sector;        // 大文件区下一分配扇区
    uint16_t large_file_count;
    large_file_info_t large_files[32]; // 最多 32 个大文件
} storage_fat_t;
```

**核心 API / Core API**（来自 `storage_manager.h`）：

```c
// 初始化 / Initialization
bool storage_manager_init(void);
void storage_manager_task(void);       // 主循环调用 - 处理 USB 文件操作

// FAT 读写 / FAT Read/Write
bool storage_fat_load(void);
void storage_fat_save(void);           // 持久化到 AT24C64 EEPROM

// 文件查询 / File Lookup
int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);

// 文件信息 / File Info
bool get_small_file_info(uint8_t file_id, small_file_info_t *info);
bool get_large_file_info(uint8_t file_id, large_file_info_t *info);

// 清除 / Clear All
void clear_large_file(void);
void clear_small_file(void);

// 状态 / State
bool storage_is_downloading(void);
```

**使用示例 / Usage Example**：

```c
// ── 查找文件并读取内容 ──
int16_t idx = find_large_file_by_name("pic_mp");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    // 起始地址 = info.start_sector * 4096
    // 文件大小 = info.size (bytes)
    // 文件类型 = info.file_type
    // 显示图片: lcd_draw_picture_from_w25q(0, 0, 160, 80, info.start_sector * 4096);
}

// ── 查找视频文件 ──
int16_t vid = find_large_file_by_name("qwq");
if (vid >= 0) {
    large_file_info_t vinfo;
    get_large_file_info((uint8_t)vid, &vinfo);
    // 播放 MJPEG: lcd_play_mjpeg_video(0, 0, 160, 80,
    //     vinfo.start_sector * 4096,
    //     vinfo.start_sector * 4096 + vinfo.size);
}
```

**文件类型对比 / File Type Comparison**：

| 特性 / Feature | 小文件 / Small | 大文件 / Large |
|:---|:---|:---|
| 分配粒度 / Allocation | 字节级 / Byte-level | 4KB 扇区 / Sector |
| 最大数量 / Max count | 32 | 32 |
| 适用场景 / Use case | 文本、配置 / Text, config | 图片、视频 / Image, video |
| 删除碎片 / Fragmentation | 有（允许）/ Allowed | 无（位图清空）/ None (bitmap clear) |
| 文件名长度 / Name length | ≤ 16 字节 / bytes | ≤ 16 字节 / bytes |

> **设计详解 / Design Notes**：
> - 分区映射见 `storage_manager.c`：保留区 (0~1) → 小文件区 (2~63, 248KB) → 大文件区 (64~4031, 15.5MB) → 用户区 (4032~4095, 256KB)
> - FAT 持久化在 AT24C64 中，每次文件操作后调用 `storage_fat_save()` 写入 EEPROM
> - USB 协议命令通过 `storage_manager_task()` 在 main loop 中解析处理

---

### 🔌 USB 控制器 | USB Controller

基于 **USB CDC (Virtual COM Port)** 的自定义通信协议，采用单槽待发送机制以节省 SRAM。

A custom protocol over **USB CDC (Virtual COM Port)**, using a single-slot pending-send mechanism to conserve SRAM.

**核心 API / Core API**（来自 `usb_controller.h`）：

```c
// ── 初始化与主循环 / Init & Main Loop ──
void usb_controller_init(usb_controller_t *controller);
void usb_controller_task(usb_controller_t *controller);

// ── 发送 / Transmit ──
// cmd: 命令字节, data: payload 指针, len: payload 长度
// 返回枚举: USB_SEND_OK / USB_SEND_QUEUED / USB_SEND_DROPPED_PREVIOUS
usb_send_status_t usb_controller_send(
    usb_controller_t *controller,
    uint8_t cmd, const uint8_t *data, uint16_t len);

// ── 接收 / Receive ──
uint16_t usb_controller_receive(
    usb_controller_t *controller,
    uint8_t *buf, uint16_t len);
uint16_t usb_controller_get_rx_free_space(void);

// ── HAL 回调桥接 / HAL Callback Bridge ──
void usb_controller_on_tx_complete(void);
void usb_controller_on_rx_received(uint8_t *buf, uint32_t len);
```

**使用示例 / Usage Example**：

```c
// ── 发送数据到主机 ──
uint8_t response[] = {0xA0, 0x00, 0x00};  // 成功响应
usb_controller_send(&g_usb_controller,
    0xA0, response, sizeof(response));
// 返回 USB_SEND_OK 或 USB_SEND_QUEUED

// ── 主循环处理 USB 任务 ──
// while(1) {
//     usb_controller_task(&g_usb_controller);
// }
```

**关键设计决策 / Key Design Decisions**：

| 特性 / Feature | 说明 / Description |
|:---|:---|
| **单槽待发送** | 不复制 payload，仅持指针，节省 64KB SRAM |
| **环形缓冲区接收** | 2560 字节，覆盖最大协议帧 (2055B) + 余量 |
| **发送分片** | 每次 6144 字节 (`USB_SEND_BYTES_PER_CALL`)，维持 LCD 流稳定性 |
| **超时保护** | 2 秒 (`USB_TX_STUCK_TIMEOUT_MS`) 无进展触发软恢复 |
| **LCD 流同步** | 发送完成后再渲染，防止单缓冲区撕裂 |

**协议帧格式 / Protocol Frame Format**：

```
┌─────────┬─────────┬──────────┬─────────────┬──────────────────┐
│  0xAA   │  0x55   │ Command  │  Length     │  Payload + CRC16 │
│ (1 Byte)│ (1 Byte)│ (1 Byte) │ (2 Byte LE) │   (Variable)     │
└─────────┴─────────┴──────────┴─────────────┴──────────────────┘
```

---

### 🔗 SPI DMA 传输 | SPI DMA Transfer

SPI1 用于 LCD 显示，SPI2 用于 W25Q Flash 通信，均支持 DMA 传输。

SPI1 drives the LCD display, SPI2 communicates with the W25Q Flash — both support DMA transfer.

**核心 API / Core API**（来自 `w25q_controller.h`）：

```c
// SPI Flash 控制命令
#define W25Q_WriteEnable     0x06
#define W25Q_WriteDisable    0x04
#define W25Q_PageProgram     0x02
#define W25Q_SectorErase     0x20   // 4KB
#define W25Q_BlockErase64K   0xD8   // 64KB
#define W25Q_ChipErase       0xC7
#define W25Q_ReadData        0x03
#define W25Q_FastReadData    0x0B
#define W25Q_JedecDeviceID   0x9F

// ── 基本读写 / Basic R/W ──
bool w25q_init(void);
void w25q_read_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_write_data(uint32_t address, uint8_t *data, uint32_t size);
void w25q_erase_sector(uint32_t address);
uint32_t w25q_read_id(void);

// ── DMA 传输 / DMA Transfer ──
bool w25q_read_data_dma(uint32_t address, uint8_t *data, uint32_t size);
bool w25q_write_data_dma(uint32_t address, uint8_t *data, uint32_t size);
bool w25q_fast_read_data_dma(uint32_t address, uint8_t *data, uint32_t size);
void w25q_dma_task(void);              // 主循环中调用的 DMA 状态机
bool w25q_dma_is_busy(void);
bool w25q_dma_is_done(void);
bool w25q_dma_is_error(void);
```

**使用示例 / Usage Example**：

```c
// ── 初始化 W25Q ──
if (!w25q_init()) {
    // 初始化失败：W25Q 不可用
    lcd_draw_string(10, 10, RED, BLACK, 8, "W25Q FAIL");
}

// ── 扇区擦除 & 数据写入 ──
uint8_t data[256] = { /* ... */ };
w25q_erase_sector(64 * 4096);          // 擦除第 64 扇区
w25q_write_data(64 * 4096, data, 256); // 写入 256 字节

// ── DMA 读取 ──
uint8_t buf[256];
w25q_fast_read_data_dma(64 * 4096, buf, sizeof(buf));

// ── 主循环中调用 DMA 状态机 ──
// while (1) {
//     w25q_dma_task();  // 处理 DMA 完成/错误回调
// }
```

> **设计详解 / Design Notes**：
> - `w25q_check_busy()` 等待 W25Q 内部操作完成，使用超时机制
> - DMA 传输通过 `w25q_dma_task()` 状态机管理，在主循环中轮询
> - `W25Q_CS_LOW()` / `W25Q_CS_HIGH()` 宏直接控制片选 GPIO
> - SPI 句柄：`hspi1` (LCD) 和 `hspi2` (W25Q)，定义在 `spi.h`

---

## 📡 USB 通信协议 | USB Communication Protocol

### 命令集 / Command Set

| 命令 / Command | 编码 / Code | 方向 / Direction | 说明 / Description |
|:---|:---:|:---|:---|
| **开始下载大文件** | `0x11` | Host → Device | 传输图片/视频数据 |
| **开始下载小文件** | `0x45` | Host → Device | 传输文本/小数据 |
| **结束下载** | `0x14` | Host → Device | 文件传输完成，注册到 FAT |
| **删除文件** | `0x19` | Host → Device | 按文件类型 + 索引删除 |
| **查询文件列表** | `0x20` | Host → Device | 查询 FAT 文件目录 |
| **成功响应** | `0xA0` | Device → Host | 操作成功 |
| **错误响应** | `0xE0` | Device → Host | 传输错误通知 |

### 传输流程 / Transfer Flow

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

### USB 通信实例代码 / USB Communication Example Code

```c
// ── 主机端发送命令帧示例 ──
// Frame: 0xBB 0x44 [CMD] [LEN_LO] [LEN_HI] [PAYLOAD...] [CRC16_LO] [CRC16_HI]
//
// 开始下载大文件:
// BB 44 11 04 00 31 32 89 C6
//   └─ 命令: 0x11 (开始下载大文件)
//      └─ 长度: 0x0004 (数据 2 字节 + CRC16 2 字节)
//         └─ 数据: 0x31 0x32
//            └─ CRC16: 0xC689 (小端)
//
// 查询文件列表:
// BB 44 20 02 00 2B BE
//
// 删除文件 (类型=0, 索引=5):
// BB 44 19 04 00 00 05 DF CD
```

---

## 🗂️ 文件系统设计 | File System Design

```
W25Q128 (16MB) 分区布局 / Partition Layout
═══════════════════════════════════════════
Sector 0-1     │ 保留区 / Reserved (8KB)
───────────────┼────────────────────────────
Sector 2-63    │ 小文件区 / Small File Area (248KB)
               │ 字节级紧凑分配 / Byte-level packing
───────────────┼────────────────────────────
Sector 64-4031 │ 大文件区 / Large File Area (15.5MB)
               │ 4KB 扇区对齐位图分配 / Sector-aligned bitmap
───────────────┼────────────────────────────
Sector 4032-4095│ 用户自定义区 / User Area (256KB)
═══════════════════════════════════════════

AT24C64 (8KB EEPROM) 用途 / Usage
═══════════════════════
地址 0x0000 → storage_fat_t (文件分配表 / File Allocation Table)
```

---

## 🛠️ 上位机工具 | Host Tools

### 🌐 Web Host — `lcd_host_web/`

一个基于 **Flask** 的现代 Web 应用，用于转换和管理图像/视频内容。
A modern Flask-based web application for transcoding and managing image/video content.

**功能亮点 / Features**：

| 功能 / Feature | 说明 / Description |
|:---|:---|
| 图片转码 | PNG/JPG/BMP/GIF → RGB565 / BL 压缩格式 |
| 视频转码 | MP4/WEBM/MKV/AVI/MOV → MJPEG / BL 压缩格式 |
| BL 块压缩 | 4×4 像素块、2 基色 + 2 插值色、2bit 索引编码 |
| 预览生成 | 转换结果实时预览 |
| 参数调节 | 分辨率、帧率、亮度、质量、字节序可调 |
| 自动清理 | 转换文件 5 分钟 TTL 自动删除 |
| 深色/浅色主题 | 一键切换 |

```bash
# 启动 Web Host
cd lcd_host_web
pip install -r requirements.txt
python server.py
# 浏览器访问 http://localhost:5000
```

**API 端点 / Endpoints**：

| 路由 / Route | 方法 / Method | 用途 / Purpose |
|:---|:---:|:---|
| `/convert` | POST | 上传文件并转换为 STM IPS 格式 |
| `/preview/<id>` | GET | 获取转换后文件的预览图像 |
| `/download/<id>` | GET | 下载转换后的二进制文件 |
| `/info/<id>` | GET | 获取转换文件元信息 |

### 🔧 Feature Tester — `feature_tester/`

开发与调试辅助工具集 / Development & debugging tools：

| 工具 / Tool | 说明 / Description |
|:---|:---|
| `sender.c` | Windows 串口发送器 — 向设备发送大量数据并逐字节验证回显 |
| `receiver.c` | Windows 串口接收器 — 接收 LCD 帧流并在窗口中实时渲染 |
| `image_decoder.py` | Python 工具 — 将 RGB565 二进制数据解码为 PNG 图像 |

---

## 🔧 环境搭建 | Environment Setup

### 硬件需求 / Hardware Requirements

- **STM32F401RCT6** 开发板 / dev board
- **160×80 IPS TFT** 显示屏（兼容 ST7735S 驱动 / ST7735S-compatible）
- **W25Q64** SPI Flash 模块
- **AT24C64** EEPROM 模块
- USB 数据线 / data cable（供电 + CDC 通信 / power + CDC）
- 可选 / Optional：编码器 / Encoder、LED、按钮 / Button

### 开发工具链 / Toolchain

| 工具 / Tool | 版本要求 / Version |
|:---|:---|
| STM32CubeIDE | ≥ 1.15（推荐 / recommended）/ 亦可使用 Makefile |
| ARM GCC | arm-none-eabi-gcc ≥ 10.3 |
| Python | ≥ 3.9 |
| FFmpeg | ≥ 5.0（Web Host 必须 / required by Web Host） |
| OpenOCD | （可选 / optional，用于烧录 / for flashing） |

### 固件编译 / Firmware Build

```bash
# 方式一 / Option 1: STM32CubeIDE
# 打开 stm_ips.ioc → Generate Code → Build All

# 方式二 / Option 2: Makefile
cd Debug && make -j4
# 输出 / Output: stm_ips.elf / stm_ips.bin
```

### 烧录 / Flashing

```bash
# OpenOCD (ST-Link)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program Debug/stm_ips.elf verify reset exit"

# STM32CubeProgrammer
STM32_Programmer_CLI -c port=SWD -w Debug/stm_ips.elf -rst
```

---

## 🚀 快速上手 | Quick Start

### 第一步：烧录固件 / Step 1: Flash Firmware

将编译好的 `stm_ips.elf` 烧录到 STM32F401RC 开发板。
Flash the compiled `stm_ips.elf` onto the STM32F401RC board.

### 第二步：启动系统 / Step 2: Power On

上电后系统自动运行 / On power-on, the system automatically:

1. 初始化所有外设（SPI · I2C · USB · 定时器）/ Initialize all peripherals
2. 检测 W25Q Flash 并初始化 / Initialize W25Q Flash
3. 从 AT24C EEPROM 加载 FAT 文件分配表 / Load FAT from AT24C EEPROM
4. 启动 LCD UI 动画引擎 / Start LCD UI animation engine
5. 进入主循环，持续更新 USB 与存储管理任务 / Enter main loop

### 第三步：上传内容 / Step 3: Upload Content

**Web Host 方式 / Via Web Host**：
```bash
python lcd_host_web/server.py
# 浏览器打开 / Open http://localhost:5000
# 拖入图片或视频 → 调整分辨率/质量 → 转换 → 下载
# Drag image/video → Adjust resolution/quality → Convert → Download
```

### 第四步：播放 / Step 4: Playback

固件启动后自动从 W25Q 中查找以下文件名的资源：
Firmware looks for the following filenames in W25Q on startup:

- 查找 `pic_mp` → 解码显示图片 / Decode & display image
- 查找 `qwq` → 解码播放 MJPEG 视频（循环播放）/ Play MJPEG video (loop)

> 可通过修改 `lcd_ui.c` 中的文件名和 `main.c` 的业务逻辑，定制启动后的显示内容。
> Customize startup content by modifying filenames in `lcd_ui.c` and logic in `main.c`.

---

## 👨‍💻 开发指南 | Development Guide

### 添加新的 UI 元素 / Adding a New UI Element

```c
// 1️⃣ 声明元素实例 / Declare element instance
static lcd_rect_t g_my_rect = {10, 30, 20, 15, CYAN};

// 2️⃣ 注册到动画管理器 / Register with animation manager
int8_t layer_id = lcd_anim_manager_add_layer(&g_my_rect, lcd_draw_rect_layer);

// 3️⃣ 添加动画 / Add animation
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

### 文件系统操作 / File System Operations

```c
// 按文件名查找大文件 / Find large file by name
int16_t idx = find_large_file_by_name("my_image");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    // 起始地址 / Start addr = info.start_sector * 4096
    // 文件大小 / Size = info.size
    // 显示图片: lcd_draw_picture_from_w25q(0, 0, 160, 80,
    //     info.start_sector * 4096);
}

// 清除所有文件 / Clear all files（慎用, 建议通过 USB 协议操作）
// clear_large_file();
// clear_small_file();
```

### CRC16 校验 / CRC16 Checksum

```c
// 初始化 CRC16 查找表
crc16_usb_init_table();

// 计算 USB 协议帧的 CRC16
// has_crc = true 表示 data 末尾已有 2 字节 CRC 占位，不计入校验
uint16_t crc = crc16_usb_packing(data, len, false);

// 追加到协议帧末尾（小端）
// data[len] = crc & 0xFF;
// data[len+1] = (crc >> 8) & 0xFF;
```

### 系统初始化序列 / System Initialization Sequence

```c
// main() 中的完整初始化流程
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

    // 应用层初始化 / Application Init
    usb_controller_init(&g_usb_controller);
    lcd_init();

    bool w25q_ok = w25q_init();
    bool storage_init_ok = storage_manager_init();

    // clear_large_file();           // 可选：清除所有文件
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

### 性能优化建议 / Performance Tips

- **DMA 优先**：尽量使用 `lcd_screen_update_dma()` 而非轮询刷新，释放 CPU
- **缓冲区策略**：`LCD_USB_STREAM_ENABLE` 开启时，LCD 会在 USB 发送完成后才渲染，避免撕裂
- **动画数量**：`LCD_ANIM_MAX_COUNT` 默认为 16，可调低以节省 RAM 和 CPU 开销
- **SPI 时钟**：SPI1 和 SPI2 时钟可在 MX 配置中调整，更高的 SPI 时钟提升帧率和 Flash 读写速度
- **解码缓存**：MJPEG 解码使用 512 字节智能缓存，可调整大小以平衡 RAM 和性能

---

## 📜 协议与许可 | License

Copyright (C) 2026 **UnikoZera**

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.**

This program is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.

---

**第三方组件 / Third-Party Components**：

| 组件 / Component | 许可 / License | 作者 / Author |
|:---|:---|:---|
| **picojpeg** | Public domain | [Rich Geldreich](https://github.com/richgel999/picojpeg) |
| **STM32 HAL** | STMicroelectronics SLA | STMicroelectronics |
| **CMSIS** | Apache 2.0 | ARM |

> 本项目基于 STM32CubeMX 生成框架开发。
> This project is based on the STM32CubeMX generated framework.

---

## 🌐 English Version

For a complete English version of this README, see **[README_EN.md](README_EN.md)**.

---

<p align="center">
  <sub>Built with by UnikoZera · STM32F401RC · 160×80 IPS · 2026</sub>
</p>
