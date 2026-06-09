#ifndef CRC32_H                                     /* 头文件保护 */
#define CRC32_H
#include <stdint.h>                                 /* uint32_t / uint8_t */

#define CRC32_INIT  0xFFFFFFFFUL                     /* CRC 初值（滚动计算的起始值） */

/* 标准 CRC-32/ISO-HDLC：初值 0xFFFFFFFF、多项式 0xEDB88320（反射）、末值再异或 0xFFFFFFFF。
   与 Python binascii.crc32 完全一致；BL / APP / PC 三处必须用同一套算法，否则升级永远校验失败。
   不要用 GD32 硬件 CRC 外设替代（多项式/字节序不同，对不上）。 */

uint32_t crc32_calc(const uint8_t *data, uint32_t len);                 /* 一次性算完一段数据（内部含初值+末值异或） */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len); /* 滚动累加：首次 crc 传 CRC32_INIT，可分多次喂 */
uint32_t crc32_final(uint32_t crc);                                     /* 滚动收尾：把累加值异或 0xFFFFFFFF 得最终结果 */

#endif /* CRC32_H */
