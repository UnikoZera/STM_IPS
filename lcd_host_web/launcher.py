import sys
import os
import time
import webbrowser
import socket
import threading

def _resource_path(rel):
    """Resolve path for PyInstaller frozen or normal dev."""
    if getattr(sys, 'frozen', False):
        base = os.path.dirname(sys.executable)
        bundle = sys._MEIPASS
    else:
        base = os.path.dirname(os.path.abspath(__file__))
        bundle = base
    # Data files go to _MEIPASS; user-dir files stay next to .exe
    return os.path.join(bundle, rel)

def _exe_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
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
    server.app.static_folder = bundle_dir
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
    server.app.run(host='127.0.0.1', port=port, debug=False, use_reloader=False)

if __name__ == '__main__':
    main()
