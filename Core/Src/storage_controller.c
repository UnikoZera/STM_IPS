/*
 * storage_controller.c
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 */

#include "storage_controller.h"

/**
 * @brief 计算数据块的CRC16校验值，使用标准的CRC-16-CCITT算法。
 * 
 * @param data 输入数据块的指针
 * @param size 输入数据块的大小（字节数）
 * @param crc 输出参数，计算得到的CRC16校验值
 */
static void mem_crc16(const uint8_t *data, uint32_t size, uint16_t *crc)
{
    uint16_t crc_accum = 0xFFFFU;
    for (uint32_t i = 0; i < size; i++)
    {
        crc_accum ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if ((crc_accum & 0x8000U) != 0U)
            {
                crc_accum = (crc_accum << 1) ^ 0x1021U;
            }
            else
            {
                crc_accum <<= 1;
            }
        }
    }
    *crc = crc_accum;
}
