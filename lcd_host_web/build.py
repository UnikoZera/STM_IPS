import subprocess
import sys
import shutil
import os
from pathlib import Path

DIR = Path(__file__).parent.resolve()
DIST = DIR / 'dist'
BUILD = DIR / 'build'
SPEC = DIR / 'STM_IPS_Host.spec'


def _imageio_ffmpeg_bin():
    try:
        import imageio_ffmpeg
        p = Path(imageio_ffmpeg.get_ffmpeg_exe())
        return p if p.is_file() else None
    except Exception:
        return None


def main():
    print('=' * 50)
    print('  STM IPS Host - 一键打包')
    print('=' * 50)

    # 1. deps for runtime + packer
    for pkg, import_name in (
        ('fastapi', 'fastapi'),
        ('uvicorn', 'uvicorn'),
        ('python-multipart', 'multipart'),
        ('imageio-ffmpeg', 'imageio_ffmpeg'),
        ('pyinstaller', 'PyInstaller'),
    ):
        try:
            mod = __import__(import_name)
            ver = getattr(mod, '__version__', '')
            print(f'[OK] {import_name} {ver}'.rstrip())
        except ImportError:
            print(f'[..] 正在安装 {pkg} ...')
            subprocess.check_call([sys.executable, '-m', 'pip', 'install', pkg, '-q'])
            print(f'[OK] {pkg} 安装完成')

    ff = _imageio_ffmpeg_bin()
    if ff:
        print(f'[OK] imageio-ffmpeg 二进制: {ff.name} ({ff.stat().st_size / 1024 / 1024:.1f} MB)')
    else:
        print('[!!] 未找到 imageio-ffmpeg 的 ffmpeg 可执行文件')
        print('     打包后的主机在无系统 PATH ffmpeg 时将无法转码')
        print('     请执行: pip install imageio-ffmpeg')

    # 2. clean old outputs (keep hand-maintained requirements etc.)
    for p in (DIST, BUILD):
        if p.is_dir():
            shutil.rmtree(p)
    if SPEC.is_file():
        # 由 PyInstaller 重新生成；避免陈旧 datas/hiddenimports
        SPEC.unlink()
    print('[OK] 已清理旧构建文件')

    # 3. package
    # Windows --add-data 使用 ;
    sep = os.pathsep
    cmd = [
        sys.executable, '-m', 'PyInstaller',
        '--onefile',
        '--name', 'STM_IPS_Host',
        '--console',
        f'--add-data=index.html{sep}.',
        f'--add-data=static{sep}static',
        f'--add-data=server.py{sep}.',
        '--collect-submodules=uvicorn',
        '--hidden-import=fastapi',
        '--hidden-import=uvicorn',
        '--hidden-import=multipart',
        '--hidden-import=imageio_ffmpeg',
        '--hidden-import=imageio_ffmpeg.binaries',
        '--collect-binaries=imageio_ffmpeg',
        '--collect-data=imageio_ffmpeg',
        '--noconfirm',
        '--clean',
        str(DIR / 'launcher.py'),
    ]

    # 双保险：把 ffmpeg 可执行文件直接塞进包根
    if ff is not None:
        cmd.insert(-1, f'--add-binary={ff}{sep}imageio_ffmpeg/binaries')

    print('[..] 正在打包 ...')
    print('     ' + ' '.join(cmd[2:]))
    result = subprocess.run(cmd, cwd=str(DIR))

    if result.returncode != 0:
        print(f'\n[!!] 打包失败 (exit code {result.returncode})')
        sys.exit(1)

    exe = DIST / 'STM_IPS_Host.exe'
    if exe.exists():
        size_mb = exe.stat().st_size / (1024 * 1024)
        print(f"\n{'=' * 50}")
        print('  打包成功!')
        print(f'  输出: {exe}')
        print(f'  大小: {size_mb:.1f} MB')
        print(f"{'=' * 50}")
        print('  使用: 双击 dist\\STM_IPS_Host.exe')
        print('  依赖: 无需本机 Python；浏览器建议 Chrome/Edge')
        print('  可选: 系统 PATH 中的 ffmpeg/ffprobe（否则用内置 ffmpeg）')
    else:
        print('[!!] 打包完成但找不到输出文件')
        sys.exit(1)


if __name__ == '__main__':
    main()
