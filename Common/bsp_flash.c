/*
 * bsp_flash.c —— GD32F470 片内 Flash 擦/写（整条路线唯一必须 GD32 原生的一处）
 *
 * 完全自包含：不 #include 任何 GD32/STM32 器件头，本文件用到的 FMC 寄存器全部在
 * 文件内部私有定义。因此它与工程里 STM32 HAL 的 .c 永远不会有器件头冲突，可直接
 * 编进 BootLoader 工程与 APP 工程，无需引入 GD32 标准外设库（DFP）。
 *
 * 寄存器地址/位/key 取自 GD32F4xx 参考手册 FMC 章节（已对照 DFP 校验）：
 *   FMC 基址 0x40023C00；页擦除粒度 4KB；FMC_PECFG/FMC_PEKEY 是 GD32 的"页擦除"机制。
 *
 * 本文件必须在 RAM 里执行（擦/写 Flash 时 CPU 不能再从同一片 Flash 取指，否则总线挂死）：
 *   - GCC：函数已标 .ramfunc 段；
 *   - MDK：靠 .sct 把 bsp_flash.o 整体放进 ER_IRAM_FUNC（见逐步实施文档第六部分）。
 */
#include "bsp_flash.h"
#include "bl_partition.h"

/* STM32F407 FLASH 寄存器定义 (标准官方定义) */
#define FLASH_BASE 0x40023C00UL
#define FLASH_KEYR (*(volatile uint32_t *)(FLASH_BASE + 0x04UL))
#define FLASH_SR (*(volatile uint32_t *)(FLASH_BASE + 0x0CUL))
#define FLASH_CR (*(volatile uint32_t *)(FLASH_BASE + 0x10UL))

/* FLASH_SR 位定义 */
#define FLASH_SR_BSY (1UL << 16)
#define FLASH_SR_EOP (1UL << 0)
#define FLASH_SR_WRPERR (1UL << 4)
#define FLASH_SR_PGAERR (1UL << 5)
#define FLASH_SR_PGPERR (1UL << 6)
#define FLASH_SR_PGSERR (1UL << 7)
#define FLASH_SR_ERR_MASK (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

/* FLASH_CR 位定义 */
#define FLASH_CR_PG (1UL << 0)
#define FLASH_CR_SER (1UL << 1)
#define FLASH_CR_STRT (1UL << 16)
#define FLASH_CR_LOCK (1UL << 31)
#define FLASH_CR_SNB_POS 3UL
#define FLASH_CR_PSIZE_32BIT (2UL << 8)
#define FLASH_CR_SNB_MASK (0x0FUL << 3)

/* FLASH 解锁密钥 */
#define FLASH_KEY1 0x45670123UL
#define FLASH_KEY2 0xCDEF89ABUL

#ifndef BSP_RAMFUNC
#define BSP_RAMFUNC __attribute__((section(".ramfunc"), aligned(4)))
#endif

static BSP_RAMFUNC uint32_t flash_addr_to_sector(uint32_t addr);
static BSP_RAMFUNC int fmc_wait(void);
static BSP_RAMFUNC void fmc_unlock(void);
static BSP_RAMFUNC void fmc_lock(void);
static BSP_RAMFUNC void fmc_clear_flags(void);
static BSP_RAMFUNC int flash_sector_erase(uint32_t sector);

static BSP_RAMFUNC int fmc_wait(void)
{
    while (FLASH_SR & FLASH_SR_BSY)
        ;

    uint32_t sr = FLASH_SR;
    FLASH_SR |= FLASH_SR_EOP | FLASH_SR_ERR_MASK;

    return (sr & FLASH_SR_ERR_MASK) ? 0 : 1;
}

static BSP_RAMFUNC void fmc_unlock(void)
{
    if (FLASH_CR & FLASH_CR_LOCK)
    {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static BSP_RAMFUNC void fmc_lock(void)
{
    FLASH_CR |= FLASH_CR_LOCK;
}

static BSP_RAMFUNC void fmc_clear_flags(void)
{
    FLASH_SR |= FLASH_SR_EOP | FLASH_SR_ERR_MASK;
}

static BSP_RAMFUNC uint32_t flash_addr_to_sector(uint32_t addr)
{
    addr -= BL_FLASH_BASE_ADDR;

    if (addr < 0x00010000UL)
    {
        return addr / 0x4000UL;
    }
    else if (addr < 0x00020000UL)
    {
        return 4UL;
    }
    else
    {
        return 5UL + (addr - 0x00020000UL) / 0x20000UL;
    }
}

static BSP_RAMFUNC int flash_sector_erase(uint32_t sector)
{
    if (!fmc_wait())
        return 0;

    FLASH_CR &= ~FLASH_CR_SNB_MASK;
    FLASH_CR |= (sector << FLASH_CR_SNB_POS);
    FLASH_CR |= FLASH_CR_SER;
    FLASH_CR |= FLASH_CR_STRT;

    int ret = fmc_wait();

    FLASH_CR &= ~FLASH_CR_SER;
    return ret;
}

BSP_RAMFUNC bool BSP_Flash_ErasePages(uint32_t addr, uint32_t size)
{
    if (size == 0 || addr < BL_FLASH_BASE_ADDR)
        return false;

    uint32_t end_addr = addr + size - 1;
    if (end_addr > BL_FLASH_END_ADDR)
        return false;

    uint32_t start_sec = flash_addr_to_sector(addr);
    uint32_t end_sec = flash_addr_to_sector(end_addr);

    fmc_unlock();
    fmc_clear_flags();

    for (uint32_t s = start_sec; s <= end_sec; s++)
    {
        if (!flash_sector_erase(s))
        {
            fmc_lock();
            return false;
        }
    }

    fmc_lock();
    return true;
}

BSP_RAMFUNC bool BSP_Flash_WriteWord(uint32_t addr, uint32_t data)
{
    if (addr < BL_FLASH_BASE_ADDR || addr > BL_FLASH_END_ADDR)
        return false;
    if (addr & 3)
        return false;

    fmc_unlock();
    fmc_clear_flags();

    if (!fmc_wait())
    {
        fmc_lock();
        return false;
    }

    FLASH_CR &= ~(0x3UL << 8);
    FLASH_CR |= FLASH_CR_PSIZE_32BIT;
    FLASH_CR |= FLASH_CR_PG;

    *(volatile uint32_t *)addr = data;
    int ok = fmc_wait();

    FLASH_CR &= ~FLASH_CR_PG;
    fmc_lock();

    return ok ? true : false;
}

BSP_RAMFUNC bool BSP_Flash_Program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0)
        return false;
    if (addr < BL_FLASH_BASE_ADDR || (addr + len - 1) > BL_FLASH_END_ADDR)
        return false;

    fmc_unlock();
    fmc_clear_flags();

    for (uint32_t i = 0; i < len; i++)
    {
        uint32_t a = addr + i;
        uint8_t d = data[i];

        if (!fmc_wait())
        {
            fmc_lock();
            return false;
        }

        FLASH_CR &= ~(0x3UL << 8);
        FLASH_CR |= FLASH_CR_PG;
        *(volatile uint8_t *)a = d;
        fmc_wait();
        FLASH_CR &= ~FLASH_CR_PG;
    }

    fmc_lock();
    return true;
}

void BSP_Flash_Init(void)
{
    // STM32F4 无需初始化，解锁即可
}
