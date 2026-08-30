# AGENT.md

这是 STM IPS 仓库的协作者与自动化工具操作手册。产品介绍见 `README.md` / `README_EN.md`；协议细节以当前固件实现、`lcd_host_web/protocol/contract.json` 和对应测试为准。

## 项目边界

STM32F401RCT6 固件驱动 160x80 IPS 显示屏，并通过 USB CDC 接收媒体文件。仓库同时包含：

| 路径 | 内容 |
|:---|:---|
| `Core/` | LCD、动画、MJPEG/BL 播放、W25Q、AT24C、存储管理、USB 协议 |
| `USB_DEVICE/` | STM32 USB CDC 设备层 |
| `lcd_host_web/` | FastAPI 转码服务、Web Serial 页面、协议测试 |
| `feature_tester/` | 串口和 RGB565 辅助工具 |
| `Drivers/`、`Middlewares/` | STM32 HAL、CMSIS、USB 库、ARM DSP 等第三方依赖 |
| `Debug/` | Makefile 构建目录和生成产物；以构建规则为准，不手改生成文件 |

主要数据流：

```text
Web Serial host
    BB44 frame
        -> storage_manager_task()
        -> W25Q write / AT24C FAT update
        -> AA55 response through usb_controller_task()

lcd_ui_init()
    FAT filename lookup
        -> start sector * 4096
        -> LCD animation / image / video layer
```

## 修改前的定位顺序

先判断改动属于哪一条链路，再只加载相关文件：

1. 主循环和初始化：`Core/Src/main.c`
2. 存储、分配器、Host 帧状态机：`Core/Src/storage_manager.c`、`Core/Inc/storage_manager.h`
3. W25Q 同步/DMA/CRC 块读写：`Core/Src/w25q_controller.c`、`Core/Inc/w25q_controller.h`
4. USB CDC 缓冲、收发队列、AA55 组帧：`Core/Src/usb_controller.c`、`Core/Inc/usb_controller.h`
5. 显示页面和启动资源绑定：`Core/Src/lcd_ui.c`
6. Host 帧和传输流程：`lcd_host_web/static/js/protocol.js`、`transfer.js`、`serial.js`
7. Host 转码和任务 API：`lcd_host_web/server.py`、`job_store.py`、`container_format.py`

协议改动必须同时检查固件、`lcd_host_web/protocol/contract.json`、生成的 `static/js/protocol_contract.js`、JavaScript 测试和 Python golden-vector 测试。

## 固件执行模型

`main.c` 的初始化顺序是 GPIO/DMA/I2C/SPI/USB/TIM，然后初始化 USB 控制器、LCD、W25Q、存储管理器和 UI。

主循环顺序为：

```text
lcd_ui_updater()
    -> w25q_dma_task()
    -> storage_manager_task()
    -> usb_controller_task()
```

注意事项：

- `w25q_dma_task()` 必须持续运行；任何等待 Flash DMA 的协议路径都要继续调用它和 `usb_controller_task()`。
- W25Q 初始化失败时主循环显示 `W25Q FAIL`，文件烧录和播放不可用。
- FAT 无效时 `storage_fat_load()` 会写入默认 FAT 并返回 `false`。由于 `main.c` 把返回值保存到局部 `storage_ok`，首次建表后通常需要复位一次。
- `storage_manager_task()` 在协议路径上可能执行同步擦除和多次读回验证。不要把长操作放进中断，也不要在等待 DMA 时停止 USB 任务。
- 下载期间 `storage_is_downloading()` 为真，LCD USB 流会临时关闭；结束或中止后恢复之前的流状态。

## 构建和测试

### 固件

需要 GNU Arm Embedded Toolchain 和 GNU Make：

```powershell
where arm-none-eabi-gcc
where make
make -C Debug -j4
```

构建规则在 `Debug/makefile`，链接脚本是 `STM32F401RCTX_FLASH.ld`，目标是 STM32F401RCT6 / Cortex-M4。工具链不可用时报告环境问题，不要声称固件构建成功。

### Host

Python 操作统一使用用户指定的 Conda 环境 `py312-classic`，不要向 Conda base 环境安装依赖：

```powershell
conda run -n py312-classic python -m py_compile .\lcd_host_web\server.py
conda run -n py312-classic python -m pytest -q .\lcd_host_web\tests

cd lcd_host_web
npm run typecheck
npm test -- --run
npm run build
```

Python 开发依赖定义在 `lcd_host_web/requirements-dev.txt`；运行服务需要 `requirements.txt` 和 FFmpeg。浏览器端要求 Chrome/Edge Web Serial。缺少 Python 包时只在 `py312-classic` 中补齐，保持 base 环境不变。

启动 Host：

```powershell
cd lcd_host_web
conda run -n py312-classic python server.py
```

### 烧录

```powershell
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program Debug/stm_ips.elf verify reset exit"
```

也可以使用 STM32CubeIDE、STM32CubeProgrammer 或 ST-Link 工具。

## W25Q 分区和 FAT

当前 `Core/Src/storage_manager.c` 的实际分区如下。若 README 或旧注释不同，以此处源码常量为准，并在同一改动中修正文档：

| 区域 | 扇区 | 大小 | 用途 |
|:---|:---:|:---:|:---|
| 保留区 | 0 - 3 | 16 KiB | 小文件压缩暂存 |
| 小文件区 | 4 - 63 | 240 KiB | 线性分配和压缩回收 |
| 大文件区 | 64 - 4031 | 3968 x 4 KiB | 连续扇区 + 位图 |
| 用户区 | 4032 - 4095 | 256 KiB | 当前未由存储管理器使用 |

FAT 保存在 AT24C64 的 `0x0000`，当前魔数为 `0x0D000721`。FAT 主要记录：

- `small_next_addr`、小文件目录和大文件目录；
- 大文件区 496 字节扇区位图；
- 小文件最多 40 个槽位，大文件最多 35 个槽位；
- 文件名数组容量为 16 字节，Host 结束帧实际截断到最多 15 个字节并补 `\\0`。

分配策略：

- 大文件首包根据声明的原始大小预分配连续扇区并立即设置位图。
- 小文件从 `small_next_addr` 线性增长；删除和中止不会回退指针。
- 小文件删除后，剩余空间低于 16 KiB 时尝试 `compact_small_files()`。
- 大文件删除或中止会逐扇区擦除，再清除位图。
- 小文件压缩使用 16 KiB 保留区分批搬运；单批无法放入保留区时压缩失败并返回 `false`。
- `clear_all_files()` 和 `clear_all_files_manual()` 会擦除文件区并重置 FAT，只能在明确需要清空设备时调用。

## Flash 数据格式和写入不变量

媒体原始数据按 1022 字节分块，每块后附 2 字节 CRC-16/USB，物理块固定为 1024 字节；末块用 `0xFF` 补齐后重新计算存储 CRC。当前存储管理器的大、小文件下载都使用这个固定 1024 字节物理块格式。

Host 数据包通常使用 1022 字节数据，以便一包对应一个存储块。首包携带完整原始文件大小，后续数据包的 `total_size` 为 0。

当前写入流程：

1. 首包分配空间，下载路径默认不执行写前擦除；这依赖新分配扇区已被删除/回收路径擦干净。
2. `flash_write_and_verify()` 优先启动 W25Q DMA，等待 DMA 和 Flash 编程完成，再分块回读并 `memcmp` 验证。
3. DMA 启动失败时回退同步写入，随后仍执行回读验证。
4. 任何写入或验证失败都回滚当前下载，并返回错误码 `0x0B`。
5. 结束命令会先等待最后一块 DMA 完成，检查实际大小，再登记 FAT。

高风险场景：脏扇区复用、异常断电、擦除中断、DMA 错误、最后一包尚未固化、压缩过程中断。这些场景需要硬件回归，不能只靠 Host 单元测试覆盖。

## USB Host 协议

### Host -> MCU

```text
BB 44 CMD TOTAL_SIZE[4] PAYLOAD_LEN[2] DATA[N] CRC16[2]
```

`PAYLOAD_LEN` 包含 DATA 和末尾 CRC 两个字节。CRC-16/USB 只覆盖 DATA，不覆盖帧头、命令、文件大小和长度字段；空 DATA 的 CRC 为 `0x0000`。固件接收状态机要求 payload 至少包含 CRC，并对长度做边界检查。

### MCU -> Host

```text
AA 55 CMD PAYLOAD_LEN[2] PAYLOAD[N]
```

当前命令：

| 命令 | 方向 | 作用 |
|:---:|:---|:---|
| `0x11` | Host -> MCU | 大文件数据 |
| `0x45` | Host -> MCU | 小文件数据 |
| `0x14` | Host -> MCU | 结束下载并登记文件名 |
| `0x15` | Host -> MCU | 中止下载 |
| `0x19` | Host -> MCU | 删除文件，payload 为类型和索引 |
| `0x20` | Host -> MCU | 查询文件列表和大文件分配运行 |
| `0x21` | Host -> MCU | 查询大文件区位图 |
| `0x10` | 双向 | LCD 流控制；子命令 `0x00` 停止、`0x01` 开启 |
| `0xA1` | MCU -> Host | 继续/完成确认 |
| `0xE0` | MCU -> Host | 错误，payload 为 1 字节错误码 |
| `0xA0` | MCU -> Host | LCD RGB565 帧 |

固件当前实际发送的错误码：

| 错误码 | 含义 |
|:---:|:---|
| `0x01` | CRC 错误 |
| `0x02` | 删除类型未知 |
| `0x03` | 大文件区没有连续空间 |
| `0x04` | 小文件区空间不足 |
| `0x05` | 下载类型不匹配或非法续传 |
| `0x06` | 大文件目录槽满 |
| `0x07` | 小文件目录槽满 |
| `0x08` | 删除索引无效 |
| `0x09` | 未知命令 |
| `0x0B` | Flash 写入/验证失败或下载超时 |

`contract.json` 还列出 `0x0A` 和 `0x0C`，但当前 `storage_manager.c` 不发送它们；修改错误码前要同时更新两侧契约和测试。

文件列表 `0x20` 的 payload 为：

```text
[entry_count][slot_count][slot records...][file records...]
slot: [record_len=10][0xFF][start_sector u32le][sector_count u32le]
small file: [len][tag][index][name_len][name][address u32le][size u32le][width u16le][height u16le]
large file: [len][tag][index][name_len][name][sector u32le][size u32le][sector_count u32le][width u16le][height u16le]
```

文件记录的 `tag` 最高位表示大/小文件，低 7 位为文件类型。未知尺寸使用 `0xFFFF`。

## UI 和媒体绑定

当前 `Core/Src/lcd_ui.c` 的资源名是：

- `UI_VIDEO_NAME` 为空字符串，默认不会绑定视频；
- `UI_PICTURE_NAME` 为 `photo_t`，没有视频时尝试显示该大文件；
- 视频优先于图片，二者都不存在时显示 `NO MEDIA`。

烧录后成功执行 `0x14` 会调用 `lcd_ui_init()` 重新绑定资源。修改资源名时同步修改 Host 的默认文件名或烧录参数，不要根据旧 README 中的 `fff` / `vp_vid` 假设当前固件会自动播放。

媒体容器当前包括：

- `RAW5`：14 字节头 + RGB565 帧数据；
- `MJPG`：14 字节头 + 每帧长度和 JPEG 数据；
- `BL`：固件中的 4x4 块压缩格式。

## Host 代码结构

- `server.py` 负责上传限制、FFmpeg 探测/转码、RAW5/MJPG 输出和下载临时文件。
- `job_store.py` 提供单进程转换任务、取消、进度和临时文件清理。
- `static/js/` 是当前浏览器运行时；`static/ts/` 是逐步迁移中的类型化接口和测试，不是默认启动路径。
- `protocol/contract.json` 是协议标识和布局的机器可读契约；`static/js/protocol_contract.js` 由它生成或按它同步维护。

## 修改与验证规则

修改存储、W25Q、USB 或协议时，完成条件至少包括：

- 固件构建命令实际通过，或明确记录缺失的工具链；
- Python 语法检查、Python 测试和前端 typecheck/test 的结果已记录；
- 帧布局、CRC 范围、最大长度、错误码和命令语义在固件与 Host 两侧一致；
- 检查 `is_downloading`、LCD 流、DMA busy/error 和 USB 发送队列的交互；
- 检查最后一包、超时、中止、槽满、删除、同名文件和异常断电行为；
- 需要硬件才能验证的项目，明确列出待执行的实机步骤，不以静态检查代替。

推荐实机回归：

1. 上电确认没有持续 `W25Q FAIL` 或 `STORAGE FAIL`；首次 FAT 建表后复位一次。
2. 分别烧录一个小文件和大文件，确认每包 `0xA1`、结束 `0x14`、列表 `0x20` 和播放结果。
3. 传输中发送 `0x15`，确认大文件扇区可再次分配，小文件高水位按设计保留。
4. 删除大文件并观察擦除完成后的 `0xA1`，再查询列表和位图。
5. 在 LCD 流开启时开始烧录，确认流被暂停，完成/中止后恢复。

## 已知文档问题

- `README.md`、`README_EN.md` 中仍有旧分区、旧资源名和旧写入流程描述；当前改动以源码和本文件为准，后续应统一 README。
- README 引用了当前不存在的 `docs/storage_host_mcu_flow.md`，不要把该路径当作可读取的事实来源。
- 根目录 `LICENSE` 是 MIT，但 README 的许可证段落写成 AGPL；许可证判断以 `LICENSE` 为准，发布前必须统一文档。
