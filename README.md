# STM IPS

[English](README_EN.md) · [协议契约](lcd_host_web/protocol/README.md) · [协作说明](AGENT.md)

STM IPS 是一个面向 **STM32F401RCT6** 和 **160 × 80 IPS 屏**的嵌入式媒体显示项目。固件通过 USB CDC 接收主机发送的文件，将其保存到 W25Q128 Flash，并可从 Flash 显示 RGB565 图像或播放 RAW5/MJPEG 媒体。仓库同时提供浏览器端 Host：转换图片/视频、通过 Web Serial 烧录、管理设备文件，以及预览 LCD 数据流。

> 本 README 以当前源码、`lcd_host_web/protocol/contract.json` 和测试为准。它描述的是当前默认配置；接线、资源文件名或协议变化后，请同时更新本文档。

## 功能概览

- SPI1 + DMA 驱动横屏 160 × 80 IPS LCD，提供图元、文本、图片、动画图层和性能信息绘制。
- SPI2 连接 W25Q128（16 MiB）外置 Flash；I2C1 连接 AT24C64，用于持久化文件分配表（FAT）。
- USB OTG FS 以 CDC 虚拟串口工作，使用带 CRC-16/USB 的自定义二进制协议传输与管理文件。
- 大文件采用连续扇区和位图分配；小文件采用线性追加分配，并在剩余空间较低时尝试压缩整理。
- Host 提供 FastAPI 转换服务和 Web Serial 页面，支持 RAW5 RGB565 与 MJPEG 容器、烧录/中止、文件列表、删除、位图查询和 LCD 流预览。
- 设备主界面默认查找名为 `photo_t` 的大文件作为静态图片；默认视频名为空，因此不会自动绑定视频。该行为由 `Core/Src/lcd_ui.c` 中的 `UI_PICTURE_NAME` / `UI_VIDEO_NAME` 决定。

## 硬件与默认连接

| 项目 | 当前配置 |
| --- | --- |
| MCU | STM32F401RCT6，Cortex-M4F，84 MHz，LQFP64 |
| LCD | 160 × 80 IPS，`USE_HORIZONTAL = 2`（横屏） |
| LCD 数据接口 | SPI1：PA5 SCK、PA6 MISO、PA7 MOSI；PB0 RES、PB1 DC、PB2 CS；PA3 / TIM9 CH2 背光 PWM |
| 外置 Flash | W25Q128，SPI2：PB13 SCK、PB14 MISO、PB15 MOSI、PA8 CS |
| EEPROM | AT24C64，I2C1：PB8 SCL、PB9 SDA |
| USB | USB OTG FS Device / CDC：PA11 DM、PA12 DP；PB12 `USB_EN` |
| 调试 | SWD：PA13 SWDIO、PA14 SWCLK |
| 人机输入 | TIM2 编码器：PA0 / PA1；PA2 编码器按键；PC15 按键 |

时钟由 25 MHz HSE 经 PLL 生成：SYSCLK/HCLK 84 MHz，USB 时钟 48 MHz。工程 CubeMX 配置位于 [`stm_ips.ioc`](stm_ips.ioc)。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `Core/` | 应用固件：LCD/UI、动画、RAW5/MJPEG 播放、W25Q、AT24C、存储管理和 USB 协议处理 |
| `USB_DEVICE/` | STM32 USB Device CDC 配置与回调 |
| `Drivers/`、`Middlewares/` | STM32 HAL、CMSIS、USB Device、ARM DSP 等依赖 |
| `lcd_host_web/` | FastAPI 转码服务、Web Serial 前端、协议契约与 Python/TypeScript 测试 |
| `feature_tester/` | 串口收发与 RGB565 辅助工具 |
| `Debug/` | STM32CubeIDE 生成的 GNU Make 构建目录及构建产物；不要手改生成文件 |

## 固件运行方式

初始化顺序为 GPIO、DMA、I2C、SPI、USB、定时器，然后初始化 USB 控制器、LCD、W25Q、存储管理器和 UI。主循环持续执行：

```text
lcd_ui_updater()
  -> w25q_dma_task()          # W25Q DMA / 异步擦除状态机
  -> storage_manager_task()   # 解析 Host 帧并执行存储操作
  -> usb_controller_task()    # USB CDC 收发
```

`w25q_dma_task()` 与 `usb_controller_task()` 是传输期间保持响应的关键，不应在中断服务程序中执行长时间擦除、编程或等待操作。

若 W25Q 初始化失败，屏幕会显示 `W25Q FAIL`。AT24C 中没有有效 FAT 时，固件会创建默认 FAT，但 `storage_manager_init()` 会返回失败；因此首次初始化后需要**复位一次**，文件管理功能才会进入正常工作状态。

## Flash 分区与文件系统

W25Q128 共 4096 个、每个 4 KiB 的扇区：

| 区域 | 扇区 | 容量 | 用途 |
| --- | ---: | ---: | --- |
| 保留区 | 0–3 | 16 KiB | 小文件压缩时的暂存区 |
| 小文件区 | 4–63 | 240 KiB | 线性追加分配 |
| 大文件区 | 64–4031 | 15.5 MiB | 连续扇区分配，496 字节位图管理 |
| 用户区 | 4032–4095 | 256 KiB | 当前存储管理器未使用 |

FAT 位于 AT24C64 地址 `0x0000`，当前魔数为 `0x0D000721`。最多记录 40 个小文件和 35 个大文件；文件名存储容量为 16 字节，Host 结束帧会截断为最多 15 个字节并补 `\0`。

下载数据按 CRC 块保存：每个完整块为 **1022 字节数据 + 2 字节 CRC-16/USB**，即 Flash 中 1024 字节物理块。尾块用 `0xFF` 填充到 1022 字节后重新计算其存储 CRC。大文件会根据首包声明的原始大小预分配连续扇区；小文件只向高地址追加。删除或中止大文件会擦除已分配扇区并释放位图；小文件删除不会回退追加指针，剩余空间少于 16 KiB 时会尝试压缩。

> 下载路径默认依赖已擦除的可用区域，并不为每次写入执行预擦除。请勿在不理解影响的情况下调用 `clear_all_files_manual()`；它会清除受管理的文件区并重置 FAT。

## 媒体格式与启动资源

Host 当前生成以下 14 字节头的容器：

```text
magic[4] | frame_count[u16le] | width[u16le] | height[u16le] | reserved[4]
```

- `RAW5`：头之后为连续的**大端 RGB565** 帧；用于单张图片或原始视频帧序列。
- `MJPG`：头之后重复 `jpeg_length[u32le] | JPEG bytes`；固件用 picojpeg 解码。

固件也能兼容无头的原始 RGB565 数据。播放时会自动识别 `MJPG` 和 `RAW5`；文件尺寸和画面尺寸应适配 160 × 80 显示区域。上传成功的 `0x14` 结束命令会重新调用 `lcd_ui_init()`，因此配置名称匹配的媒体会立刻重新绑定。

修改启动资源时，在 `Core/Src/lcd_ui.c` 中调整：

```c
#define UI_VIDEO_NAME    ""        /* 为空时默认不播放视频 */
#define UI_PICTURE_NAME  "photo_t" /* 无视频时显示的图片 */
```

主界面优先视频、其次图片、最后显示 `NO MEDIA`。Host 的快捷默认名可能是 `vp_vid` / `vp_img`，烧录前应改成与上述宏一致的名称。

## USB CDC 协议

完整的机器可读契约位于 [`lcd_host_web/protocol/contract.json`](lcd_host_web/protocol/contract.json)。以下为实现摘要。

Host → MCU：

```text
BB 44 | command[u8] | total_size[u32le] | payload_length[u16le] | data | crc16[u16le]
```

其中帧头长度为 9 字节，`payload_length = data_length + 2`。CRC-16/USB **只覆盖 `data`**，不覆盖帧头、命令、大小字段或 CRC 本身；空数据的 CRC 是 `0x0000`。单帧最多 1024 字节数据。

MCU → Host：

```text
AA 55 | command[u8] | payload_length[u16le] | payload
```

| 命令 | 方向 | 含义 |
| --- | --- | --- |
| `0x11` | Host → MCU | 大文件数据 |
| `0x45` | Host → MCU | 小文件数据 |
| `0x14` | Host → MCU | 完成下载并提交文件名 |
| `0x15` | Host → MCU | 中止下载并回滚 |
| `0x19` | Host → MCU | 删除文件：`[file_type, file_index]` |
| `0x20` | Host → MCU | 查询文件列表及已分配大文件区间 |
| `0x21` | Host → MCU | 查询大文件区位图 |
| `0x10` | 双向 | LCD 流控制：`0x00` 停止，`0x01` 开启 |
| `0xA1` | MCU → Host | 可发送下一帧 / 操作成功 |
| `0xE0` | MCU → Host | 错误，负载为一字节错误码 |
| `0xA0` | MCU → Host | LCD RGB565 数据流 |

下载开始时 LCD 流会临时关闭，结束或中止时恢复原状态。下载中 15 秒没有新的数据包会自动中止并报告 `0x0B`。协议变更必须同时更新固件、`contract.json`、生成的前端契约和测试。

## 构建与烧录固件

推荐使用 STM32CubeIDE 打开此目录并构建 Debug 配置。也可使用项目自带的 GNU Make 文件；它当前由 GNU Tools for STM32 14.3.rel1 生成。

```powershell
where arm-none-eabi-gcc
where make
make -C Debug -j4
```

输出包括 `Debug/stm_ips.elf`。例如使用 OpenOCD 烧录：

```powershell
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program Debug/stm_ips.elf verify reset exit"
```

也可使用 STM32CubeProgrammer 或 CubeIDE 的下载功能。构建目录由 CubeIDE 生成，若重新生成工程，以 `stm_ips.ioc` 和源码为准。

## 使用 Host

Host 依赖 Python、FFmpeg 和支持 Web Serial 的 Chromium 浏览器（Chrome 或 Edge）。项目约定使用 Conda 环境 **`py312-classic`**，不要向 Conda `base` 环境安装项目依赖。

```powershell
conda activate py312-classic
cd lcd_host_web
python -m pip install -r requirements.txt
python server.py
```

默认地址为 <http://127.0.0.1:5000>。服务会优先使用系统 `ffmpeg` / `ffprobe`，不可用时尝试 `imageio-ffmpeg` 提供的可执行文件。`launcher.py` 可自动选择 5000 起的可用本地端口并打开浏览器；`STM_IPS_Host.bat` 为 Windows 启动入口。

基本流程：

1. 构建并烧录固件，首次创建 FAT 后复位一次。
2. 在 `py312-classic` 环境启动 Host，并用 Chrome/Edge 打开页面。
3. 通过 Web Serial 选择设备的 CDC 串口。
4. 转换图片/视频，确认文件名与 `lcd_ui.c` 的资源名一致。
5. 选择大文件（通常用于媒体）或小文件，发送并等待每个 `0xA1` 确认及最终 `0x14` 完成。
6. 使用列表、删除和位图功能检查写入结果；开启 LCD 流可预览设备输出。

## 开发与验证

Host 的 Python 依赖和开发测试依赖分别在 `lcd_host_web/requirements.txt`、`lcd_host_web/requirements-dev.txt` 中；前端检查由 `lcd_host_web/package.json` 提供。

```powershell
conda activate py312-classic
python -m pytest -q .\lcd_host_web\tests

cd lcd_host_web
npm ci
npm run typecheck
npm test
npm run build
```

涉及存储、协议或 USB 的改动还应在硬件上至少验证：首次 FAT 建表后的复位、大/小文件烧录、取消下载、删除后重新分配、文件列表/位图，以及下载期间 LCD 流暂停与恢复。静态测试不能替代 Flash、DMA 和 USB 的实机验证。

## 许可证

本项目采用 [MIT License](LICENSE)，Copyright (c) 2026 UnikoZera。STM32 HAL、CMSIS、USB Device 和 ARM DSP 等第三方组件分别遵循其随附许可证。
