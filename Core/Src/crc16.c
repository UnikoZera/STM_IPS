#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 查找表（运行时填充） */
static uint16_t crc16_usb_table[256];
static bool crc16_usb_table_ok = false;

/**
 * @brief 初始化 CRC16/USB 查找表（须在其他函数之前调用一次）
 */
void crc16_usb_init_table(void)
{
    const uint16_t rev_poly = 0xA001;   /* 0x8005 的位反转 */
    for (int i = 0; i < 256; i++) {
        uint16_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ rev_poly;
            } else {
                crc >>= 1;
            }
        }
        crc16_usb_table[i] = crc;
    }
    crc16_usb_table_ok = true;
}

/**
 * @brief 计算 CRC16/USB（纯数据部分）
 * @param data  数据首地址
 * @param len   数据字节数
 * @return CRC16 校验值（已包含输出异或 0xFFFF）
 */
uint16_t crc16_usb_calc(const uint8_t *data, size_t len)
{
    if (!crc16_usb_table_ok) {
        /* 若未调用过 init，则此处自动初始化（不推荐，最好由用户显式调用） */
        crc16_usb_init_table();
    }
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc = (crc >> 8) ^ crc16_usb_table[(crc & 0xFF) ^ *data++];
    }
    return crc ^ 0xFFFF;   /* 关键：XorOut = 0xFFFF */
}

/**
 * @brief 校验/追加 CRC16/USB（兼容您的原接口）
 * @param data    数据包首地址
 * @param len     数据包总长度（含 CRC 时为全长，不含时为纯数据长度）
 * @param has_crc  true: 包末尾带有 CRC（小端序），需要校验；
 *                 false: 包末尾无 CRC，计算并返回 CRC 值
 * @return has_crc 为 true  时：1=校验通过，0=校验失败；
 *         has_crc 为 false 时：返回计算好的 CRC16 值
 */
uint16_t crc16_usb_packing(const uint8_t *data, size_t len, bool has_crc)
{
    if (has_crc) {
        if (len < 2) return 0;

        /* 提取包末尾的小端 CRC */
        uint16_t received_crc = (uint16_t)data[len - 1] << 8 | data[len - 2];
        uint16_t calculated_crc = crc16_usb_calc(data, len - 2);
        return (received_crc == calculated_crc) ? 1 : 0;
    } else {
        return crc16_usb_calc(data, len);
    }
}