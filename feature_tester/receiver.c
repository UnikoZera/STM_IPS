#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define LCD_W 160
#define LCD_H 80
#define FRAME_PIXELS (LCD_W * LCD_H)
#define FRAME_BYTES (FRAME_PIXELS * 2)
#define TAIL_BYTES 4
#define FRAME_TOTAL (FRAME_BYTES + TAIL_BYTES)
#define STREAM_CAPACITY (256 * 1024)
#define RX_CHUNK_SIZE 4096

static const uint8_t kTail[TAIL_BYTES] = { 0x0D, 0x00, 0x07, 0x21 };

static uint8_t g_stream[STREAM_CAPACITY];
static size_t g_stream_len = 0;

static uint32_t g_pixels[FRAME_PIXELS];
static BITMAPINFO g_bmi;
static HWND g_hwnd = NULL;
static HANDLE g_serial = INVALID_HANDLE_VALUE;
static HANDLE g_reader_thread = NULL;
static volatile bool g_running = true;
static volatile bool g_have_frame = false;
static volatile uint32_t g_frame_index = 0;
static CRITICAL_SECTION g_frame_lock;

static uint32_t rgb565_to_rgb32(uint16_t value)
{
    uint32_t r5 = (value >> 11) & 0x1F;
    uint32_t g6 = (value >> 5) & 0x3F;
    uint32_t b5 = value & 0x1F;

    uint32_t r = (r5 * 255U + 15U) / 31U;
    uint32_t g = (g6 * 255U + 31U) / 63U;
    uint32_t b = (b5 * 255U + 15U) / 31U;

    return (r << 16) | (g << 8) | b;
}

static void update_title(void)
{
    if (g_hwnd != NULL)
    {
        char title[128];
        snprintf(title, sizeof(title), "LCD Serial Receiver - frame %u", (unsigned)g_frame_index);
        SetWindowTextA(g_hwnd, title);
    }
}

static void request_redraw(void)
{
    if (g_hwnd != NULL)
    {
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void decode_frame(const uint8_t *frame)
{
    EnterCriticalSection(&g_frame_lock);

    for (size_t i = 0; i < FRAME_PIXELS; ++i)
    {
        uint16_t raw = (uint16_t)((frame[i * 2] << 8) | frame[i * 2 + 1]);
        g_pixels[i] = rgb565_to_rgb32(raw);
    }

    g_have_frame = true;
    g_frame_index++;

    LeaveCriticalSection(&g_frame_lock);

    update_title();
    request_redraw();
}

static void compact_stream(size_t keep_from)
{
    if (keep_from >= g_stream_len)
    {
        g_stream_len = 0;
        return;
    }

    size_t remain = g_stream_len - keep_from;
    memmove(g_stream, g_stream + keep_from, remain);
    g_stream_len = remain;
}

static void try_extract_frames(void)
{
    while (g_stream_len >= FRAME_TOTAL)
    {
        size_t found = (size_t)-1;

        for (size_t i = 0; i + FRAME_TOTAL <= g_stream_len; ++i)
        {
            if (memcmp(g_stream + i + FRAME_BYTES, kTail, TAIL_BYTES) == 0)
            {
                found = i;
                break;
            }
        }

        if (found == (size_t)-1)
        {
            if (g_stream_len > FRAME_TOTAL - 1)
            {
                compact_stream(g_stream_len - (FRAME_TOTAL - 1));
            }
            break;
        }

        decode_frame(g_stream + found);
        compact_stream(found + FRAME_TOTAL);
    }
}

static void feed_bytes(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    if (len > STREAM_CAPACITY)
    {
        data += (len - STREAM_CAPACITY);
        len = STREAM_CAPACITY;
    }

    if (g_stream_len + len > STREAM_CAPACITY)
    {
        size_t keep = FRAME_TOTAL - 1;
        if (keep > g_stream_len)
        {
            keep = g_stream_len;
        }

        compact_stream(g_stream_len - keep);
    }

    memcpy(g_stream + g_stream_len, data, len);
    g_stream_len += len;
    try_extract_frames();
}

static void paint_frame(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    int client_w = rc.right - rc.left;
    int client_h = rc.bottom - rc.top;

    HBRUSH bg = CreateSolidBrush(RGB(10, 10, 10));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    EnterCriticalSection(&g_frame_lock);

    if (!g_have_frame)
    {
        LeaveCriticalSection(&g_frame_lock);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 220, 220));
        const char *msg = "Waiting for LCD frames on serial port...";
        TextOutA(hdc, 20, 20, msg, (int)strlen(msg));
        return;
    }

    int scale_x = client_w / LCD_W;
    int scale_y = client_h / LCD_H;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1)
    {
        scale = 1;
    }

    int draw_w = LCD_W * scale;
    int draw_h = LCD_H * scale;
    int left = (client_w - draw_w) / 2;
    int top = (client_h - draw_h) / 2;

    StretchDIBits(
        hdc,
        left, top, draw_w, draw_h,
        0, 0, LCD_W, LCD_H,
        g_pixels,
        &g_bmi,
        DIB_RGB_COLORS,
        SRCCOPY);

    LeaveCriticalSection(&g_frame_lock);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint_frame(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static void init_bitmap_info(void)
{
    ZeroMemory(&g_bmi, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = LCD_W;
    g_bmi.bmiHeader.biHeight = -LCD_H;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
}

static HWND create_view_window(void)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LCDReceiverWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return NULL;
    }

    return CreateWindowExA(
        0,
        "LCDReceiverWindow",
        "LCD Serial Receiver",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        500,
        NULL,
        NULL,
        GetModuleHandleA(NULL),
        NULL);
}

static bool setup_serial_port(const char *port_name, DWORD baud_rate)
{
    char full_name[64];
    DCB dcb;
    COMMTIMEOUTS timeouts;

    snprintf(full_name, sizeof(full_name), "\\\\.\\%s", port_name);

    g_serial = CreateFileA(
        full_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (g_serial == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    SetupComm(g_serial, 1 << 16, 1 << 16);
    PurgeComm(g_serial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(g_serial, &dcb))
    {
        return false;
    }

    dcb.BaudRate = baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(g_serial, &dcb))
    {
        return false;
    }

    ZeroMemory(&timeouts, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutConstant = 20;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if (!SetCommTimeouts(g_serial, &timeouts))
    {
        return false;
    }

    return true;
}

static DWORD WINAPI reader_thread_proc(LPVOID param)
{
    (void)param;

    uint8_t buffer[RX_CHUNK_SIZE];

    while (g_running)
    {
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(g_serial, buffer, sizeof(buffer), &bytes_read, NULL);

        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED)
            {
                break;
            }
            Sleep(10);
            continue;
        }

        if (bytes_read > 0U)
        {
            feed_bytes(buffer, bytes_read);
        }
    }

    return 0;
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            g_running = false;
            return;
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

int main(int argc, char **argv)
{
    const char *port_name = "COM5";
    DWORD baud_rate = 921600;

    if (argc >= 2)
    {
        port_name = argv[1];
    }
    if (argc >= 3)
    {
        baud_rate = (DWORD)strtoul(argv[2], NULL, 10);
        if (baud_rate == 0U)
        {
            baud_rate = 921600;
        }
    }

    InitializeCriticalSection(&g_frame_lock);
    init_bitmap_info();

    g_hwnd = create_view_window();
    if (g_hwnd == NULL)
    {
        printf("failed to create window\n");
        DeleteCriticalSection(&g_frame_lock);
        return 1;
    }

    if (!setup_serial_port(port_name, baud_rate))
    {
        printf("failed to open serial port %s\n", port_name);
        DestroyWindow(g_hwnd);
        DeleteCriticalSection(&g_frame_lock);
        return 1;
    }

    g_reader_thread = CreateThread(NULL, 0, reader_thread_proc, NULL, 0, NULL);
    if (g_reader_thread == NULL)
    {
        printf("failed to create reader thread\n");
        CloseHandle(g_serial);
        DestroyWindow(g_hwnd);
        DeleteCriticalSection(&g_frame_lock);
        return 1;
    }

    while (g_running)
    {
        pump_messages();
        Sleep(10);
    }

    if (g_serial != INVALID_HANDLE_VALUE)
    {
        CancelIo(g_serial);
    }

    if (g_reader_thread != NULL)
    {
        WaitForSingleObject(g_reader_thread, 1000);
        CloseHandle(g_reader_thread);
    }

    if (g_serial != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_serial);
    }

    DeleteCriticalSection(&g_frame_lock);
    return 0;
}