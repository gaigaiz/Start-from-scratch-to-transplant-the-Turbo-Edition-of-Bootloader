#ifndef BL_PARAM_H                                  /* 头文件保护，防重复包含 */
#define BL_PARAM_H
#include <stdint.h>                                 /* uint32_t 等定长整数 */
#include <stdbool.h>                                /* bool / true / false */
#include <stddef.h>                                 /* offsetof 宏（算结构体成员偏移，用于 CRC 范围） */
#include "bl_partition.h"                           /* 复用分区地址宏（参数页地址等） */

/* ========== 参数页内三块区域的地址 ========== */
#define BL_PARAM_PAGE_ADDR    0x0800C000UL           /* 参数页整页起始（4KB 一整页） */
#define BL_PARAM_PAGE_SIZE    0x00001000UL           /* 参数页大小 4KB（擦除/回写以整页为单位） */
#define BL_PARAM_MAIN_ADDR    0x0800C000UL           /* 主副本地址（页首 +0x000） */
#define BL_PARAM_BACKUP_ADDR  0x0800C100UL           /* 备副本地址（页内 +0x100），主坏了用它恢复 */
#define BL_LOG_ADDR           0x0800C200UL           /* 环形日志区起始（页内 +0x200） */
#define BL_LOG_ENTRY_SIZE     32UL                  /* 每条日志 32 字节 */
#define BL_LOG_ENTRY_COUNT    32UL                  /* 日志环共 32 条，写满回绕覆盖最旧 */

/* ========== 校验/版本魔数（用非平凡常量抗位翻转） ========== */
#define BL_PARAM_MAGIC        0x5AA5C33CUL           /* 参数结构头魔数，标识"这是一份合法参数" */
#define BL_PARAM_TAIL_MAGIC   0xA5A5C3C3UL           /* 参数结构尾魔数，首尾都对才认 */
#define BL_PARAM_VERSION      0x00010001UL           /* 参数结构版本号，结构变了改这里 */

/* ========== update_flag 取值：APP 与 BL 握手用的核心标志 ========== */
#define BL_UPDATE_FLAG_IDLE     0x00000000UL         /* 空闲：无升级，正常启动 */
#define BL_UPDATE_FLAG_PENDING  0xAA55AA55UL         /* 待升级：APP 已写好 APP2，请 BL 执行升级 */
#define BL_UPDATE_FLAG_FAILED   0xDEAD0001UL         /* 升级失败：校验/拷贝出错（保住旧 APP1） */

/* ========== 错误码（记进参数页 last_error，便于排障） ========== */
#define BL_ERR_NONE          0UL                     /* 无错误 */
#define BL_ERR_PARAM_INVALID 1UL                     /* 参数页主备都坏，已重建默认值 */
#define BL_ERR_APP2_INVALID  2UL                     /* APP2 向量表非法或整镜像 CRC 不过 */
#define BL_ERR_COPY_FAILED   3UL                     /* APP2→APP1 拷贝后 CRC 不过 */
#define BL_ERR_APP1_INVALID  4UL                     /* 无可运行 APP1，停在 BL */

/* ========== 日志条目相关常量 ========== */
#define BL_LOG_MAGIC               0xB100B100UL      /* 日志条目魔数，区分有效条目与空 Flash(0xFF) */
#define BL_LOG_EVENT_PARAM_RECOVER 1UL               /* 事件：参数页修复 */
#define BL_LOG_EVENT_UPDATE_OK     2UL               /* 事件：升级成功 */
#define BL_LOG_EVENT_UPDATE_FAIL   3UL               /* 事件：升级失败 */
#define BL_LOG_EVENT_JUMP_FAIL     4UL               /* 事件：无 APP 可跳，停在 BL */

#define BL_COPY_CHUNK_SIZE   256UL                  /* BL 拷贝 APP2→APP1 时每块 256 字节 */

/* ========== 参数结构体（固定 256 字节；主备各占 256，间距 0x100=256） ========== */
typedef struct {                                    /* CRC 覆盖范围 = 结构起始 ~ param_crc32 之前 */
    uint32_t magic;                                 /* 头魔数，应 == BL_PARAM_MAGIC */
    uint32_t version;                               /* 版本号，应 == BL_PARAM_VERSION */
    uint32_t update_flag;                           /* 升级标志：IDLE/PENDING/FAILED */
    uint32_t app_size;                              /* 待升级镜像的字节数 */
    uint32_t app_crc32;                             /* 待升级镜像的整镜像 CRC32 */
    uint32_t app1_addr;                             /* 执行区地址，应 == BL_APP1_START_ADDR（防错配） */
    uint32_t app2_addr;                             /* 暂存区地址，应 == BL_APP2_START_ADDR */
    uint32_t update_counter;                        /* 升级成功计数：主/备择新时取大者为新 */
    uint32_t fail_counter;                          /* 升级失败计数 */
    uint32_t last_error;                            /* 最近一次错误码 BL_ERR_* */
    uint32_t log_write_index;                       /* 日志环写指针（递增，取模得槽位） */
    uint32_t reserved[51];                          /* 预留，把结构体补满到 256 字节；新增字段占这里 */
    uint32_t param_crc32;                           /* 上面所有字段的 CRC32（不含本字段及之后） */
    uint32_t tail_magic;                            /* 尾魔数，应 == BL_PARAM_TAIL_MAGIC */
} bl_param_t;                                       /* 整个结构正好 256 字节 */

/* ========== 单条日志结构（固定 32 字节，× 32 条 = 1KB 日志环） ========== */
typedef struct {
    uint32_t magic;                                 /* 条目魔数，应 == BL_LOG_MAGIC */
    uint32_t seq;                                   /* 序号 = update_counter + fail_counter */
    uint32_t event_id;                              /* 事件类型 BL_LOG_EVENT_* */
    uint32_t result;                                /* 结果：1=成功 0=失败 */
    uint32_t value0;                                /* 上下文 0（如错误码） */
    uint32_t value1;                                /* 上下文 1（如 app_size） */
    uint32_t value2;                                /* 上下文 2（如 app_crc32） */
    uint32_t crc32;                                 /* 前面 7 个字段的 CRC32，校验本条是否有效 */
} bl_log_entry_t;                                   /* 正好 32 字节 */

#endif /* BL_PARAM_H */
