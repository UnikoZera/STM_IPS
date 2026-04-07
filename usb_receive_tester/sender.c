#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdint.h>
#include <time.h>

HANDLE hComm;

DWORD WINAPI ReceiveThread(LPVOID lpParam) {
    uint8_t buffer[4096];
    DWORD bytesRead;
    while (1) {
        if (ReadFile(hComm, buffer, sizeof(buffer), &bytesRead, NULL)) {
            if (bytesRead > 0) {
                printf("\n[RX] %lu bytes: ", bytesRead);
                for(DWORD i=0; i<bytesRead; i++) {
                    printf("%02X ", buffer[i]);
                }
                printf("\n> ");
            }
        } else {
            break;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <COM_PORT> <BAUD_RATE>\n", argv[0]);
        printf("Example: %s COM24 921600\n", argv[0]);
        return 1;
    }

    char port_name[32];
    snprintf(port_name, sizeof(port_name), "\\\\.\\%s", argv[1]);
    int baud_rate = atoi(argv[2]);

    hComm = CreateFileA(port_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hComm == INVALID_HANDLE_VALUE) {
        printf("Error opening %s\n", port_name);
        return 1;
    }

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hComm, &dcbSerialParams)) {
        printf("Error getting state\n");
        CloseHandle(hComm);
        return 1;
    }

    dcbSerialParams.BaudRate = baud_rate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;
    if (!SetCommState(hComm, &dcbSerialParams)) {
        printf("Error setting state\n");
        CloseHandle(hComm);
        return 1;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hComm, &timeouts);

    srand((unsigned int)time(NULL));

    HANDLE hThread = CreateThread(NULL, 0, ReceiveThread, NULL, 0, NULL);
    if (hThread == NULL) {
        printf("Error creating thread\n");
        CloseHandle(hComm);
        return 1;
    }

    printf("Connected to %s at %d bps.\n", argv[1], baud_rate);
    printf("Type a length (e.g. 1024) to send random data, or 0 to exit.\n");

    while (1) {
        int length;
        printf("\n> ");
        if (scanf("%d", &length) != 1 || length <= 0) {
            break;
        }

        uint8_t *data = (uint8_t*)malloc(length);
        if (data == NULL) {
            printf("Memory allocation failed!\n");
            continue;
        }

        for (int i = 0; i < length; i++) {
            data[i] = rand() % 256;
        }

        DWORD bytesWritten;
        printf("[TX] Sending %d bytes: ", length);
        for (int i = 0; i < length; i++) {
            printf("%02X ", data[i]);
        }
        printf("\n");
        
        if (WriteFile(hComm, data, length, &bytesWritten, NULL)) {
            printf("[TX] Successfully sent %lu bytes.\n", bytesWritten);
        } else {
            printf("[TX] Send failed!\n");
        }
        free(data);
    }

    CloseHandle(hComm);
    return 0;
}