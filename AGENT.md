# AGENT.md

面向 AI / 自动化工具与人类协作者的**仓库操作手册**。产品说明见 `README.md` / `README_EN.md`。

## 项目概览

基于 **STM32F401RCT6** 的嵌入式多媒体工程：

| 能力 | 说明 |
|:---|:---|
| 显示 | 160×80 IPS（ST7735 类），SPI1 + DMA |
| 视频 | MJPEG（picojpeg）/ BL 块压缩 / 原始 RGB565 |
| 存储 | W25Q128（SPI2）+ AT24C64（I2C，FAT） |
| 主机 | USB CDC 自定义帧；`lcd_host_web` Electron 上位机 |

目录：

| 路径 | 内容 |
|:---|:---|
| `Core/` | 固件：LCD、动画、存储、W25Q、USB 协议、主循环 |
| `USB_DEVICE/` | STM32 USB CDC |
| `lcd_host_web/` | Electron + Svelte + 本地 FFmpeg 上位机 |
| `feature_tester/` | 串口 / RGB565 辅助测试 |
| `Debug/` | Makefile 构建产物（勿手改生成物） |

## 先看什么

改代码前优先阅读：

1. `README.md` / `README_EN.md` — 架构与协议总览  
2. `Debug/makefile` + `STM32F401RCTX_FLASH.ld` — 编译与链接  
3. `Core/Src/main.c` — 初始化与主循环  
4. `Core/Src/storage_manager.c` — Host 协议与 FAT  
5. `Core/Src/w25q_controller.c` — Flash SPI/DMA  
6. `Core/Src/usb_controller.c` — CDC 组帧发送  
7. `Core/Src/lcd_ui.c` — 启动资源绑定（文件名）  
8. `lcd_host_web/src/lib/protocol.js` + `electron/media-service.cjs` — 上位机协议与转码格式

## 关键模块与数据流

```
Host (Electron Web Serial)
  BB44 帧 ──► storage_manager_task() 解析
                 │
                 ├─► flash_write_and_verify / FAT (AT24C)
                 ├─► w25q_* (SPI2)
                 └─► usb_controller_send (AA55 应答)

lcd_ui_init
  find_large_file_by_name ──► start_sector*4096 ──► 播放层读 W25Q
```

主循环（`main.c`）顺序大致为：

```
lcd_ui_updater → w25q_dma_task → storage_manager_task → usb_controller_task
```

- **W25Q 失败**：屏显 `W25Q FAIL`，烧录/播放不可用。  
- **FAT 加载失败**：屏显 `STORAGE FAIL`；首次 magic 不匹配会写默认表，**可能需复位一次**后 `storage_ok` 才为真。  

## 环境要求

### 固件

- GNU Arm Embedded Toolchain（`arm-none-eabi-gcc` 等）  
- GNU Make  
- 可选：STM32CubeIDE、ST-Link / OpenOCD  

### 上位机

- Node.js + pnpm
- FFmpeg（`lcd_host_web` 视频转码）

### Windows 检查

```powershell
where arm-none-eabi-gcc
where make
where node
where pnpm
where ffmpeg
```

## 自动编译

```powershell
cd Debug
make -j4
```

成功时常见产物：

- `Debug/stm_ips.elf`  
- `Debug/stm_ips.map`  
- `Debug/stm_ips.list`  

说明：

- 规则来自 `Debug/makefile`  
- 链接脚本：`STM32F401RCTX_FLASH.ld`  
- 目标：STM32F401RCT6 / Cortex-M4  

工具链不可用时：**报告环境问题**，不要假装编译成功。

## 烧录建议

```powershell
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program Debug/stm_ips.elf verify reset exit"
```

亦可用 CubeProgrammer / CubeIDE Download。

## 上位机

```powershell
cd lcd_host_web/desktop
pnpm install
pnpm dev
```

桌面应用通过 Electron Web Serial 访问 CDC；转码在主进程调用 FFmpeg，不依赖 Python 或 Flask。

## 协议与存储（改前必读）

### 帧格式

| 方向 | 格式 |
|:---|:---|
| Host→MCU | `BB 44` + CMD + total_size(4 LE) + payload_len(2 LE) + data + CRC16/USB |
| MCU→Host | `AA 55` + CMD + len(2 LE) + payload |

常用命令：`0x11` 大文件、`0x45` 小文件、`0x14` 结束、`0x15` 中止、`0x19` 删除、`0x20` 列表、`0x21` 位图、`0xA1` 继续、`0xE0` 错误、`0x0B` Flash 写失败。

### 当前实现要点（以源码为准）

| 点 | 行为 |
|:---|:---|
| 大文件分配 | 首包按 `total_size` 找连续扇区并标 bitmap |
| 写 Flash | `flash_write_and_verify`：**优先 DMA 写**；DMA 启动成功则**本包不回读**，下一块顶部再等 DMA；同步路径才 memcmp |
| 写前擦除 | **下载路径默认不擦**；删除/中止大文件时 `erase_and_free_large_sectors` |
| 小文件 | 线性 `small_next_addr`；中止**不回退** next；删除后空间不足可 `compact_small_files` |
| 下载超时 | 约 5s 无新包 → `abort_download_common` |
| 播放绑定 | `lcd_ui_init` 里 `find_large_file_by_name`；**当前视频名示例为 `fff`，图片 `photo_t`**（改名需与上位机一致） |

### W25Q 驱动约定

- SPI 访问经 `w25q_spi_transmit` / `transmit_receive` / DMA 封装，**统一先 `w25q_flush_rx_fifo()`**，减轻 OVR/脏 RX。  
- 同步读多走 **全双工 TransmitReceive**（命令 + dummy 一并发）。  
- `page_program` 在拉 CS 前轮询 SPI BSY，避免锁存不完整。  
- DMA 进度依赖主循环 **`w25q_dma_task()`**；协议路径里等待 DMA 时应继续 `usb_controller_task`，避免 CDC 堵死。  
- SPI2 分频见 `Core/Src/spi.c`（与信号完整性相关，改速需回归读 ID / 擦写）。  

## 给 AI / 自动化工具的操作建议

1. 先确认这是 **固件 + Electron 上位机** 混合仓库，再动协议或存储。
2. 固件改动优先 `Core/Src`、`Core/Inc`；协议/CRC 必须与 `lcd_host_web` **双侧对齐**。
3. 编译：`make -C Debug -j4`；上位机：`cd lcd_host_web && pnpm build`。
4. **不要**手改 `Debug/` 下 `.o` / 自动生成 makefile 片段，除非明确维护构建系统。  
5. 改存储/USB/LCD：检查  
   - 帧布局与 CRC  
   - `is_downloading` 与 LCD USB 流互斥  
   - DMA busy / ERROR 是否被当成功  
   - 主循环是否被长擦写占死  
6. 播放异常：先查 **文件名绑定**、**FAT size/sector**、**容器 magic（MJPG/BL）**，再查写路径是否真正落盘。  
7. 调试清空：`clear_all_files()` / `clear_all_files_manual()` **会擦文件区**，默认保持注释。  
8. 文档与代码冲突时：**以当前 `.c` 源码为准**，再回写 README。  

## 常见故障速查

| 现象 | 优先检查 |
|:---|:---|
| `W25Q FAIL` | SPI2 接线、供电、JEDEC ID `0xC84018`、分频 |
| `STORAGE FAIL` | AT24C、复位一次完成 FAT 建表 |
| 列表有、屏不播 | `lcd_ui.c` 文件名是否匹配；是否只 init 绑一次未重启 |
| 烧录假成功 / 花屏白闪 | DMA 写无校验、脏扇区未擦、末包 DMA 未完成 |
| 首包卡住 / 超时 | 协议路径长阻塞、未泵 `usb_controller_task`、busy 死等 |
| 上位机无应答 | COM 占用、CRC、`storage_ok`、帧头 BB44 |

## 验证清单（改存储/W25Q/USB 后）

- [ ] `make -C Debug -j4` 通过  
- [ ] 上电：无持续 FAIL（或仅首次 FAT 后复位正常）  
- [ ] 小文件/大文件各烧一条：有 `0xA1`，结束 `0x14` 后 `0x20` 可见  
- [ ] 取消传输 `0x15`：大文件空间可再分配  
- [ ] 播放：文件名与 `lcd_ui` 一致，magic 与转码格式一致  

## 注意事项（摘要）

- 首次 FAT 初始化可能需要**复位一次**。  
- Flash/USB/DMA 时序敏感，改驱动保持「封装统一 + 主循环泵任务」。  
- 大文件删除路径会擦扇区；若改为写前擦除或强制校验，需同步更新 README 中的设计不变量描述。  
- 许可：项目 AGPLv3；第三方见 README。  
