import sys
import os
import time
import socket
import threading
import webbrowser


def _resource_path(rel):
    """Resolve path for PyInstaller frozen or normal dev.

    Data files (index.html / static/) go to _MEIPASS; user-dir files stay next
    to the .exe. `server.py` resolves its own assets via ``Path(__file__).parent``,
    so this is only used to make the ``server`` module importable when frozen.
    """
    if getattr(sys, 'frozen', False):
        return sys._MEIPASS
    return os.path.dirname(os.path.abspath(__file__))


def _find_free_port(preferred=5000):
    """Use a fixed port so localStorage persists across sessions."""
    for p in range(preferred, preferred + 100):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(('127.0.0.1', p))
                return p
            except OSError:
                continue
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def _wait_for_server(url, timeout=15):
    import urllib.request
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(url, timeout=1)
            return True
        except Exception:
            time.sleep(0.15)
    return False


def main():
    port = _find_free_port()
    url = f'http://127.0.0.1:{port}'

    bundle_dir = _resource_path('.')
    sys.path.insert(0, bundle_dir)

    import server
    server.TMP.mkdir(parents=True, exist_ok=True)

    def _open_browser():
        if _wait_for_server(url, timeout=15):
            webbrowser.open(url)

    threading.Thread(target=_open_browser, daemon=True).start()

    print(f'Starting STM IPS Host at {url}')
    try:
        accel = server._detect_hwaccel()
        print(f'Video HW accel: {accel or "cpu"} (override: env STM_IPS_HWACCEL=cuda|d3d11va|off)')
    except Exception as e:
        print(f'Video HW accel probe failed: {e}')
    print('Press Ctrl+C to stop.')

    import uvicorn
    uvicorn.run(server.app, host='127.0.0.1', port=port, log_level='info')


if __name__ == '__main__':
    main()
