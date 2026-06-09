#ifndef OTA_PROTOCOL_H                               /* 头文件保护 */
#define OTA_PROTOCOL_H
#include <stdint.h>                                 /* 定长整数类型 */

#define OTA_FRAME_MAGIC        0x5AA5C33CUL          /* 帧头魔数：每帧开头，靠它在字节流里找帧边界 */
#define OTA_FRAME_MAX_PAYLOAD  512U                 /* 单帧 payload 上限 512 字节 */

/* ========== 帧类型（header.type） ========== */
#define OTA_FRAME_TYPE_START   0x01U                /* 起始帧：带镜像元数据，触发擦 APP2 */
#define OTA_FRAME_TYPE_DATA    0x02U                /* 数据帧：搬运固件内容 */
#define OTA_FRAME_TYPE_END     0x03U                /* 结束帧：带整镜像 CRC，触发校验+提交 */
#define OTA_FRAME_TYPE_ACK     0x81U                /* 设备回应：成功 */
#define OTA_FRAME_TYPE_NACK    0x82U                /* 设备回应：失败（status 带原因码） */

/* ========== NACK 原因码（header.status） ========== */
#define OTA_NACK_BAD_STATE  1U                      /* 状态不对（如没 START 就发 DATA） */
#define OTA_NACK_BAD_CRC    2U                      /* payload CRC 校验失败 */
#define OTA_NACK_BAD_SEQ    3U                      /* 帧序号 seq 不连续 */
#define OTA_NACK_BAD_OFFSET 4U                      /* 偏移 offset 与设备已收不符 */
#define OTA_NACK_BAD_LENGTH 5U                      /* 长度非法（0 或 >512 或越界） */
#define OTA_NACK_FLASH      6U                      /* Flash 擦/写失败 */
#define OTA_NACK_FINAL      7U                      /* 最终校验失败（总长或整镜像 CRC 不符） */

#define OTA_IMAGE_TYPE_APP1  1U                     /* V2 头里 image_type 取值：本镜像是给 APP1 的 */

#pragma pack(push, 1)                               /* 关闭结构体对齐：保证设备与 PC 字节布局一致 */

/* ---------- V1 START 载荷：16 字节（简易元数据头） ---------- */
typedef struct {
    uint32_t magic;                                 /* 应 == OTA_FRAME_MAGIC */
    uint32_t app_version;                           /* 固件版本号（信息用） */
    uint32_t app_size;                              /* 整镜像字节数 */
    uint32_t app_crc32;                             /* 整镜像 CRC32 */
} ota_header_t;

/* ---------- V2 START 载荷：40 字节（带目标校验，防烧错） ---------- */
typedef struct {
    uint32_t magic;                                 /* 应 == OTA_FRAME_MAGIC */
    uint32_t app_version;                           /* 固件版本号 */
    uint32_t app_size;                              /* 整镜像字节数 */
    uint32_t app_crc32;                             /* 整镜像 CRC32 */
    uint32_t header_version;                        /* 头版本，应 == 2 */
    uint32_t header_size;                           /* 本结构大小，应 == 40 */
    uint32_t target_addr;                           /* 目标地址，应 == BL_APP1_START_ADDR（防烧错位置） */
    uint32_t image_type;                            /* 镜像类型，应 == OTA_IMAGE_TYPE_APP1（防烧错类型） */
    uint32_t hw_id;                                 /* 硬件 ID（可选，防烧错板子） */
    uint32_t header_crc32;                          /* 前 36 字节的 CRC32，校验本头是否完好 */
} ota_image_header_v2_t;

/* ---------- 帧头：固定 20 字节，每帧最前面 ---------- */
typedef struct {
    uint32_t magic;                                 /* 帧魔数 OTA_FRAME_MAGIC */
    uint8_t  type;                                  /* 帧类型 OTA_FRAME_TYPE_* */
    uint8_t  status;                                /* NACK 时为原因码，其它为 0 */
    uint16_t seq;                                   /* 帧序号：START=0，DATA 从 1 递增，END=末DATA+1 */
    uint32_t offset;                                /* 本帧数据在镜像中的字节偏移；ACK/NACK 时回传设备已收字节数 */
    uint16_t length;                                /* payload 字节数（≤512）；END 为 0 */
    uint16_t reserved;                              /* 对齐预留，凑成整齐 20 字节 */
    uint32_t crc32;                                 /* 仅覆盖 payload；END 帧借此字段携带整镜像 CRC */
} ota_frame_header_t;

#pragma pack(pop)                                   /* 恢复默认对齐 */

#endif /* OTA_PROTOCOL_H */
