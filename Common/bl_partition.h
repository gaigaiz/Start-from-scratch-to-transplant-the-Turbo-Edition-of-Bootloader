#ifndef BL_PARTITION_H
#define BL_PARTITION_H
#include <stdint.h>

/* ========== STM32F407VET6 片内 Flash 总体参数 ========== */
#define BL_FLASH_BASE_ADDR 0x08000000UL  /* 起始地址不变 */
#define BL_FLASH_TOTAL_SIZE 0x00080000UL /* 512KB 总容量不变 */
#define BL_FLASH_END_ADDR (BL_FLASH_BASE_ADDR + BL_FLASH_TOTAL_SIZE - 1UL)
#define BL_FLASH_PAGE_SIZE 0x00004000UL /* 关键：STM32F407 页大小 16KB */

/* ========== 分区 1：BootLoader 自身（对齐 16KB 页） ========== */
#define BL_BOOT_START_ADDR 0x08000000UL /* 起始地址不变 */
#define BL_BOOT_SIZE 0x00010000UL       /* 调整为 64KB（4 个 16KB 页），原 48KB 非 16KB 整数倍 */

/* ========== 分区 2：参数页（必须 1 个完整页） ========== */
#define BL_PARAM_START_ADDR 0x08010000UL /* 紧接 BootLoader，16KB 页边界 */
#define BL_PARAM_SIZE 0x00004000UL       /* 1 个 STM32F4 页（16KB），原 4KB 过小且非页对齐 */

/* ========== 分区 3：APP1 执行区（按页对齐） ========== */
#define BL_APP1_START_ADDR 0x08014000UL /* 紧接参数页，16KB 页边界 */
#define BL_APP1_SIZE 0x00068000UL       /* 416KB（26 个 16KB 页），保证后续分区对齐 */
#define BL_APP1_END_ADDR (BL_APP1_START_ADDR + BL_APP1_SIZE - 1UL)

/* ========== 分区 4：APP2 暂存区（与 APP1 等大，页对齐） ========== */
#define BL_APP2_START_ADDR 0x0807C000UL /* 紧接 APP1，16KB 页边界 */
#define BL_APP2_SIZE 0x00004000UL       /* 16KB（1 个页），注：原 224KB 超出 512KB 总容量，需压缩（见说明） */
#define BL_APP2_END_ADDR (BL_APP2_START_ADDR + BL_APP2_SIZE - 1UL)

/* ========== 分区 5：预留用户数据（剩余空间） ========== */
#define BL_DATA_START_ADDR 0x08080000UL - BL_DATA_SIZE /* 从末尾向前算 */
#define BL_DATA_SIZE 0x00000000UL                      /* 若需保留，需重新规划总容量（见说明） */

#endif /* BL_PARTITION_H */