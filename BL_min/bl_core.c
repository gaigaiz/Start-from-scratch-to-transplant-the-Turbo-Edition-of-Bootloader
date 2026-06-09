#include "bl_core.h"                                /* 本模块对外声明 bootloader_run() */
#include "bl_partition.h"                           /* Flash 分区地址宏（BL/APP1/APP2/参数页） */
#include "bl_param.h"                               /* 参数页结构 bl_param_t / 日志 / 各种魔数 */
#include "crc32.h"                                  /* 统一的软件 CRC32（与 APP、PC 一致） */
#include "bsp_flash.h"                              /* 中性 Flash 擦/写接口（内部是 GD32 FMC） */
#include "stm32f4xx.h"                              /* CMSIS：SCB / NVIC / __disable_irq / NVIC_SystemReset 等 */
#include "usart.h"                                  /* CubeMX 生成：extern UART_HandleTypeDef huart3（OTA/BL 专用口 PB10/PB11） */
#include <string.h>                                 /* memcpy / memset */

typedef void (*app_entry_t)(void);                  /* 函数指针类型：指向 APP 的复位向量（跳转用） */

static uint8_t s_page_cache[BL_PARAM_PAGE_SIZE];    /* 参数页整页 RAM 缓存（读整页→改→擦→整页写回） */

/* ---------- 串口打印（走 OTA/BL 专用口 USART3 = 物理 PB10/PB11） ---------- */
static void bl_print(const char *s)
{
    uint32_t n = 0;                                 /* 字符串长度计数 */
    while (s[n]) n++;                                /* 数到字符串结尾（不含 '\0'） */
    HAL_UART_Transmit(&huart3, (uint8_t *)s, (uint16_t)n, 100);  /* 阻塞发送，超时 100ms */
}

/* ---------- 参数页：校验 / 默认值 / 日志 / 回写 ---------- */

/* 算参数结构的 CRC：覆盖范围 = 结构起始 ~ param_crc32 字段之前 */
static uint32_t param_calc_crc(const bl_param_t *p)
{
    return crc32_calc((const uint8_t *)p, (uint32_t)offsetof(bl_param_t, param_crc32));  /* offsetof 得 param_crc32 的偏移=被校验长度 */
}

/* 判断一份参数是否有效（魔数/版本/地址/大小/CRC 全过才算有效） */
static bool param_valid(const bl_param_t *p)
{
    if (p->magic != BL_PARAM_MAGIC || p->tail_magic != BL_PARAM_TAIL_MAGIC) return false;  /* 首尾魔数都得对 */
    if (p->version != BL_PARAM_VERSION) return false;                                       /* 版本号要匹配 */
    if (p->app1_addr != BL_APP1_START_ADDR || p->app2_addr != BL_APP2_START_ADDR) return false;  /* 防错配分区 */
    if (p->app_size > BL_APP1_SIZE || p->app_size > BL_APP2_SIZE) return false;              /* 镜像不能超过槽大小 */
    return param_calc_crc(p) == p->param_crc32;                                             /* 最后比 CRC */
}

/* 把一份参数重置成出厂默认（主备都坏时用它重建） */
static void param_set_default(bl_param_t *p)
{
    memset(p, 0, sizeof(*p));                        /* 整块清零 */
    p->magic       = BL_PARAM_MAGIC;                 /* 写头魔数 */
    p->version     = BL_PARAM_VERSION;               /* 写版本号 */
    p->update_flag = BL_UPDATE_FLAG_IDLE;            /* 默认空闲（正常启动） */
    p->app1_addr   = BL_APP1_START_ADDR;             /* 填执行区地址 */
    p->app2_addr   = BL_APP2_START_ADDR;             /* 填暂存区地址 */
    p->tail_magic  = BL_PARAM_TAIL_MAGIC;            /* 写尾魔数 */
    p->param_crc32 = param_calc_crc(p);              /* 最后补上 CRC */
}

/* 往参数页的环形日志区写一条日志（先写进 RAM 缓存，随整页回写时落盘） */
static void log_put(bl_param_t *w, uint32_t event_id, uint32_t result,
                     uint32_t v0, uint32_t v1, uint32_t v2)
{
    bl_log_entry_t e;                                                   /* 临时日志条目 */
    uint32_t idx = w->log_write_index % BL_LOG_ENTRY_COUNT;             /* 写指针取模 → 环形槽位 */
    memset(&e, 0, sizeof(e));                                           /* 清零 */
    e.magic    = BL_LOG_MAGIC;                                          /* 条目魔数 */
    e.seq      = w->update_counter + w->fail_counter;                   /* 序号 = 成功数+失败数 */
    e.event_id = event_id;                                              /* 事件类型 */
    e.result   = result;                                                /* 1=成功 0=失败 */
    e.value0   = v0; e.value1 = v1; e.value2 = v2;                      /* 三个上下文值 */
    e.crc32    = crc32_calc((const uint8_t *)&e, (uint32_t)offsetof(bl_log_entry_t, crc32));  /* 算本条 CRC */
    memcpy(&s_page_cache[(BL_LOG_ADDR - BL_PARAM_PAGE_ADDR) + idx * BL_LOG_ENTRY_SIZE],
           &e, sizeof(e));                                              /* 写进页缓存对应槽位 */
    w->log_write_index++;                                               /* 写指针递增 */
}

/* 把一份参数整页回写 Flash：读整页→主备都填这份→重算 CRC→擦页→整页写回 */
static bool commit_param_page(bl_param_t *param)
{
    bl_param_t copy;                                                                 /* 待写入的副本 */
    memcpy(s_page_cache, (const void *)BL_PARAM_PAGE_ADDR, BL_PARAM_PAGE_SIZE);       /* 先把整页（含日志区）读进缓存 */
    memcpy(&copy, param, sizeof(copy));                                              /* 复制传入参数 */
    copy.param_crc32 = param_calc_crc(&copy);                                        /* 重算 CRC（字段可能被改过） */
    memcpy(&s_page_cache[BL_PARAM_MAIN_ADDR   - BL_PARAM_PAGE_ADDR], &copy, sizeof(copy));  /* 写进缓存里的"主副本"位置 */
    memcpy(&s_page_cache[BL_PARAM_BACKUP_ADDR - BL_PARAM_PAGE_ADDR], &copy, sizeof(copy));  /* 写进"备副本"位置（主备同值） */
    if (!BSP_Flash_ErasePages(BL_PARAM_PAGE_ADDR, BL_PARAM_PAGE_SIZE)) return false;  /* 擦掉参数页 */
    return BSP_Flash_Program(BL_PARAM_PAGE_ADDR, s_page_cache, BL_PARAM_PAGE_SIZE);   /* 整页写回（含日志） */
}

/* ---------- 镜像合法性 / 拷贝 / 跳转 ---------- */

/* 判断 base 处是不是一份合法固件（看向量表前两个字：MSP 在 SRAM、复位向量在 Flash） */
static bool app_vector_valid(uint32_t base)
{
    uint32_t msp = *(volatile uint32_t *)base;                          /* 向量表[0] = 初始主栈指针 */
    uint32_t rst = *(volatile uint32_t *)(base + 4UL);                  /* 向量表[1] = 复位向量 */
    if ((msp & 0x2FFE0000UL) != 0x20000000UL) return false;             /* MSP 必须落在 SRAM 区（高位特征 0x2000xxxx） */
    if (rst < BL_FLASH_BASE_ADDR || rst > BL_FLASH_END_ADDR) return false;  /* 复位向量必须落在 Flash 区 */
    return true;                                                        /* 两项都合理 → 像一份真固件（空 Flash 全 0xFF 会被否决） */
}

/* 把 APP2 暂存区的 app_size 字节拷到 APP1 执行区（先擦 APP1 需要的页，再分块拷） */
static bool copy_app2_to_app1(uint32_t app_size)
{
    uint8_t buf[BL_COPY_CHUNK_SIZE];                                    /* 256 字节中转缓冲（先读到 RAM 再写） */
    uint32_t copied = 0, erase, left, n;                                /* copied=已拷字节，erase=要擦的字节数 */

    if (app_size == 0 || app_size > BL_APP1_SIZE || app_size > BL_APP2_SIZE) return false;  /* 大小非法 */

    erase = (app_size + BL_FLASH_PAGE_SIZE - 1UL) & ~(BL_FLASH_PAGE_SIZE - 1UL);  /* app_size 向上取整到整页 */
    if (!BSP_Flash_ErasePages(BL_APP1_START_ADDR, erase)) return false;            /* 擦掉 APP1 用到的那些页 */

    while (copied < app_size) {                                         /* 直到拷完 */
        left = app_size - copied;                                       /* 还剩多少没拷 */
        n = (left > BL_COPY_CHUNK_SIZE) ? BL_COPY_CHUNK_SIZE : left;     /* 本次拷 min(剩余, 256) */
        memcpy(buf, (const void *)(BL_APP2_START_ADDR + copied), n);     /* 从 APP2 读一块到 RAM（此刻未擦未写，读 Flash 安全） */
        if (!BSP_Flash_Program(BL_APP1_START_ADDR + copied, buf, n)) return false;  /* 写进 APP1 对应位置 */
        copied += n;                                                    /* 累加进度 */
    }
    return true;                                                        /* 全部拷完 */
}

/* 干净地跳到 base 处的固件运行（关中断/关 SysTick/清 NVIC/重定位 VTOR/换栈/跳复位向量） */
static void jump_to_app(uint32_t base)
{
    app_entry_t entry;                                                  /* APP 入口（复位向量） */
    uint32_t i;
    __disable_irq();                                                    /* 先关总中断，下面拆外设不能被打断 */
    SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;             /* 关掉 SysTick，免得进 APP 后还按 BL 的节拍中断 */
    for (i = 0; i < 8UL; i++) { NVIC->ICER[i] = 0xFFFFFFFFUL; NVIC->ICPR[i] = 0xFFFFFFFFUL; }  /* 禁用并清除所有外部中断 */
    __DSB(); __ISB();                                                   /* 数据/指令同步屏障，确保上面写入生效 */
    SCB->VTOR = base;                                                   /* 中断向量表重定位到 APP，否则 APP 中断会跳进 BL 表 */
    __set_MSP(*(volatile uint32_t *)base);                              /* 把主栈指针换成 APP 的初始 MSP */
    entry = (app_entry_t)(*(volatile uint32_t *)(base + 4UL));          /* 取 APP 复位向量地址 */
    __enable_irq();                                                     /* 开中断 */
    entry();                                                            /* 跳过去，控制权移交 APP，不再返回 */
}

/* 上电兜底：把环形日志区里有效的条目通过串口打印出来，便于现场排障 */
static void log_dump_uart(void)
{
    char line[64];                                                      /* 单行打印缓冲 */
    uint32_t i, k;
    bl_log_entry_t e;
    bl_print("BL: --- log dump ---\r\n");                               /* 打印分隔头 */
    for (i = 0; i < BL_LOG_ENTRY_COUNT; i++) {                          /* 遍历 32 条 */
        memcpy(&e, (const void *)(BL_LOG_ADDR + i * BL_LOG_ENTRY_SIZE), sizeof(e));  /* 读出第 i 条 */
        if (e.magic != BL_LOG_MAGIC) continue;                          /* 魔数不对 → 空槽，跳过 */
        if (e.crc32 != crc32_calc((const uint8_t *)&e, (uint32_t)offsetof(bl_log_entry_t, crc32))) continue;  /* CRC 不过 → 损坏，跳过 */
        const char hex[] = "0123456789ABCDEF";                          /* 十六进制字符表 */
        uint32_t vals[4] = { e.seq, e.event_id, e.result, e.value0 };   /* 挑 4 个关键值打印 */
        k = 0;
        for (uint32_t v = 0; v < 4; v++) {                              /* 4 个值 */
            for (int b = 28; b >= 0; b -= 4) line[k++] = hex[(vals[v] >> b) & 0xF];  /* 每个值按 8 位十六进制输出 */
            line[k++] = ' ';                                            /* 值之间空格分隔 */
        }
        line[k++] = '\r'; line[k++] = '\n'; line[k] = 0;                /* 行尾 + 字符串结束 */
        bl_print(line);                                                 /* 打印这一行 */
    }
}

/* ---------- BootLoader 主流程 ---------- */
void bootloader_run(void)
{
    bl_param_t m, b, w;                                                 /* m=主副本 b=备副本 w=最终选用的工作副本 */
    bool mv, bv;                                                        /* 主/备是否有效 */

    memcpy(&m, (const void *)BL_PARAM_MAIN_ADDR,   sizeof(m));          /* 读主副本到 RAM */
    memcpy(&b, (const void *)BL_PARAM_BACKUP_ADDR, sizeof(b));          /* 读备副本到 RAM */
    mv = param_valid(&m);                                               /* 校验主副本 */
    bv = param_valid(&b);                                               /* 校验备副本 */

    if (mv && bv) {                                                     /* 主备都有效 */
        w = (m.update_counter >= b.update_counter) ? m : b;             /* 取 update_counter 大的（较新的）那份 */
        if (!(m.update_counter >= b.update_counter)) {                  /* 如果选了备份（说明主偏旧/坏） */
            log_put(&w, BL_LOG_EVENT_PARAM_RECOVER, 1, 0, 0, 0);        /* 记一条"参数修复"日志 */
            commit_param_page(&w);                                      /* 用新的覆盖回去，修好主副本 */
        }
    } else if (mv) {                                                    /* 只有主有效 */
        w = m; log_put(&w, BL_LOG_EVENT_PARAM_RECOVER, 1, 1, 0, 0); commit_param_page(&w);  /* 用主修复备 */
    } else if (bv) {                                                    /* 只有备有效 */
        w = b; log_put(&w, BL_LOG_EVENT_PARAM_RECOVER, 1, 2, 0, 0); commit_param_page(&w);  /* 用备修复主 */
    } else {                                                            /* 主备都坏 */
        param_set_default(&w);                                          /* 重建默认参数 */
        w.last_error = BL_ERR_PARAM_INVALID;                            /* 记下"参数曾全坏" */
        log_put(&w, BL_LOG_EVENT_PARAM_RECOVER, 0, BL_ERR_PARAM_INVALID, 0, 0);  /* 记失败日志 */
        commit_param_page(&w);                                          /* 写回默认参数 */
    }

    if (w.update_flag == BL_UPDATE_FLAG_PENDING) {                      /* 留言板=PENDING：有新固件待装 */
        bool ok = false;                                               /* 升级是否成功 */
        if (w.app_size && w.app_size <= BL_APP2_SIZE                    /* app_size 合法 */
            && app_vector_valid(BL_APP2_START_ADDR)                     /* APP2 像一份合法固件 */
            && crc32_calc((const uint8_t *)BL_APP2_START_ADDR, w.app_size) == w.app_crc32) {  /* APP2 整镜像 CRC 通过 */
            bl_print("BL: APP2 ok, copying...\r\n");                    /* 打印进度 */
            if (copy_app2_to_app1(w.app_size)                          /* 擦 APP1 并把 APP2 拷过去 */
                && crc32_calc((const uint8_t *)BL_APP1_START_ADDR, w.app_size) == w.app_crc32) {  /* 拷完再校验 APP1（确认真的拷对） */
                ok = true;                                              /* 一路都过 → 成功 */
            }
        }
        if (ok) {                                                       /* 升级成功 */
            w.update_flag = BL_UPDATE_FLAG_IDLE;                        /* 留言板改回 IDLE */
            w.update_counter++;                                         /* 成功计数 +1 */
            w.last_error = BL_ERR_NONE;                                 /* 清错误码 */
            log_put(&w, BL_LOG_EVENT_UPDATE_OK, 1, w.app_size, w.app_crc32, 0);  /* 记成功日志 */
            bl_print("BL: update OK\r\n");
        } else {                                                        /* 升级失败 */
            w.update_flag = BL_UPDATE_FLAG_FAILED;                      /* 标记失败（旧 APP1 在未进擦除前仍可用） */
            w.fail_counter++;                                           /* 失败计数 +1 */
            w.last_error = BL_ERR_APP2_INVALID;                         /* 记错误码 */
            log_put(&w, BL_LOG_EVENT_UPDATE_FAIL, 0, w.app_size, w.app_crc32, 0);  /* 记失败日志 */
            bl_print("BL: update FAIL\r\n");
        }
        commit_param_page(&w);                                          /* 把结果写回参数页 */
        NVIC_SystemReset();                                             /* 复位；复位后不再是 PENDING，走下面正常分支 */
    }

    if (app_vector_valid(BL_APP1_START_ADDR)) {                        /* 正常分支：APP1 是合法固件 */
        bl_print("BL: jumping to app...\r\n");                          /* 打印 */
        jump_to_app(BL_APP1_START_ADDR);                                /* 跳过去运行 APP1（不返回） */
    }

    /* 兜底：没有可运行的 APP1（空/坏/从没烧过）——故意停在 BL，不跳飞，方便排障/再 OTA */
    w.last_error = BL_ERR_APP1_INVALID;                                 /* 记错误码 */
    log_put(&w, BL_LOG_EVENT_JUMP_FAIL, 0, BL_ERR_APP1_INVALID, 0, 0);  /* 记"无 APP 可跳"日志 */
    commit_param_page(&w);                                              /* 写回参数页 */
    log_dump_uart();                                                    /* 把历史日志 dump 到串口 */
    bl_print("BL: no valid APP, halt.\r\n");                            /* 提示停住 */
    while (1) { }                                                       /* 死循环停在 BootLoader（设备可观测、还能再 OTA 救活） */
}
