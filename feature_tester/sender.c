#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <string.h>

HANDLE hComm;

#define USB_PROTO_HEAD0 0xAAU
#define USB_PROTO_HEAD1 0x55U
#define USB_PROTO_HEADER_SIZE 5U
#define USB_ECHO_CMD 0x02U

#define READ_CHUNK_SIZE 65536U
#define SEND_CHUNK_SIZE 65536U
#define PARSER_BUFFER_SIZE 524288U
#define MAX_EXPECTED_PAYLOAD 4096U
#define TX_TIMEOUT_RETRY_MAX 200U
#define TX_TIMEOUT_RETRY_DELAY_MS 10U

typedef struct {
    uint8_t *tx_data;
    size_t tx_len;

    size_t echoed_payload_len;
    size_t matched_bytes;
    size_t mismatch_bytes;
    size_t extra_bytes;

    size_t echo_frame_count;
    size_t other_cmd_frame_count;
    size_t invalid_frame_count;
    size_t dropped_stream_bytes;

    long first_mismatch_index;
    uint8_t first_expected;
    uint8_t first_actual;
} compare_stats_t;

static CRITICAL_SECTION g_stats_lock;
static compare_stats_t g_stats;
static volatile LONG g_reader_running = 1;
static volatile LONG g_parser_reset_requested = 0;

static void reset_stats(uint8_t *tx_data, size_t tx_len)
{
    EnterCriticalSection(&g_stats_lock);
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.tx_data = tx_data;
    g_stats.tx_len = tx_len;
    g_stats.first_mismatch_index = -1;
    LeaveCriticalSection(&g_stats_lock);
}

static void clear_stats_tx_reference(void)
{
    EnterCriticalSection(&g_stats_lock);
    g_stats.tx_data = NULL;
    g_stats.tx_len = 0U;
    LeaveCriticalSection(&g_stats_lock);
}

static void snapshot_stats(compare_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }

    EnterCriticalSection(&g_stats_lock);
    *out = g_stats;
    LeaveCriticalSection(&g_stats_lock);
}

static void on_drop_stream_bytes(size_t dropped)
{
    EnterCriticalSection(&g_stats_lock);
    g_stats.dropped_stream_bytes += dropped;
    LeaveCriticalSection(&g_stats_lock);
}

static void on_invalid_frame(void)
{
    EnterCriticalSection(&g_stats_lock);
    g_stats.invalid_frame_count++;
    LeaveCriticalSection(&g_stats_lock);
}

static void on_other_cmd_frame(void)
{
    EnterCriticalSection(&g_stats_lock);
    g_stats.other_cmd_frame_count++;
    LeaveCriticalSection(&g_stats_lock);
}

static void on_echo_payload(const uint8_t *payload, size_t payload_len)
{
    size_t i;
    size_t compare_len;
    size_t tx_offset;
    size_t remain;

    if ((payload == NULL) || (payload_len == 0U))
    {
        return;
    }

    EnterCriticalSection(&g_stats_lock);

    g_stats.echo_frame_count++;
    tx_offset = g_stats.echoed_payload_len;
    remain = (tx_offset < g_stats.tx_len) ? (g_stats.tx_len - tx_offset) : 0U;
    compare_len = (payload_len < remain) ? payload_len : remain;

    if ((g_stats.tx_data == NULL) || (g_stats.tx_len == 0U))
    {
        compare_len = 0U;
    }

    for (i = 0U; i < compare_len; i++)
    {
        uint8_t expected = g_stats.tx_data[tx_offset + i];
        uint8_t actual = payload[i];

        if (expected == actual)
        {
            g_stats.matched_bytes++;
        }
        else
        {
            g_stats.mismatch_bytes++;

            if (g_stats.first_mismatch_index < 0)
            {
                g_stats.first_mismatch_index = (long)(tx_offset + i);
                g_stats.first_expected = expected;
                g_stats.first_actual = actual;
            }
        }
    }

    if (payload_len > compare_len)
    {
        g_stats.extra_bytes += (payload_len - compare_len);
    }

    g_stats.echoed_payload_len += payload_len;

    LeaveCriticalSection(&g_stats_lock);
}

static void parse_usb_frames(uint8_t *stream_buf, size_t *stream_len)
{
    size_t search_idx;

    if ((stream_buf == NULL) || (stream_len == NULL))
    {
        return;
    }

    while (*stream_len >= 2U)
    {
        search_idx = 0U;
        while ((search_idx + 1U) < *stream_len)
        {
            if ((stream_buf[search_idx] == USB_PROTO_HEAD0) &&
                (stream_buf[search_idx + 1U] == USB_PROTO_HEAD1))
            {
                break;
            }
            search_idx++;
        }

        if ((search_idx + 1U) >= *stream_len)
        {
            if (*stream_len > 1U)
            {
                on_drop_stream_bytes(*stream_len - 1U);
                stream_buf[0] = stream_buf[*stream_len - 1U];
                *stream_len = 1U;
            }
            return;
        }

        if (search_idx > 0U)
        {
            on_drop_stream_bytes(search_idx);
            memmove(stream_buf, stream_buf + search_idx, *stream_len - search_idx);
            *stream_len -= search_idx;
        }

        if (*stream_len < USB_PROTO_HEADER_SIZE)
        {
            return;
        }

        uint8_t cmd = stream_buf[2];
        uint16_t payload_len = (uint16_t)stream_buf[3] | ((uint16_t)stream_buf[4] << 8);
        size_t frame_len = (size_t)USB_PROTO_HEADER_SIZE + (size_t)payload_len;

        if (payload_len > MAX_EXPECTED_PAYLOAD)
        {
            on_invalid_frame();
            on_drop_stream_bytes(1U);
            memmove(stream_buf, stream_buf + 1U, *stream_len - 1U);
            *stream_len -= 1U;
            continue;
        }

        if (*stream_len < frame_len)
        {
            return;
        }

        if (cmd == USB_ECHO_CMD)
        {
            on_echo_payload(stream_buf + USB_PROTO_HEADER_SIZE, payload_len);
        }
        else
        {
            on_other_cmd_frame();
        }

        if (*stream_len > frame_len)
        {
            memmove(stream_buf, stream_buf + frame_len, *stream_len - frame_len);
        }
        *stream_len -= frame_len;
    }
}

DWORD WINAPI ReceiveThread(LPVOID lpParam)
{
    uint8_t read_buffer[READ_CHUNK_SIZE];
    uint8_t *parser_buffer = NULL;
    size_t parser_len = 0U;
    OVERLAPPED osReader = {0};
    (void)lpParam;

    osReader.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (osReader.hEvent == NULL) return 1;

    parser_buffer = (uint8_t *)malloc(PARSER_BUFFER_SIZE);
    if (parser_buffer == NULL)
    {
        CloseHandle(osReader.hEvent);
        printf("[RX] parser buffer allocation failed.\n");
        return 1;
    }

    while (InterlockedCompareExchange(&g_reader_running, 1, 1) == 1)
    {
        DWORD bytes_read = 0U;
        BOOL ok = ReadFile(hComm, read_buffer, (DWORD)sizeof(read_buffer), &bytes_read, &osReader);

        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                while (InterlockedCompareExchange(&g_reader_running, 1, 1) == 1)
                {
                    DWORD wait_res = WaitForSingleObject(osReader.hEvent, 50);
                    if (wait_res == WAIT_OBJECT_0)
                    {
                        ok = GetOverlappedResult(hComm, &osReader, &bytes_read, FALSE);
                        break;
                    }
                }
                if (InterlockedCompareExchange(&g_reader_running, 1, 1) == 0)
                {
                    CancelIo(hComm);
                    break;
                }
            }
            else
            {
                break;
            }
        }

        if (InterlockedExchange(&g_parser_reset_requested, 0) == 1)
        {
            parser_len = 0U;
        }

        if (!ok || bytes_read == 0U)
        {
            continue;
        }

        if ((parser_len + (size_t)bytes_read) > PARSER_BUFFER_SIZE)
        {
            size_t keep = (USB_PROTO_HEADER_SIZE > 1U) ? (USB_PROTO_HEADER_SIZE - 1U) : 1U;

            if (parser_len > keep)
            {
                on_drop_stream_bytes(parser_len - keep);
                memmove(parser_buffer, parser_buffer + (parser_len - keep), keep);
                parser_len = keep;
            }
            else
            {
                parser_len = 0U;
            }
        }

        memcpy(parser_buffer + parser_len, read_buffer, (size_t)bytes_read);
        parser_len += (size_t)bytes_read;
        parse_usb_frames(parser_buffer, &parser_len);
    }

    free(parser_buffer);
    CloseHandle(osReader.hEvent);
    return 0;
}

static bool read_int_prompt(const char *prompt, int *value)
{
    int scanned;

    if ((prompt == NULL) || (value == NULL))
    {
        return false;
    }

    printf("%s", prompt);
    fflush(NULL);

    scanned = scanf("%d", value);
    if (scanned != 1)
    {
        int ch;
        while (((ch = getchar()) != '\n') && (ch != EOF))
        {
            // discard invalid input line.
        }
        return false;
    }

    return true;
}

static void print_compare_report(int round_index, int total_rounds, size_t tx_offset, const compare_stats_t *final_snap)
{
    size_t compared_bytes;
    size_t missing_bytes;

    if (final_snap == NULL)
    {
        return;
    }

    compared_bytes = (final_snap->tx_len < final_snap->echoed_payload_len)
                   ? final_snap->tx_len
                   : final_snap->echoed_payload_len;
    missing_bytes = (tx_offset > final_snap->echoed_payload_len)
                  ? (tx_offset - final_snap->echoed_payload_len)
                  : 0U;

    printf("\n=== Compare Report (round %d/%d) ===\n", round_index, total_rounds);
    printf("Sent bytes                 : %llu\n", (unsigned long long)tx_offset);
    printf("Echo payload bytes(cmd=0x02): %llu\n", (unsigned long long)final_snap->echoed_payload_len);
    printf("Compared bytes             : %llu\n", (unsigned long long)compared_bytes);
    printf("Matched bytes              : %llu\n", (unsigned long long)final_snap->matched_bytes);
    printf("Mismatch bytes             : %llu\n", (unsigned long long)final_snap->mismatch_bytes);
    printf("Missing bytes              : %llu\n", (unsigned long long)missing_bytes);
    printf("Extra echoed bytes         : %llu\n", (unsigned long long)final_snap->extra_bytes);
    printf("Echo frames(cmd=0x02)      : %llu\n", (unsigned long long)final_snap->echo_frame_count);
    printf("Other cmd frames           : %llu\n", (unsigned long long)final_snap->other_cmd_frame_count);
    printf("Invalid frames             : %llu\n", (unsigned long long)final_snap->invalid_frame_count);
    printf("Dropped stream bytes       : %llu\n", (unsigned long long)final_snap->dropped_stream_bytes);

    if (final_snap->first_mismatch_index >= 0)
    {
        printf("First mismatch index       : %ld (exp=%02X, got=%02X)\n",
               final_snap->first_mismatch_index,
               final_snap->first_expected,
               final_snap->first_actual);
    }

    if ((missing_bytes == 0U) && (final_snap->mismatch_bytes == 0U) && (final_snap->extra_bytes == 0U))
    {
        printf("Result                     : PASS (no missing/no mismatch)\n");
    }
    else
    {
        printf("Result                     : FAIL (missing or mismatch detected)\n");
    }
}

static bool run_single_round(size_t tx_len, int round_index, int total_rounds)
{
    uint8_t *tx_data = NULL;
    size_t tx_offset = 0U;
    DWORD test_start_tick;
    DWORD last_progress_tick;
    DWORD max_wait_ms;
    size_t last_echoed = 0U;
    const DWORD idle_timeout_ms = 20000U;
    compare_stats_t final_snap;

    tx_data = (uint8_t *)malloc(tx_len);
    if (tx_data == NULL)
    {
        printf("Memory allocation failed for tx buffer.\n");
        return false;
    }

    for (size_t i = 0U; i < tx_len; i++)
    {
        tx_data[i] = (uint8_t)(rand() & 0xFF);
    }

    // Flush residual bytes and reset frame parser state to isolate each round.
    PurgeComm(hComm, PURGE_RXABORT | PURGE_RXCLEAR);
    InterlockedExchange(&g_parser_reset_requested, 1);

    reset_stats(tx_data, tx_len);

    printf("\n[Round %d/%d] Start sending %llu bytes random payload...\n",
           round_index,
           total_rounds,
           (unsigned long long)tx_len);

    OVERLAPPED osWrite = {0};
    osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (osWrite.hEvent == NULL) return false;

    size_t next_progress = 256U * 1024U;

    while (tx_offset < tx_len)
    {
        DWORD bytes_written = 0U;
        size_t remain = tx_len - tx_offset;
        size_t chunk = (remain > SEND_CHUNK_SIZE) ? SEND_CHUNK_SIZE : remain;

        BOOL ok = WriteFile(hComm, tx_data + tx_offset, (DWORD)chunk, &bytes_written, &osWrite);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                ok = GetOverlappedResult(hComm, &osWrite, &bytes_written, TRUE);
                if (!ok) err = GetLastError();
            }

            if (!ok)
            {
                printf("[TX] WriteFile error %lu at offset %llu.\n", err, (unsigned long long)tx_offset);
                break;
            }
        }

        if (bytes_written == 0U)
        {
            continue;
        }

        tx_offset += (size_t)bytes_written;

        if (tx_offset >= next_progress || tx_offset == tx_len)
        {
            printf("[TX] Progress: %llu / %llu\n",
                   (unsigned long long)tx_offset,
                   (unsigned long long)tx_len);
            next_progress = tx_offset + (256U * 1024U);
        }
    }

    CloseHandle(osWrite.hEvent);

    printf("[TX] Done sending: %llu bytes.\n", (unsigned long long)tx_offset);

    test_start_tick = GetTickCount();
    last_progress_tick = test_start_tick;
    max_wait_ms = 60000U;
    if (tx_len > 0U)
    {
        DWORD size_based_wait = (DWORD)(tx_len / 50U);
        if (size_based_wait > max_wait_ms)
        {
            max_wait_ms = size_based_wait;
        }
    }

    while (1)
    {
        compare_stats_t snap;

        Sleep(50);
        snapshot_stats(&snap);

        if (snap.echoed_payload_len >= tx_offset)
        {
            break;
        }

        if (snap.echoed_payload_len != last_echoed)
        {
            last_echoed = snap.echoed_payload_len;
            last_progress_tick = GetTickCount();
        }

        if ((GetTickCount() - last_progress_tick) > idle_timeout_ms)
        {
            break;
        }

        if ((GetTickCount() - test_start_tick) > max_wait_ms)
        {
            break;
        }
    }

    snapshot_stats(&final_snap);
    print_compare_report(round_index, total_rounds, tx_offset, &final_snap);

    clear_stats_tx_reference();
    free(tx_data);
    return true;
}

int main(int argc, char *argv[])
{
    int exit_code = 0;
    int length_input = 0;
    int rounds_input = 0;
    int round_idx;
    HANDLE hThread;

    if (argc < 3)
    {
        printf("Usage: %s <COM_PORT> <BAUD_RATE>\n", argv[0]);
        printf("Example: %s COM24 921600\n", argv[0]);
        return 1;
    }

    srand((unsigned int)time(NULL));

    char port_name[32];
    snprintf(port_name, sizeof(port_name), "\\\\.\\%s", argv[1]);
    int baud_rate = atoi(argv[2]);

    hComm = CreateFileA(port_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hComm == INVALID_HANDLE_VALUE)
    {
        printf("Error opening %s\n", port_name);
        return 1;
    }

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hComm, &dcbSerialParams))
    {
        printf("Error getting serial state\n");
        CloseHandle(hComm);
        return 1;
    }

    dcbSerialParams.BaudRate = baud_rate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if (!SetCommState(hComm, &dcbSerialParams))
    {
        printf("Error setting serial state\n");
        CloseHandle(hComm);
        return 1;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 20U;
    timeouts.ReadTotalTimeoutConstant = 20U;
    timeouts.ReadTotalTimeoutMultiplier = 0U;
    timeouts.WriteTotalTimeoutConstant = 10000U;
    timeouts.WriteTotalTimeoutMultiplier = 0U;
    if (!SetCommTimeouts(hComm, &timeouts))
    {
        printf("Error setting timeouts\n");
        CloseHandle(hComm);
        return 1;
    }

    InitializeCriticalSection(&g_stats_lock);
    clear_stats_tx_reference();
    InterlockedExchange(&g_reader_running, 1);
    InterlockedExchange(&g_parser_reset_requested, 1);

    hThread = CreateThread(NULL, 0, ReceiveThread, NULL, 0, NULL);
    if (hThread == NULL)
    {
        printf("Error creating receive thread\n");
        DeleteCriticalSection(&g_stats_lock);
        CloseHandle(hComm);
        return 1;
    }

    printf("Connected to %s at %d bps.\n", argv[1], baud_rate);

    while (1)
    {
        if (!read_int_prompt("\nInput random data length to send (bytes, 0 to quit): ", &length_input))
        {
            printf("Invalid length input, please try again.\n");
            continue;
        }

        if (length_input == 0)
        {
            break;
        }

        if (length_input < 0)
        {
            printf("Length must be positive.\n");
            continue;
        }

        if (!read_int_prompt("Input round count for this stress test (>0): ", &rounds_input))
        {
            printf("Invalid round input, please try again.\n");
            continue;
        }

        if (rounds_input <= 0)
        {
            printf("Round count must be > 0.\n");
            continue;
        }

        for (round_idx = 1; round_idx <= rounds_input; round_idx++)
        {
            if (!run_single_round((size_t)length_input, round_idx, rounds_input))
            {
                exit_code = 1;
                goto cleanup;
            }
        }
    }

cleanup:
    InterlockedExchange(&g_reader_running, 0);
    if (hComm != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hComm);
        hComm = INVALID_HANDLE_VALUE;
    }

    WaitForSingleObject(hThread, 2000U);
    CloseHandle(hThread);

    DeleteCriticalSection(&g_stats_lock);
    return exit_code;
}