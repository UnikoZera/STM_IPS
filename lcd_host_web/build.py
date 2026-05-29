import subprocess, sys, shutil, os
from pathlib import Path

DIR = Path(__file__).parent
DIST = DIR / 'dist'
BUILD = DIR / 'build'
SPEC = DIR / 'STM_IPS_Host.spec'

def main():
    print("=" * 50)
    print("  STM IPS Host - 一键打包")
    print("=" * 50)

    # 1. 确保 pyinstaller 已安装
    try:
        import PyInstaller
        print(f"[OK] PyInstaller {PyInstaller.__version__} 已安装")
    except ImportError:
        print("[..] 正在安装 PyInstaller ...")
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'pyinstaller', '-q'])
        print("[OK] PyInstaller 安装完成")

    # 2. 清理旧产物
    for p in [DIST, BUILD, SPEC]:
        if p.is_dir():
            shutil.rmtree(p)
        elif p.is_file():
            p.unlink()
    print("[OK] 已清理旧构建文件")

    # 3. 执行打包
    cmd = [
        sys.executable, '-m', 'PyInstaller',
        '--onefile',
        '--name', 'STM_IPS_Host',
        '--add-data', f'lcd_host_web.html{os.pathsep}.',
        '--add-data', f'server.py{os.pathsep}.',
        '--hidden-import', 'flask',
        '--noconfirm',
        '--clean',
        str(DIR / 'launcher.py'),
    ]

    print(f"[..] 正在打包 ...")
    print(f"     命令: {' '.join(cmd[2:])}")
    result = subprocess.run(cmd, cwd=str(DIR))

    if result.returncode != 0:
        print(f"\n[!!] 打包失败 (exit code {result.returncode})")
        sys.exit(1)

    exe = DIST / 'STM_IPS_Host.exe'
    if exe.exists():
        size_mb = exe.stat().st_size / (1024 * 1024)
        print(f"\n{'=' * 50}")
        print(f"  打包成功!")
        print(f"  输出: {exe}")
        print(f"  大小: {size_mb:.1f} MB")
        print(f"{'=' * 50}")
    else:
        print("[!!] 打包完成但找不到输出文件")
        sys.exit(1)

if __name__ == '__main__':
    main()
