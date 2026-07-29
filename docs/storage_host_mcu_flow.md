# Host ↔ MCU 存储协议与流程

与 `README.md` / `AGENT.md` 配套。实现以 `Core/Src/storage_manager.c`、`w25q_controller.c`、`usb_controller.c`、`lcd_host_web/lcd_host_web.html` 为准。

## 1. 帧格式

### Host → Device

```
[0xBB][0x44][CMD][total_size LE 4B][payload_len LE 2B][payload...][CRC16/USB LE 2B]
```

- `total_size`：数据**首包**为完整文件字节数；后续数据包多为 0。  
- `payload_len`：本包 data 长度 + 2（含 CRC）。  
- CRC 覆盖从 `BB` 到 data 末（不含 CRC 本身）。

### Device → Host

```
[0xAA][0x55][CMD][len LE 2B][payload...]
```

常用：`0xA1` 继续、`0xE0` + 1B 错误码、`0x20` 列表、`0x21` 位图、`0xA0` LCD 帧。

## 2. 命令一览

| CMD | 含义 |
|:---:|:---|
| `0x11` | 大文件数据 |
| `0x45` | 小文件数据 |
| `0x14` | 结束下载（文件名） |
| `0x15` | 中止下载 |
| `0x19` | 删除 |
| `0x20` | 查询列表 TLV |
| `0x21` | 大文件 bitmap |
| `0x10` | LCD 流控 |
| `0xA1` | 继续 / 中止确认 |
| `0xE0` | 错误 |
| `0x0B` | Flash 写失败（经 `0xE0` 载荷） |

## 3. 大文件烧录（`0x11`）

```
Host                         MCU
 |-- 0x11 首包 + total_size -->| is_downloading=1
 |                             |  allocate_large_sectors
 |                             |  flash_write_and_verify → 0xA1
 |-- 0x11 后续包 ------------->|  write → 0xA1
 |-- 0x14 filename ----------->|  写 FAT (recycle/append) → 0xA1
 |-- (取消) 0x15 ------------->|  erase+free bitmap → 0xA1
```

**FAT 变更**：`0x14` 成功时写 `large_files[]` 与 `large_file_count`（或复用 `is_valid=0` 槽）。  
**回滚**：`0x15` / 超时 / 写失败 `0x0B` / 槽满 `0x06` → `erase_and_free_large_sectors`。

## 4. 小文件烧录（`0x45`）

```
首包: allocate_small_space(total) 推进 small_next_addr
写数据同 flash_write_and_verify
0x14: 登记 small_files[]
中止: 不回退 next（死区靠 compact）
```

删除后若 `END - small_next_addr < threshold` 可触发 `compact_small_files`。

## 5. 写路径（当前代码）

```
flash_write_and_verify(addr, data, size)
  wait DMA busy (泵 w25q_dma_task + usb_controller_task)
  memcpy → dma_write_buf
  if w25q_write_data_dma(...) ok:
       return true          // 不等待、不回读
  else:
       w25q_write_data 同步
       fast_read + memcmp → 成功/重试
```

含义：

- **吞吐优先**：DMA 与 USB 可重叠。  
- **风险**：DMA 路径坏数据仍可能 ACK；脏扇区未擦时 NOR 只能 1→0，易花屏。  
- 删除大文件会擦扇区，有利于「干净分配」假设。

## 6. 读 / 播放

1. `0x20` 或 `find_large_file_by_name` 得 `start_sector`、`size`。  
2. 地址 = `start_sector * 4096`，`end = start + size`。  
3. 读 magic：`MJPG` → MJPEG；`BL` → BL；否则 raw RGB565。  
4. `lcd_ui_init` **仅启动绑一次**；烧完需复位或改代码重绑。

## 7. 分区回顾

| 区 | 扇区 | 用途 |
|:---|:---:|:---|
| 保留 | 0–1 | compact 中转 8KB |
| 小文件 | 2–63 | 线性 next |
| 大文件 | 64–4031 | bitmap |
| 用户 | 4032–4095 | 预留 |

FAT：`AT24C @0x0000`，magic `0x0D000722`。

## 8. 故障对照

| 现象 | 方向 |
|:---|:---|
| 无 `0xA1` | CRC、storage_ok、COM、MCU 阻塞 |
| `0x0B` | 同步写校验失败 / DMA 等超时 |
| 列表有不播 | 文件名、`lcd_ui` 绑定、未复位 |
| 花屏/白闪 | 脏扇区、DMA 无校验、格式不匹配 |
| 首次 STORAGE FAIL | 复位一次完成 FAT 默认表 |

## 9. 变更记录约定

修改下列行为时请同步本文件与 README「设计不变量」：

- 写前擦除策略  
- DMA 是否强制校验  
- 超时 / 错误码  
- 默认播放文件名  
