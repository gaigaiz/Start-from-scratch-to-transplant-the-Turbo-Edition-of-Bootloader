#ifndef BSP_FLASH_H                                 /* 头文件保护 */
#define BSP_FLASH_H
#include <stdint.h>                                 /* uint32_t / uint8_t */
#include <stdbool.h>                                /* bool 返回值 */

/* 中性接口层：这里只出现纯 C 类型，不出现任何 GD32/STM32 器件类型或寄存器宏。
   所以 BL / APP / 任何 .c 都能放心 #include 本文件，不会引入器件头冲突。 */

/* 按 4KB 页擦除：把 [addr, addr+size) 覆盖到的所有整页擦掉。返回 true=成功 */
bool BSP_Flash_ErasePages(uint32_t addr, uint32_t size);

/* 写 Flash：地址 4 字节对齐且剩余 ≥4 时按字写，否则逐字节写。返回 true=成功 */
bool BSP_Flash_Program(uint32_t addr, const uint8_t *data, uint32_t size);

/* 说明：无需 SetWaitStates —— GD32F470 的 FMC 默认等待周期已覆盖 ≤240MHz 全频段，120MHz 无需设置 */
#endif /* BSP_FLASH_H */
