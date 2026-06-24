# ⚡ STM IPS

> **STM32F401RC 驱动的 160×80 IPS 嵌入式多媒体平台**  
> **Display · Animation · MJPEG Video · USB CDC · Embedded File System**

![STM32](https://img.shields.io/badge/MCU-STM32F401RCT6-03234B?logo=arm)
![Display](https://img.shields.io/badge/Display-160%C3%9780_IPS-00BCD4)
![Protocol](https://img.shields.io/badge/Protocol-USB_CDC_Custom-7C4DFF)
![License](https://img.shields.io/badge/License-AGPLv3-blue)
![Status](https://img.shields.io/badge/Status-Active-success)
- [English Version](./README_EN.md)
---

## 📋 目录

- [系统概览](#-系统概览)
- [硬件架构](#-硬件架构)
- [软件架构](#-软件架构)
- [核心功能](#-核心功能)
  - [显示系统](#-显示系统)
  - [动画引擎](#-动画引擎)
  - [MJPEG 视频播放](#-mjpeg-视频播放)
  - [BL 压缩视频](#-bl-压缩视频)
  - [嵌入式文件系统](#-嵌入式文件系统)
  - [USB 控制器](#-usb-控制器)
  - [SPI DMA 传输层](#-spi-dma-传输层)
  - [性能监控](#-性能监控)
- [USB 通信协议](#-usb-通信协议)
- [文件系统设计](#-文件系统设计)
- [上位机工具](#-上位机工具)
- [环境搭建](#-环境搭建)
- [快速上手](#-快速上手)
- [开发指南](#-开发指南)
- [许可](#-许可)

---

## 🔭 系统概览

**STM IPS** 是一个运行于 STM32F401RC (Cortex-M4, 84MHz) 上的嵌入式图像处理与多媒体系统。系统以一块 **160×80 IPS 彩色 TFT** 为核心输出设备，通过 **SPI DMA** 实现全屏无撕裂刷新，搭载：

- 自研 **动画引擎** — 15 种缓动函数、16 路并发动画、16 层渲染管线
- **picojpeg 软解** MJPEG 视频播放器
- **轻量级文件系统** — 双区分配（字节级小文件 / 位图大文件），FAT 持久化于 EEPROM
- **USB CDC 自定义协议** — 文件传输、LCD 实时流、设备诊断

项目覆盖从硬件固件到上位机工具的完整链路：

| 组件 | 技术栈 | 用途 |
|:---|:---|:---|
| `Core/` | C (STM32 HAL) | 固件：LCD 驱动、动画、视频解码、存储管理、USB 协议 |
| `USB_DEVICE/` | C (STM32 USB Device Lib) | USB CDC 虚拟串口层 |
| `lcd_host_web/` | Python Flask + HTML5 | Web 端图像/视频转码与文件管理 |
| `feature_tester/` | C + Python | 串口回环测试 / RGB565 解码验证 |

---

## ⚙️ 硬件架构

### 主控芯片

| 参数 | 规格 |
|:---|:---|
| **MCU** | STM32F401RCT6, ARM Cortex-M4 FPU, 84MHz |
| **Flash** | 256KB (内部) |
| **SRAM** | 64KB |
| **封装** | LQFP64 |

### 外设连接

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
│   └─ 4096 sectors × 4KB, page program 256B                      │
│                                                                  │
│   I2C1 ───────────────────────→ AT24C64 8KB EEPROM               │
│   ├─ SCL(PB8) · SDA(PB9)                                        │
│   └─ 32B page, I2C addr 0xA0, 存储 FAT 文件分配表               │
│                                                                  │
│   USB_OTG_FS ─────────────────→ USB CDC Virtual COM Port         │
│   └─ EN(PB12)                                                   │
│                                                                  │
│   TIM2 · TIM3 · TIM4 · TIM9 ── PWM / 定时                        │
│   (未使用) 编码器 PA0/PA1 · LED PC13/PC14/PA15 · 按钮 PC15        │
└──────────────────────────────────────────────────────────────────┘
```

### 存储布局

| 区域 | 扇区范围 | 大小 | 分配策略 |
|:---|:---:|:---:|:---|
| **保留区** | 0 ~ 1 | 8 KB | 压缩暂存区 |
| **小文件区** | 2 ~ 63 | 248 KB | 字节级线性挤压 + 可回收压缩 |
| **大文件区** | 64 ~ 4031 | 15.5 MB | 4KB 扇区对齐位图分配 |
| **用户区** | 4032 ~ 4095 | 256 KB | 用户自定义 |

> W25Q128 总容量 16MB。FAT (文件分配表) 持久化存储在 AT24C64 EEPROM 地址 `0x0000`。

---

## 🧩 软件架构

### 模块层次

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

### 主循环流程

```
main()
 ├── HAL_Init() + SystemClock_Config()       // 84MHz HSE + PLL
 ├── 外设初始化:
 │     MX_GPIO_Init() · MX_DMA_Init()
 │     MX_I2C1_Init() · MX_SPI1_Init() · MX_SPI2_Init()
 │     MX_USB_DEVICE_Init() · MX_TIMx_Init()
 │
 ├── 应用层初始化:
 │     usb_controller_init()   // USB 收发器
 │     lcd_init()              // ST7735 初始化序列
 │     w25q_init()             // W25Q128 JEDEC ID 检测
 │     storage_manager_init()  // 从 AT24C 加载 FAT
 │     lcd_ui_init()           // UI 图层 + 动画启动
 │
 └── while(1)  // ~60 FPS @ 84MHz
      ├── lcd_ui_updater()          // 更新 UI 状态 + 动画 + 渲染
      ├── w25q_dma_task()           // SPI Flash DMA 状态机轮询
      ├── storage_manager_task()    // 解析 USB 主机命令帧
      └── usb_controller_task()     // USB 发送队列 + 超时恢复
```

---

## ✨ 核心功能

### 🖥️ 显示系统

160×80 IPS 显示器通过 **SPI1 + DMA2 Stream3** 驱动，单缓冲架构（预留尾字节支持双缓冲迁移）。

**分辨率控制**：通过 `USE_HORIZONTAL` 宏配置（0=竖屏, 1=竖屏+BGR, 2=横屏, 3=横屏+BGR），默认横屏 `LCD_W=160` × `LCD_H=80`。

```c
// ── 基本绘图 ──
void lcd_fill_screen(uint16_t color);              // 全屏填充
void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2,
                   uint16_t y2, uint16_t color);   // Bresenham
void lcd_draw_rectangle(uint16_t x1, uint16_t y1,
                        uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x0, uint16_t y0,
                     uint8_t r, uint16_t color);
void lcd_draw_string(int16_t x, int16_t y, uint16_t fc,
                     uint16_t bc, uint8_t sizey, const char *p);

// ── DMA 加速绘制（目标：帧缓冲 → SPI DMA → LCD）──
void lcd_screen_update_dma(void);                  // 单次 DMA 全屏刷新
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

// ── 从 W25Q Flash 读取显示 ──
void lcd_draw_picture_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height, uint32_t w25q_addr);
void lcd_play_video_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_addr, uint32_t file_size);
// ↑ 自动检测格式: MJPEG(magic "MJPG") / BL 压缩(magic "BL") / 原始RGB565

// ── USB 实时流 ──
// lcd_usb_stream_enabled=true 时，每次 DMA 更新后自动通过 USB 发送帧数据
// 帧尾附加同步字节 {0x0D, 0x00, 0x07, 0x21}
// 受 LCD_USB_STREAM_MIN_INTERVAL_MS(30ms) 限速

// ── 硬件性能计数器 ──
void lcd_calculate_fps(void);       // 基于 DWT_CYCCNT 的帧率统计
void lcd_calculate_usage(void);     // DWT 周期计数器估算 CPU 负载

extern uint16_t lcd_fps;            // 当前帧率
extern uint8_t  cpu_usage_percent;  // CPU 占用百分比
```

**使用示例**：

```c
lcd_fill_screen(BLACK);
lcd_draw_string(10, 10, WHITE, BLACK, 8, "STM IPS");
lcd_draw_circle(80, 40, 30, RED);
lcd_draw_rectangle(20, 20, 60, 60, GREEN);
lcd_draw_line(0, 0, LCD_W - 1, LCD_H - 1, BLUE);
lcd_screen_update_dma();            // 一次 DMA 传输刷新全屏

set_lcd_brightness(128);            // 背光 0~255 (PWM)
```

**预定义颜色**（20 种 RGB565 常量）：

| 名称 | 值 | 名称 | 值 |
|:---|:---:|:---|:---:|
| `WHITE` | `0xFFFF` | `BLACK` | `0x0000` |
| `RED` | `0xF800` | `GREEN` | `0x07E0` |
| `BLUE` | `0x001F` | `CYAN` | `0x7FFF` |
| `YELLOW` | `0xFFE0` | `MAGENTA` | `0xF81F` |
| `GRAY` | `0x8430` | `DARKBLUE` | `0x01CF` |
| ... | ... | ... | ... |

> **设计要点**：单缓冲帧缓冲 `lcd_frame_buffer[160*80 + 4]`，`lcd_write_ptr` 为写指针，预留尾字节用于 USB 流同步。DMA 忙标志防止并发刷新冲突。

---

### 🎞️ 动画引擎

完整的动画框架，支持 **16 路并发动画** × **16 层渲染图层**。缓动函数、执行回调、完成回调完全可插拔。内部使用 Q10 定点数运算避免浮点开销。

```c
// ── 动画配置 ──
typedef struct {
    void *target;                    // 目标变量指针
    int32_t start_value;             // 起始值
    int32_t end_value;               // 结束值
    uint32_t duration_ms;            // 持续时间 (ms)
    uint32_t delay_ms;               // 延迟启动 (ms)
    bool repeat;                     // 重复
    bool yoyo;                       // 往返
    lcd_anim_exec_cb_t exec_cb;      // 执行回调（每帧更新目标）
    lcd_anim_done_cb_t done_cb;      // 完成回调
    lcd_anim_path_cb_t path_cb;      // 缓动函数（NULL = 线性）
} lcd_anim_config_t;

// ── 图层元素类型 ──
typedef struct { int16_t x, y, w, h; uint16_t color; } lcd_rect_t;
typedef struct { int16_t x, y; uint8_t radius; uint16_t color; } lcd_circle_t;
typedef struct { int16_t x, y; uint16_t fg, bg;
                 uint8_t size; const char *text; } lcd_label_t;
typedef struct { int16_t x, y, w, h; uint32_t addr; } lcd_picture_t;
typedef struct { int16_t x, y, w, h;
                 uint32_t start, end; } lcd_video_t;
```

**API 速览**：

```c
void  lcd_anim_manager_init(void);
void  lcd_anim_manager_set_bg(uint16_t color);
void  lcd_anim_manager_task(void);         // 更新所有动画进度
void  lcd_anim_manager_render(void);       // 渲染所有图层 → 帧缓冲 → DMA

int8_t lcd_anim_manager_add_layer(void *ctx, lcd_layer_draw_cb_t draw_cb);
bool   lcd_anim_manager_remove_layer(int8_t id);
void   lcd_anim_manager_clear_layers(void);

int8_t lcd_anim_start(const lcd_anim_config_t *config);
bool   lcd_anim_stop(int8_t anim_id);
void   lcd_anim_stop_all(void);
```

**15 种缓动函数**：

| 枚举 | 效果 |
|:---|:---|
| `LCD_ANIM_EASE_LINEAR` | 匀速 |
| `LCD_ANIM_EASE_IN_QUAD` / `OUT_QUAD` / `IN_OUT_QUAD` | 二次方缓入/出/入出 |
| `LCD_ANIM_EASE_IN_SINE` / `OUT_SINE` / `IN_OUT_SINE` | 正弦缓入/出/入出 |
| `LCD_ANIM_EASE_IN_EXPO` / `OUT_EXPO` / `IN_OUT_EXPO` | 指数缓入/出/入出 |
| `LCD_ANIM_EASE_IN_CIRC` / `OUT_CIRC` / `IN_OUT_CIRC` | 圆形缓入/出/入出 |
| `LCD_ANIM_EASE_IN_OUT_BACK` | 回弹缓入出（超出目标再回弹） |
| `LCD_ANIM_EASE_OUT_ELASTIC` | 弹性缓出（末端弹跳） |

**使用示例**：

```c
// 注册一个矩形图层，让它在 X 轴来回弹性滑动
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

// 主循环中：
// lcd_anim_manager_task();    // 更新动画
// lcd_anim_manager_render();  // 渲染→刷新
```

---

### 🎬 MJPEG 视频播放

使用 **picojpeg** 开源库在 STM32F401 上软解 JPEG 帧。集成 **1024B 智能读缓存**减少 W25Q SPI 读取次数。从 W25Q Flash 读取 → MCU 逐帧软解 RGB565 → 写入帧缓冲。

**文件格式**：

```
Header (14 Bytes):
  [0..3]  Magic "MJPG" (0x47504A4D LE)
  [4..5]  frame_count (uint16 LE)
  [6..7]  width        (uint16 LE)
  [8..9]  height       (uint16 LE)
  [10..13] reserved

Body (frame_count 次重复):
  [frame_size (uint32 LE)] [JPEG data (frame_size bytes)]
```

**解码流程**：

1. 读取 14B 文件头，验证 Magic
2. 读取 4B 帧大小前缀 + JPEG 数据
3. 智能缓存层 → picojpeg `pjpeg_decode_mcu()` 逐 MCU 解码（8×8 块）
4. 内联 `rgb565()` 转换 → 写入帧缓冲
5. 播放完最后一帧自动回到文件头循环

```c
void lcd_play_mjpeg_video(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);
    // 每次调用解码一帧，播完循环

int8_t lcd_mjpeg_last_error(void);
const mjpeg_state_t *lcd_mjpeg_get_state(void);
    // 诊断：last_error 含错误码，frame_dump[44] 调试用
```

**错误码**：

| 值 | 宏 | 含义 |
|:---:|:---|:---|
| 255 | `MJPEG_ERR_DMA_BUSY` | DMA 忙无法读取 |
| 254 | `MJPEG_ERR_BAD_MAGIC` | 非 MJPEG 文件 |
| 253 | `MJPEG_ERR_ZERO_FRAMES` | 0 帧 |
| 251 | `MJPEG_ERR_DECODE_INIT` | picojpeg 初始化失败 |
| 250 | `MJPEG_ERR_NOT_3_COMP` | 非 3 分量 JPEG |

---

### 📦 BL 压缩视频

自研 **Block-Local 4×4 有损压缩**格式，专为嵌入式场景设计。每 4×4 像素块编码为：2 基色 + 2 插值色 + 2bit 索引。相比原始 RGB565 大幅降低存储需求。

**自动检测**：`lcd_play_video_from_w25q()` 读取前 2 字节 Magic，自动路由到 BL 解码或 MJPEG 解码或原始 RGB565 渲染路径。

```c
void lcd_play_compressed_video_from_w25q(int16_t x, int16_t y,
    int16_t width, int16_t height,
    uint32_t w25q_start_addr, uint32_t w25q_end_addr);
```

---

### 💾 嵌入式文件系统

运行在 **W25Q128 (16MB SPI Flash) + AT24C64 (8KB EEPROM)** 上的轻量级文件系统。FAT (文件分配表) 持久化于 EEPROM，意外断电不丢失。

**双区设计**：

| 特性 | 小文件区 | 大文件区 |
|:---|:---|:---|
| 分配策略 | 字节级线性挤压 | 4KB 扇区位图 |
| 最大文件数 | 32 | 32 |
| 文件名长度 | ≤ 16 字节 | ≤ 16 字节 |
| 碎片回收 | 自动压缩（分块搬移） | 删除即释放（位图清空） |
| 典型用途 | 文本、配置、元数据 | 图片、MJPEG 视频 |

**FAT 结构**（存储在 AT24C64 地址 `0x0000`）：

```c
#define FAT_MAGIC_NUMBER 0x0D000722
#define W25Q_SECTOR_SIZE 4096
#define MAX_SMALL_FILES  32
#define MAX_LARGE_FILES  32

typedef struct {
    uint32_t magic;                              // 魔数 0x0D000722
    uint32_t small_next_addr;                    // 小文件区下一分配地址
    uint16_t small_file_count;
    small_file_info_t small_files[MAX_SMALL_FILES];
    uint8_t  large_sector_bitmap[496];           // 3968 bit → 每个 bit 代表一个扇区
    uint16_t large_file_count;
    large_file_info_t large_files[MAX_LARGE_FILES];
} storage_fat_t;
```

**小文件压缩回收** (`compact_small_files`)：

当剩余空间低于阈值 (`SMALL_FILE_COMPACT_THRESHOLD = 4KB`) 时自动触发分块式压缩：

1. 按地址排序所有有效文件
2. 将与当前文件共享扇区的文件组成一批（防止扇区擦除丢失数据）
3. 将整批拷贝到保留区 (Sector 0~1, 8KB)
4. 擦除源扇区 → 从保留区读回 → 紧凑写入小文件区起始
5. 重复直到全部处理完毕 → 每批完成后立即持久化 FAT

```c
bool storage_manager_init(void);
void storage_manager_task(void);       // 主循环轮询，解析 USB 命令

bool storage_fat_load(void);
void storage_fat_save(void);           // 持久化到 AT24C

int16_t find_small_file_by_name(const char *name);
int16_t find_large_file_by_name(const char *name);
bool get_small_file_info(uint8_t id, small_file_info_t *info);
bool get_large_file_info(uint8_t id, large_file_info_t *info);

bool compact_small_files(void);        // 小文件区垃圾回收
void clear_all_files(void);            // 格式化：擦除所有扇区 + 重置 FAT

bool storage_is_downloading(void);
```

**查找文件并使用的完整示例**：

```c
// 查找图片 "pic_mp" → 显示
int16_t idx = find_large_file_by_name("pic_mp");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    lcd_draw_picture_from_w25q(0, 0, 160, 80,
        info.start_sector * W25Q_SECTOR_SIZE);
}

// 查找视频 "qwq" → 播放
int16_t vid = find_large_file_by_name("qwq");
if (vid >= 0) {
    large_file_info_t vinfo;
    get_large_file_info((uint8_t)vid, &vinfo);
    lcd_play_video_from_w25q(0, 0, 160, 80,
        vinfo.start_sector * W25Q_SECTOR_SIZE, vinfo.size);
}
```

---

### 🔌 USB 控制器

基于 **USB CDC (Virtual COM Port)** 的自定义通信协议，采用**单槽待发送机制**节省 SRAM。RX 使用 2560B 环形缓冲区，TX 分包每次 6144B。

**协议帧格式**（设备↔主机双向）：

```
┌─────────┬─────────┬──────────┬─────────────┬──────────────────┐
│  0xAA   │  0x55   │ Command  │  Length LE   │  Payload + CRC16 │
│ (1 Byte)│ (1 Byte)│ (1 Byte) │ (2 Byte)     │   (Variable)     │
└─────────┴─────────┴──────────┴─────────────┴──────────────────┘
```

```c
void usb_controller_init(usb_controller_t *ctl);
void usb_controller_task(usb_controller_t *ctl);   // 发送队列 + 超时恢复

// cmd=命令字节, data=payload 指针, len=payload 长度
// 返回 USB_SEND_OK / USB_SEND_QUEUED / USB_SEND_DROPPED_PREVIOUS
usb_send_status_t usb_controller_send(
    usb_controller_t *ctl, uint8_t cmd,
    const uint8_t *data, uint16_t len);

uint16_t usb_controller_receive(usb_controller_t *ctl,
    uint8_t *buf, uint16_t len);
uint16_t usb_controller_get_rx_free_space(void);
```

**关键设计决策**：

| 特性 | 说明 |
|:---|:---|
| **单槽待发送** | 不拷贝 payload，仅持指针，节省 SRAM |
| **RX 环形缓冲区** | 2560 字节，覆盖最大协议帧 + 余量 |
| **发送分片** | 6144 字节/次，维持 LCD 流稳定性 |
| **超时保护** | 2 秒无进展触发端点软恢复 |
| **LCD 流同步** | USB 发送完成后才渲染，防单缓冲区撕裂 |
| **中断安全** | `__disable_irq()` 保护环形缓冲头尾指针 |

---

### 🔗 SPI DMA 传输层

SPI1 (LCD) 和 SPI2 (W25Q Flash) 均支持 DMA 传输。W25Q 驱动包含完整的 **10 状态 DMA 状态机**：pending → transmitting → wait_flash_ready → done/error。

```c
bool w25q_init(void);
uint32_t w25q_read_id(void);           // JEDEC ID 0x9F: 0xEF4018

// ── 同步读写 ──
void w25q_read_data(uint32_t addr, uint8_t *data, uint32_t size);
void w25q_fast_read_data(uint32_t addr, uint8_t *data, uint32_t size);
void w25q_write_data(uint32_t addr, uint8_t *data, uint32_t size);
void w25q_page_program(uint32_t addr, uint8_t *data, uint16_t size);
void w25q_erase_sector(uint32_t addr);  // 4KB
void w25q_erase_chip(void);

// ── DMA 异步 ──
bool w25q_write_data_dma(uint32_t addr, uint8_t *data, uint32_t size);
bool w25q_read_data_dma(uint32_t addr, uint8_t *data, uint32_t size);
bool w25q_fast_read_data_dma(uint32_t addr, uint8_t *data, uint32_t size);

void w25q_dma_task(void);              // 主循环中轮询 DMA 状态机
bool w25q_dma_is_busy(void);
bool w25q_dma_is_done(void);
bool w25q_dma_is_error(void);
```

**AT24C64 EEPROM**（I2C，无 DMA）：

```c
bool at24c_write_byte(uint16_t memAddr, uint8_t *data);
bool at24c_read_byte(uint16_t memAddr, uint8_t *data);
bool at24c_write_buffer(uint16_t memAddr, uint8_t *pData, uint16_t size);
bool at24c_read_buffer(uint16_t memAddr, uint8_t *pData, uint16_t size);
// 内部自动处理 32B 页写入 + 等待就绪 (poll ACK, 最大 30ms)
```

**CRC16-USB**：

```c
// CRC-16/USB: poly=0x8005, init=0xFFFF, final XOR=0xFFFF
uint16_t crc16_usb_packing(const uint8_t *data, uint16_t len, bool has_crc);
// has_crc=false → 返回 CRC16 值
// has_crc=true  → 验证 data 末尾 2 字节是否为有效 CRC，返回 1(通过)/0(失败)
```

---

### 📊 性能监控

基于 **DWT_CYCCNT**（Cortex-M4 硬件周期计数器）的精确性能测量：

```c
void lcd_calculate_fps(void);       // 每秒帧数，无需额外定时器
void lcd_calculate_usage(void);     // CPU 占用百分比（空闲循环 vs 非空闲）

extern uint16_t lcd_fps;            // ℉PS
extern uint8_t  cpu_usage_percent;  // CPU 负载 %
```

典型性能（84MHz 实测）：
- 全屏 DMA 刷新：~0.3ms
- MJPEG 解码播放：~10-20 FPS（视压缩率）
- UI 动画渲染：~60 FPS
- CPU 空闲占用：~40-60%（含 USB 轮询 + Flash 状态机）

---

## 📡 USB 通信协议

### 主机命令帧格式

```
  [0][1]: Frame header 0xBB 0x44 (2B)
  [2]:   Command (1B)
  [3-6]: Total file size uint32 LE (4B) — 仅下载首包有效，非数据命令填 0
  [7-8]: Packet length uint16 LE (2B) = payload_len + 2 (CRC16)
  [9+]:  Payload data
  [last-2][last-1]: CRC16 (2B) over header + payload (before CRC)
```

### 命令集

| 命令 | 编码 | 方向 | 说明 |
|:---|:---:|:---|:---|
| **开始/继续下载大文件** | `0x11` | Host → Device | 传输图片/视频数据扇区位图分配 |
| **开始/继续下载小文件** | `0x45` | Host → Device | 传输文本/小数据字节线性分配 |
| **结束下载** | `0x14` | Host → Device | 传入文件名(≤16B)，注册到 FAT |
| **删除文件** | `0x19` | Host → Device | file_type(1B)+file_index(1B) |
| **查询文件列表** | `0x20` | Host → Device | 返回 TLV 格式文件目录 + 扇区碎片信息 |
| **发送位图** | `0x21` | Host → Device | 返回大文件区位图(496B) |
| **LCD 流控制** | `0x10` | Host → Device | sub_cmd=0x01 开启流/0x00 关闭流 |
| **继续发送** | `0xA1` | Device → Host | 下载确认，主机可发下一包 |
| **错误响应** | `0xE0` | Device → Host | error_type(1B)：CRC 错误/空间不足/无效索引等 |

### 传输流程

```
Host                                     Device
  │                                         │
  ├── [0x45] Data Frame + CRC16 ──────────→ │
  │                                         ├── 分配空间，擦除扇区，写入数据
  │  ← ── [0xA1] Continue ──────────────── │
  ├── [0x45] Data Frame + CRC16 ──────────→ │
  │                                         ├── 按需扩展分配，写入数据
  │  ← ── [0xA1] Continue ──────────────── │
  ├── [0x14] End Download (filename+CRC) ─→ │
  │                                         ├── 注册到 FAT → 持久化到 EEPROM
  │  ← ── [0xA1] OK / [0xE0] Error ─────── │
```

---

## 🗂️ 文件系统设计

```
W25Q128 (16MB) 分区布局
═══════════════════════════════════════════
Sector    0 ~    1 (   8KB) │ 保留区 (压缩暂存)
────────────────────────────┼────────────────────────────
Sector    2 ~   63 ( 248KB) │ 小文件区 — 字节级线性挤压
                            │ 自动压缩回收 deleted 文件空间
────────────────────────────┼────────────────────────────
Sector   64 ~ 4031 (15.5MB) │ 大文件区 — 4KB 扇区对齐位图
                            │ 496 字节 bitmap 管理 3968 个扇区
────────────────────────────┼────────────────────────────
Sector 4032 ~ 4095 ( 256KB) │ 用户自定义区
═══════════════════════════════════════════

AT24C64 (8KB EEPROM) 分配
═══════════════════════
地址 0x0000 ──→ storage_fat_t (~2KB)
═══════════════════════
```

---

## 🛠️ 上位机工具

### 🌐 Web Host — `lcd_host_web/`

基于 **Flask** 的现代 Web 应用，用于图像/视频转码与设备文件管理。

| 功能 | 说明 |
|:---|:---|
| 图片转码 | PNG/JPG/BMP/GIF → RGB565 / BL 压缩 |
| 视频转码 | MP4/WEBM/MKV/AVI/MOV → MJPEG / BL 压缩 |
| BL 压缩 | 4×4 像素块、基色+插值、2bit 索引编码 |
| 预览生成 | 转换结果实时预览 |
| 参数调节 | 分辨率、帧率、亮度、质量、字节序 |
| 自动清理 | 转换文件 5 分钟 TTL 自动删除 |
| 深色/浅色主题 | 一键切换 |

```bash
cd lcd_host_web
pip install -r requirements.txt
python server.py
# 浏览器访问 http://localhost:5000
```

**API 端点**：

| 路由 | 方法 | 用途 |
|:---|:---:|:---|
| `/convert` | POST | 上传并转换 |
| `/preview/<id>` | GET | 预览转换结果 |
| `/download/<id>` | GET | 下载二进制文件 |
| `/info/<id>` | GET | 元信息 |

### 🔧 Feature Tester — `feature_tester/`

| 工具 | 说明 |
|:---|:---|
| `sender.c` | Windows 串口发送器 — 大量数据回环验证 |
| `receiver.c` | Windows 串口接收器 — LCD 帧流实时窗口渲染 |
| `image_decoder.py` | Python 工具 — RGB565 二进制 → PNG 解码 |

---

## 🔧 环境搭建

### 硬件需求

- **STM32F401RCT6** 开发板
- **160×80 IPS TFT**（ST7735S 兼容）
- **W25Q128** SPI Flash 模块
- **AT24C64** EEPROM 模块
- USB 数据线（供电 + CDC 通信）
- 可选：编码器、LED、按钮

### 开发工具链

| 工具 | 版本 |
|:---|:---|
| STM32CubeIDE | ≥ 1.15（推荐）/ Makefile 亦可 |
| ARM GCC | `arm-none-eabi-gcc` ≥ 10.3 |
| Python | ≥ 3.9 |
| FFmpeg | ≥ 5.0（Web Host 必需） |
| OpenOCD | 可选（烧录用） |

### 固件编译

```bash
# STM32CubeIDE: 打开 stm_ips.ioc → Generate Code → Build All
# Makefile:
cd Debug && make -j4
# 输出: stm_ips.elf / stm_ips.bin
```

### 烧录

```bash
# OpenOCD (ST-Link)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program Debug/stm_ips.elf verify reset exit"

# STM32CubeProgrammer
STM32_Programmer_CLI -c port=SWD -w Debug/stm_ips.elf -rst
```

---

## 🚀 快速上手

### 1️⃣ 烧录固件

将编译好的 `stm_ips.elf` 通过 ST-Link 烧录到 STM32F401RC。

### 2️⃣ 上电

系统自动执行：
1. 初始化所有外设（SPI · I2C · USB · TIM）
2. 检测 W25Q128 Flash，读取 JEDEC ID
3. 从 AT24C64 EEPROM 加载 FAT
4. 启动 LCD UI 动画引擎 → 查找文件 "okay"(图片) 和 "teest"(视频)
5. 进入主循环

### 3️⃣ 上传内容

```bash
python lcd_host_web/server.py
# 浏览器 http://localhost:5000
# 拖入图片/视频 → 调整参数 → 转换 → 下载
# 通过 USB CDC 串口发送到设备
```

### 4️⃣ 播放

固件启动时自动查找文件并显示/播放。可通过修改 `lcd_ui.c` 中的文件名和 `main.c` 的业务逻辑自定义启动行为。

---

## 👨‍💻 开发指南

### 添加新的 UI 元素

```c
// 1. 声明元素
static lcd_rect_t g_rect = {10, 30, 20, 15, CYAN};

// 2. 注册到动画管理器
lcd_anim_manager_add_layer(&g_rect, lcd_draw_rect_layer);

// 3. 启动动画
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

### 文件系统操作

```c
// 查找文件
int16_t idx = find_large_file_by_name("my_image");
if (idx >= 0) {
    large_file_info_t info;
    get_large_file_info((uint8_t)idx, &info);
    // start_sector × 4096 = 起始地址
    // info.size = 文件大小
}

// 格式化（谨慎调用，建议通过 USB 协议操作）
// clear_all_files();
```

### CRC16 校验

```c
crc16_usb_init_table();  // 初始化查找表（静态 const，调用无害）

// 计算 CRC（不含尾部占位）
uint16_t crc = crc16_usb_packing(data, len, false);

// 追加到数据末尾（小端）
// data[len]   = crc & 0xFF;
// data[len+1] = (crc >> 8) & 0xFF;

// 验证
// bool ok = crc16_usb_packing(frame, frame_len, true);
```

### 系统初始化序列

```c
void main(void) {
    HAL_Init();  SystemClock_Config();

    MX_GPIO_Init();  MX_DMA_Init();
    MX_I2C1_Init();  MX_SPI1_Init();  MX_SPI2_Init();
    MX_USB_DEVICE_Init();  MX_TIM2_Init();  MX_TIM3_Init();
    MX_TIM9_Init();  MX_TIM4_Init();

    usb_controller_init(&g_usb_controller);
    lcd_init();
    w25q_init();                        // 返回 false → W25Q 不可用
    storage_manager_init();             // 返回 false → FAT 初始化新表
    // clear_all_files();               // 可选格式化
    lcd_ui_init();

    while (1) {
        lcd_ui_updater();
        w25q_dma_task();
        storage_manager_task();
        usb_controller_task(&g_usb_controller);
    }
}
```

### 性能优化建议

| 策略 | 方法 |
|:---|:---|
| **DMA 优先** | 使用 `lcd_screen_update_dma()` 而非轮询，释放 CPU |
| **减少动画数** | 调低 `LCD_ANIM_MAX_COUNT` / `LCD_LAYER_MAX_COUNT` 节省 RAM |
| **SPI 时钟** | MX 配置中提高 SPI1/SPI2 时钟提升帧率 |
| **解码缓存** | MJPEG 512B 缓存，可调整平衡 RAM 与性能 |
| **缓冲区策略** | `LCD_USB_STREAM_ENABLE` 开启后 USB TX 完成才渲染，防撕裂 |

---

## 📜 许可

Copyright (C) 2026 **UnikoZera**

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU Affero General Public License** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

**第三方组件**：

| 组件 | 许可 | 作者 |
|:---|:---|:---|
| picojpeg | Public domain | Rich Geldreich |
| STM32 HAL | STMicroelectronics SLA | STMicroelectronics |
| CMSIS | Apache 2.0 | ARM |

> 基于 STM32CubeMX 生成框架开发。

---

<p align="center">
  <sub>Built with ❤️ by UnikoZera · STM32F401RC · 160×80 IPS · 2026</sub>
</p>
