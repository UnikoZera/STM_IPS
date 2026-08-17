# ⚡ STM IPS

> **STM32F401RC 驱动的 160×80 IPS 嵌入式多媒体平台**  
> **Display · Animation · MJPEG Video · USB CDC · Embedded File System**

![STM32](https://img.shields.io/badge/MCU-STM32F401RCT6-03234B?logo=arm)
![Display](https://img.shields.io/badge/Display-160%C3%9780_IPS-00BCD4)
![Protocol](https://img.shields.io/badge/Protocol-USB_CDC_Custom-7C4DFF)
![License](https://img.shields.io/badge/License-MIT-blue)
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
- [文档与闭环说明](#-文档与闭环说明)
- [许可](#-许可)

---

## 🔭 系统概览

**STM IPS** 运行于 **STM32F401RC**（Cortex-M4，84MHz），以 **160×80 IPS 彩色 TFT** 为输出核心，通过 **SPI DMA** 做全屏刷新，并集成：

- 自研 **动画引擎** — 多种缓动、并发动画与分层渲染
- **picojpeg 软解** MJPEG 播放 + 自研 **BL 4×4** 压缩视频
- **轻量文件系统** — 小文件线性分配 / 大文件扇区位图，FAT 落在 EEPROM
- **USB CDC 自定义协议** — 文件烧录、删除、列表、位图查询、LCD 实时流、传输中止回滚

| 组件 | 技术栈 | 用途 |
|:---|:---|:---|
| `Core/` | C (STM32 HAL) | LCD、动画、视频解码、存储管理、USB 协议 |
| `USB_DEVICE/` | STM32 USB Device | USB CDC 虚拟串口 |
| `lcd_host_web/` | Python FastAPI + HTML5 + Web Serial | 转码、串口烧录、文件管理、位图可视化 |
| `feature_tester/` | C + Python | 串口回环 / RGB565 校验 |
| `docs/` | Markdown | 上下位机存储协议与流程说明 |

---

## ⚙️ 硬件架构

### 主控

| 参数 | 规格 |
|:---|:---|
| **MCU** | STM32F401RCT6，Cortex-M4 FPU，84MHz |
| **Flash / SRAM** | 256KB / 64KB |
| **封装** | LQFP64 |

### 外设连接

```
┌──────────────────────────────────────────────────────────────────┐
│                         STM32F401RC                              │
│                                                                  │
│   SPI1 ── DMA ───→ ST7735S 160×80 IPS LCD                        │
│   ├─ SCK/MISO/MOSI · RES/DC/CS · BL_PWM(TIM)                     │
│                                                                  │
│   SPI2 ── DMA ───→ W25Q128 16MB Flash                            │
│   └─ 4096 × 4KB sector，Page Program 256B                        │
│                                                                  │
│   I2C1 ──────────→ AT24C64 EEPROM（FAT 持久化）                   │
│                                                                  │
│   USB_OTG_FS ────→ USB CDC Virtual COM Port                      │
│                                                                  │
│   TIM2 / TIM3 / TIM4 / TIM9 ── 定时 / PWM                        │
└──────────────────────────────────────────────────────────────────┘
```

### W25Q 存储分区

| 区域 | 扇区 | 大小 | 策略 |
|:---|:---:|:---:|:---|
| **保留区** | 0 ~ 1 | 8 KB | 小文件 compact 暂存 |
| **小文件区** | 2 ~ 63 | 248 KB | 字节级线性挤压 + 条件压缩回收 |
| **大文件区** | 64 ~ 4031 | 15.5 MB | 4KB 扇区位图（496 字节 bitmap） |
| **用户区** | 4032 ~ 4095 | 256 KB | 预留 |

> FAT（`storage_fat_t`）持久化在 AT24C EEPROM 起始地址 `0x0000`，魔数 `0x0D000722`。

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

### 主循环

```
main()
 ├── HAL + 时钟 + GPIO/DMA/SPI/I2C/USB/TIM 初始化
 ├── usb_controller_init()
 ├── lcd_init()
 ├── w25q_init()                 // 失败则主循环提示 W25Q FAIL
 ├── storage_manager_init()      // 从 AT24C 加载 FAT；失败提示 STORAGE FAIL
 ├── lcd_ui_init()               // 按文件名绑定资源 + 图层
 └── while(1)
      ├── lcd_ui_updater()
      ├── w25q_dma_task()        // 仅 w25q_ok
      ├── storage_manager_task() // 仅 storage_ok：解析 Host 帧
      └── usb_controller_task()
```

> 首次 FAT magic 不匹配时会写入默认表并返回失败，**当次上电可能不跑存储任务**，复位一次后正常。调试可用 `clear_all_files()`（会擦除文件区，务必谨慎）。

---

## ✨ 核心功能

### 🖥️ 显示系统

160×80 IPS，**SPI1 + DMA** 驱动；默认横屏 `LCD_W=160`、`LCD_H=80`（`USE_HORIZONTAL` 可改）。

```c
void lcd_fill_screen(uint16_t color);
void lcd_draw_point / line / rectangle / circle / string(...);
void lcd_screen_update_dma(void);
void lcd_draw_picture_dma(...);
void lcd_draw_picture_from_w25q(...);
void lcd_play_video_from_w25q(...);   // 自动识别 MJPEG / BL / 原始 RGB565

// USB 实时流：lcd_usb_stream_enabled 且非下载中时推送帧（cmd 0xA0）
void lcd_calculate_fps(void);
void lcd_calculate_usage(void);
```

烧录进行中 `storage_is_downloading()==true` 时会抑制 LCD USB 流，避免与文件传输抢带宽。

### 🎞️ 动画引擎

并发动画 + 图层渲染，缓动可插拔（线性 / Quad / Sine / Expo / Circ / Back / Elastic 等）。

```c
lcd_anim_manager_init();
lcd_anim_manager_add_layer(ctx, draw_cb);
lcd_anim_start(&config);     // target / duration / yoyo / path_cb ...
lcd_anim_manager_task();
lcd_anim_manager_render();
```

当前 `lcd_ui_init()` 默认挂载 **FPS/占用标签** 与 **视频图层**；图片图层可按需取消注释。

### 🎬 MJPEG 视频播放

**picojpeg** 软解 + Flash 读缓存。容器头 14B：`MJPG` + frame_count/width/height。

### 📦 BL 压缩视频

4×4 块：双基色 + 索引位图。`lcd_play_video_from_w25q()` 读 Magic 自动路由。

### 💾 嵌入式文件系统

| 特性 | 小文件区 (`0x45`) | 大文件区 (`0x11`) |
|:---|:---|:---|
| 分配 | `small_next_addr` 线性增长 | 连续空闲扇区 + bitmap |
| 槽位 | 最多 32 | 最多 32 |
| 文件名 | ≤16 字节 | ≤16 字节 |
| 删除 | `is_valid=0`；空间不足时 compact | 擦扇区 + 清 bitmap |
| 中止/失败 | **不**回退 next（防脏尾） | erase + 释放 bitmap |
| 典型用途 | 配置、小资源 | 图片、视频 |

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

**资源绑定示例（以 `lcd_ui.c` 为准，可改）**：

```c
// 大文件名 "photo_t" → 图片（默认）
int16_t idx = find_large_file_by_name("photo_t");
// 大文件名：当前仓库示例为 "fff"；上位机 VP 常默认 "vp_vid"
// 二者必须一致，否则列表有文件但不会挂到播放层
int16_t vid = find_large_file_by_name("fff");
// 地址 = start_sector * 4096；仅在 lcd_ui_init 绑定一次
```

**小文件 compact 要点**：

- 触发：删除小文件后，若 `END - small_next_addr < 4096`
- 流程：有效文件按地址排序 → 按共享 4KB 扇区分批 → 拷入 8KB 保留区 → 擦原扇区 → 从头紧凑写回 → 擦旧高水位尾巴 → 更新 `small_next_addr`
- 单批有效数据合计须 ≤ 8KB，否则 compact 失败

### 🔌 USB 控制器

CDC 上自定义帧。设备→主机：

```
[0xAA][0x55][CMD][LEN LE 2B][PAYLOAD...]
```

主机→设备（存储管理解析）：

```
[0xBB][0x44][CMD][TOTAL_SIZE LE 4B][PAYLOAD_LEN LE 2B][DATA...][CRC16 LE]
```

CRC：**CRC-16/USB**（与上位机 `lcd_host_web/static/js/protocol.js` 一致）。

### 🔗 SPI DMA / EEPROM / CRC

- W25Q：同步读写 + DMA 写/读状态机（`w25q_dma_task`）
  - SPI 经统一封装：`w25q_spi_transmit` / `transmit_receive` / DMA 变体，**先 flush RX FIFO** 再访问
  - 同步读优先全双工 `TransmitReceive`（命令与 dummy 一并发送）
  - 页编程在拉高 CS 前等待 SPI BSY，降低锁存不完整风险
- AT24C：页写缓冲 API（无 DMA）
- `crc16_usb_packing(data, len, has_crc)`

### 📊 性能监控

基于 **DWT_CYCCNT** 的 FPS 与 CPU 占用估算。

---

## 📡 USB 通信协议

### 主机下行帧

```
[0][1]  0xBB 0x44
[2]     CMD
[3..6]  total_file_size uint32 LE   // 数据首包填完整文件大小，其余常 0
[7..8]  payload_len uint16 LE       // = data_len + 2
[9..]   payload
[尾2B]  CRC16/USB 小端（覆盖 BB 起至 data 末，不含 CRC）
```

### 命令集

| 命令 | 编码 | 方向 | 说明 |
|:---|:---:|:---|:---|
| 大文件数据 | `0x11` | H→D | 首包预分配扇区并写入；后续续传 |
| 小文件数据 | `0x45` | H→D | 首包推进 `small_next_addr` 并写入 |
| 结束下载 | `0x14` | H→D | 文件名 ≤16B，登记 FAT；槽满回 `0x06`/`0x07` |
| **中止下载** | **`0x15`** | H→D | 取消/超时：大文件擦除并释放位图 |
| 删除文件 | `0x19` | H→D | `[file_type, file_index]`，type=`0x11`/`0x45` |
| 查询列表 | `0x20` | H→D | TLV：entry_count + slot_count + records |
| 查询位图 | `0x21` | H→D | 大文件区 bitmap 496B |
| LCD 流控 | `0x10` | H→D | `0x01` 开 / `0x00` 关 / 空 payload 查询 |
| 继续 | `0xA1` | D→H | 可发下一包 / 中止确认 |
| 错误 | `0xE0` | D→H | 1 字节错误码 |
| LCD 帧 | `0xA0` | D→H | RGB565 画面流 |

### 常见错误码

| 码 | 含义 |
|:---:|:---|
| `0x01` | CRC 错误 |
| `0x02` | 删除类型未知 |
| `0x03` | 大文件区无连续空间 |
| `0x04` | 小文件区空间不足 |
| `0x05` | 续传类型不匹配 |
| `0x06` | 大文件目录槽满 |
| `0x07` | 小文件目录槽满 |
| `0x08` | 删除索引无效 |
| `0x09` | 未知命令 |
| `0x0B` | Flash 写入失败 |

### 烧录时序（简图）

```
Host                              Device
 |-- 0x11/0x45 首包 total_size -->| 分配 + 写 → 0xA1
 |-- 后续数据包 ------------------>| 写 → 0xA1
 |-- 0x14 filename --------------->| 登记 FAT → 0xA1
 |-- 0x20 列表 ------------------->| TLV 目录
 |  (取消) 0x15 ------------------>| 回滚（大文件擦扇区）→ 0xA1
```

设备侧另有 **约 5s** 下载无新包自动 `abort_download_common()` 保护。

### 0x20 列表 TLV（摘要）

```
[entry_count][slot_count][slot_records...][file_records...]
slot: rLen=10, tag=0xFF, start_sector, sector_count   // 大文件 used 连续块
file: rLen, tag(bit7: 0=small/1=large), index, name_len, name, addr/sector, size
      large 另附 sector_count；small rLen=12+name_len，large rLen=16+name_len
```

---

## 🗂️ 文件系统设计

```
W25Q128 (16MB)
═══════════════════════════════════════════
S0~1     8KB    保留区（compact 中转）
S2~63    248KB  小文件区 — 线性 next + 条件 compact
S64~4031 15.5MB 大文件区 — bitmap 连续分配
S4032~4095 256KB 用户区
═══════════════════════════════════════════
AT24C  @0x0000  storage_fat_t（magic / next / counts / tables / bitmap）
```

**设计不变量与写路径（当前实现立场，改代码后请同步本文）**：

- 大文件空闲扇区在**删除/中止**路径上 **先 erase 再清 bit**。
- **下载烧录路径默认不写前擦除**（依赖删除时已擦净；复用脏扇区或异常断电后可能花屏/校验失败）。
- `flash_write_and_verify`：**优先 `w25q_write_data_dma`**；DMA 启动成功则本包**立即返回成功且不回读**，下一块在函数入口等待 DMA；仅同步回退路径做 `memcmp`。写失败上报 `0x0B`。
- 小文件只在 `small_next_addr` 之后追加；删除不回退 next。
- 大文件中止/槽满：释放已分配扇区；小文件中止/槽满：保持 next，死区靠后续 compact。
- 下载约 **5s** 无新包自动中止回滚。

---

## 🛠️ 上位机工具

### 🌐 `lcd_host_web/`

FastAPI 转码服务 + **浏览器 Web Serial** 主机页（`index.html`，前端已拆分至 `static/`）。

| 能力 | 说明 |
|:---|:---|
| 图片/视频转码 | → RGB565 / BL / MJPEG 等 |
| 串口烧录 | `0x11`/`0x45` 分包 + `0x14` 结束 |
| 取消传输 | `0x15` 通知 MCU 回滚 |
| 文件列表/删除 | `0x20` / `0x19` |
| Flash 位图 | `0x21` 可视化大文件区占用 |
| LCD 流 | `0x10` + 接收 `0xA0` 预览 |
| VP 快捷烧录 | 上位机常默认名 `vp_vid`；需与 `lcd_ui.c` 绑定名一致 |

```bash
cd lcd_host_web
pip install -r requirements.txt
python server.py
# 浏览器打开页面（Chrome/Edge + Web Serial）
# 也可使用打包入口 STM_IPS_Host.bat / launcher.py
```

> 需 **FFmpeg** 做视频管线。详细协议时序见 `docs/storage_host_mcu_flow.md`。

### 🔧 `feature_tester/`

串口发送/接收与 RGB565 解码辅助工具。

---

## 🔧 环境搭建

| 项目 | 要求 |
|:---|:---|
| IDE | STM32CubeIDE ≥ 1.15（或 Makefile + arm-none-eabi-gcc） |
| Python | ≥ 3.9（上位机） |
| FFmpeg | ≥ 5.0（Web Host 转码） |
| 调试器 | ST-Link / OpenOCD / CubeProgrammer |

```bash
# 编译（CubeIDE Build，或）
cd Debug && make -j4

# 烧录示例
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program Debug/stm_ips.elf verify reset exit"
```

---

## 🚀 快速上手

1. **烧录固件**到 STM32F401RC  
2. **上电**：初始化外设 → 加载 FAT → `lcd_ui` 按配置查找图片/视频文件名  
3. **启动上位机** `lcd_host_web`，连接 CDC 串口  
4. **转码并烧录**图片/视频（大文件 `0x11`，小文件 `0x45`）；文件名与 `lcd_ui.c` 一致  
5. **刷新列表**确认；**复位一次**后再看播放（绑定仅在 `lcd_ui_init`）  

若列表无响应：确认 `storage_ok`（必要时复位完成 FAT 建表），并关闭冲突串口占用。  
若列表有、不播：检查绑定名、容器 magic（`MJPG`/`BL`）、以及 Flash 是否真正写成功。

---

## 👨‍💻 开发指南

### 改启动资源名

编辑 `Core/Src/lcd_ui.c` 中 `find_large_file_by_name("...")`，与上位机烧录文件名一致。

### 主循环勿阻塞

长时间擦写在协议处理路径内；Flash DMA 进度依赖 `w25q_dma_task()`；等待 DMA 时宜继续 `usb_controller_task()`，勿在中断里做重活。

### 格式化

```c
// clear_all_files();  // 擦大文件 used 扇区 + 全小文件区 + 重置 FAT
```

### CRC

```c
uint16_t crc = crc16_usb_packing(data, len, false); // 计算
// crc16_usb_packing(frame, frame_len, true);       // 校验 → 1/0
```

### 给自动化 / AI

见仓库根目录 [`AGENT.md`](./AGENT.md)：编译入口、协议注意点、故障速查、验证清单。

---

## 📘 文档与闭环说明

| 文档 | 内容 |
|:---|:---|
| [`AGENT.md`](./AGENT.md) | 面向协作者/AI 的操作手册与改动边界 |
| [`docs/storage_host_mcu_flow.md`](./docs/storage_host_mcu_flow.md) | Host↔MCU 存储协议、烧录/删除/中止流程与写路径说明 |

当前实现已覆盖：分包 ACK、结束登记、槽满错误、写失败 `0x0B`（同步路径校验失败）、中止 `0x15`、下载超时保护、列表/位图查询。

**已知边角 / 风险（实现现状）**：

- DMA 写成功路径**不逐包回读**，坏数据仍可能 `0xA1`  
- 下载路径**默认不写前擦除**  
- 同名文件不自动覆盖；删除无独立成功 ACK（依赖后续查询）  
- 上位机结束帧超时策略以实现为准  

文档与源码冲突时以 **`Core/Src` 当前源码** 为准。

---

## 📜 许可

Copyright (C) 2026 **UnikoZera**

本程序在 **GNU Affero General Public License v3**（或更高版本）下发布。

**第三方**：

| 组件 | 许可 | 说明 |
|:---|:---|:---|
| picojpeg | Public domain | Rich Geldreich |
| STM32 HAL | ST SLA | STMicroelectronics |
| CMSIS | Apache 2.0 | ARM |

> 基于 STM32CubeMX 生成框架开发。

---

<p align="center">
  <sub>Built with ❤️ by UnikoZera · STM32F401RC · 160×80 IPS · 2026</sub>
</p>
