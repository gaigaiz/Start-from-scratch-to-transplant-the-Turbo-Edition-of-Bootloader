#include "crc32.h"                                  /* 本模块声明 */

/* 滚动累加：对 data[0..len) 逐字节更新 crc，返回中间值（未做末值异或，可继续喂） */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i, j;                                  /* i 遍历字节，j 遍历该字节的 8 位 */
    for (i = 0; i < len; i++) {                     /* 逐字节处理 */
        crc ^= data[i];                             /* 当前字节异或进 crc 低 8 位 */
        for (j = 0; j < 8; j++)                     /* 每字节处理 8 位 */
            crc = (crc & 1UL) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);  /* 最低位为1则右移后异或多项式，否则只右移（反射式 CRC） */
    }
    return crc;                                     /* 返回累加中间值 */
}

/* 滚动收尾：把累加中间值异或 0xFFFFFFFF，得到最终 CRC32 */
uint32_t crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFUL;                       /* 标准 CRC-32 的末值异或 */
}

/* 便捷接口：一次性算一整段数据的 CRC32（初值 + 累加 + 末值异或一气呵成） */
uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    return crc32_final(crc32_update(CRC32_INIT, data, len));  /* = 收尾( 累加( 初值, data ) ) */
}
