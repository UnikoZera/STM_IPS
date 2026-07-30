# STM IPS Host Desktop

Electron + Svelte 桌面上位机。媒体转码、USB CDC Web Serial、文件烧录和 LCD 预览均在应用内完成；运行时不依赖 Python、Flask 或本地 HTTP 服务。

## 依赖

- Node.js 20+ 与 pnpm 10
- FFmpeg：开发机安装在 `PATH`，或通过 `STM_IPS_FFMPEG` 指定 `ffmpeg` / `ffmpeg.exe` 的绝对路径。

Electron 运行时由 `pnpm install` 下载。Linux 上本地 Electron 二进制不可用时，启动器才回退到系统 Electron；Windows 不使用 Linux 路径。

## 开发

Linux、Windows 使用相同命令：

```bash
cd lcd_host_web/desktop
pnpm install
pnpm dev
```

构建前端并启动桌面应用：

```bash
pnpm build
pnpm start
```

在 Windows PowerShell 中指定 FFmpeg：

```powershell
$env:STM_IPS_FFMPEG = 'C:\\ffmpeg\\bin\\ffmpeg.exe'
pnpm dev
```

Windows 安装包在 Windows 环境中生成：

```powershell
pnpm dist
```

产物为 NSIS 安装包；未签名版本可能触发 SmartScreen 警告。

## 环境变量

- `STM_IPS_FFMPEG`：FFmpeg 可执行文件的绝对路径；优先级高于发布包内置二进制与系统 `PATH`。

## 媒体输出格式

- `mjpeg`：14 字节 `MJPG` 头，随后为每帧的 4 字节小端长度和 JPEG 数据。
- `raw`：14 字节 `RAW5` 头，随后为大端 RGB565 像素帧。

两种格式都使用 16 位小端 `frame_count`、宽度与高度字段；单次转换最多 65535 帧。
