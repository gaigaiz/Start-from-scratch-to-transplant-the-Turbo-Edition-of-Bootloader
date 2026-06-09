#include "ota_uart.h"
#include "ota_protocol.h"
#include "bl_partition.h"
#include "bl_param.h"
#include "crc32.h"
#include "bsp_flash.h"
#include "ota_ringbuffer.h"
#include "stm32f4xx.h"
#include "usart.h"            /* extern UART_HandleTypeDef huart3; */
#include <string.h>

/* ---------- 环形缓冲 ---------- */
static uint8_t   s_rb_mem[OTA_RING_BUFFER_SIZE];
static ringbuf_t s_rb;
static int       s_paused = 0;

/* ---------- 会话状态 ---------- */
typedef enum { OTA_IDLE = 0, OTA_RECEIVING } ota_state_t;
static ota_state_t s_state;
static uint32_t s_app_size;     /* START 声明的整镜像大小 */
static uint32_t s_app_crc;      /* START 声明的整镜像 CRC */
static uint32_t s_recv;         /* 已写入 APP2 的字节数 */
static uint16_t s_exp_seq;      /* 期望的下一个 DATA seq */
static uint32_t s_roll;         /* 滚动 CRC 中间值（未 final） */

void ota_reset_state(void)
{
    rb_init(&s_rb, s_rb_mem, OTA_RING_BUFFER_SIZE);
    s_state = OTA_IDLE;
    s_app_size = s_app_crc = s_recv = s_roll = 0;
    s_exp_seq = 0;
    s_paused = 0;
}

/* ---------- 喂数 + 流控 ---------- */
void ota_feed(const uint8_t *data, uint32_t len)
{
    rb_write(&s_rb, data, len);
}

static void flow_control(void)
{
    uint32_t lvl = rb_count(&s_rb) * 100U / OTA_RING_BUFFER_SIZE;
    if (!s_paused && lvl >= 80U) {
        HAL_UART_Transmit(&huart3, (uint8_t *)"PAUSE\r\n", 7, 50);
        s_paused = 1;
    } else if (s_paused && lvl <= 20U) {
        HAL_UART_Transmit(&huart3, (uint8_t *)"RESUME\r\n", 8, 50);
        s_paused = 0;
    }
}

/* ---------- 回包 ---------- */
static void send_reply(uint8_t type, uint8_t status, uint16_t seq)
{
    ota_frame_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic  = OTA_FRAME_MAGIC;
    h.type   = type;
    h.status = status;
    h.seq    = seq;
    h.offset = s_recv;          /* 让 PC 知道设备当前进度，可精确续传 */
    h.length = 0;
    h.crc32  = 0;
    HAL_UART_Transmit(&huart3, (uint8_t *)&h, sizeof(h), 100);
}
#define ACK(seq)         send_reply(OTA_FRAME_TYPE_ACK,  0,        (seq))
#define NACK(seq,reason) send_reply(OTA_FRAME_TYPE_NACK, (reason), (seq))

/* ---------- START ---------- */
static void handle_start(const uint8_t *pl, uint16_t len, uint16_t seq)
{
    uint32_t app_size = 0, app_crc = 0;

    if (len == sizeof(ota_header_t)) {                       /* V1 */
        ota_header_t v1;
        memcpy(&v1, pl, sizeof(v1));
        if (v1.magic != OTA_FRAME_MAGIC) { NACK(seq, OTA_NACK_BAD_STATE); return; }
        app_size = v1.app_size; app_crc = v1.app_crc32;
    } else if (len == sizeof(ota_image_header_v2_t)) {       /* V2 */
        ota_image_header_v2_t v2;
        memcpy(&v2, pl, sizeof(v2));
        if (v2.magic != OTA_FRAME_MAGIC)                       { NACK(seq, OTA_NACK_BAD_STATE); return; }
        if (v2.header_version != 2U || v2.header_size != sizeof(v2)) { NACK(seq, OTA_NACK_BAD_STATE); return; }
        if (v2.target_addr != BL_APP1_START_ADDR)              { NACK(seq, OTA_NACK_BAD_STATE); return; }
        if (v2.image_type != OTA_IMAGE_TYPE_APP1)              { NACK(seq, OTA_NACK_BAD_STATE); return; }
        if (crc32_calc(pl, (uint32_t)offsetof(ota_image_header_v2_t, header_crc32)) != v2.header_crc32) {
            NACK(seq, OTA_NACK_BAD_CRC); return;
        }
        app_size = v2.app_size; app_crc = v2.app_crc32;
    } else {
        NACK(seq, OTA_NACK_BAD_LENGTH); return;
    }

    if (app_size == 0 || app_size > BL_APP2_SIZE) { NACK(seq, OTA_NACK_BAD_STATE); return; }

    /* 会话中收到一致的重复 START → 视为重试，回当前进度 */
    if (s_state == OTA_RECEIVING && app_size == s_app_size && app_crc == s_app_crc) {
        ACK(seq); return;
    }

    if (!BSP_Flash_ErasePages(BL_APP2_START_ADDR,
            (app_size + BL_FLASH_PAGE_SIZE - 1UL) & ~(BL_FLASH_PAGE_SIZE - 1UL))) {
        NACK(seq, OTA_NACK_FLASH); return;
    }

    s_app_size = app_size;
    s_app_crc  = app_crc;
    s_recv     = 0;
    s_exp_seq  = 1;
    s_roll     = CRC32_INIT;
    s_state    = OTA_RECEIVING;
    ACK(seq);
}

/* ---------- DATA ---------- */
static void handle_data(const uint8_t *pl, uint16_t len, uint16_t seq, uint32_t offset)
{
    if (s_state != OTA_RECEIVING) { NACK(seq, OTA_NACK_BAD_STATE); return; }

    if (seq < s_exp_seq) { ACK(seq); return; }                 /* ACK 丢失重传：已写过，回 ACK */
    if (seq != s_exp_seq) { NACK(seq, OTA_NACK_BAD_SEQ); return; }
    if (offset != s_recv) { NACK(seq, OTA_NACK_BAD_OFFSET); return; }
    if (len == 0 || (s_recv + len) > s_app_size) { NACK(seq, OTA_NACK_BAD_LENGTH); return; }

    if (!BSP_Flash_Program(BL_APP2_START_ADDR + s_recv, pl, len)) {
        NACK(seq, OTA_NACK_FLASH); return;
    }
    s_roll = crc32_update(s_roll, pl, len);
    s_recv   += len;
    s_exp_seq++;
    ACK(seq);
}

/* ---------- 提交参数页：置 PENDING（APP 只写 PENDING） ---------- */
static bool ota_commit_pending(void)
{
    static uint8_t cache[BL_PARAM_PAGE_SIZE];
    bl_param_t mp;

    memcpy(cache, (const void *)BL_PARAM_PAGE_ADDR, BL_PARAM_PAGE_SIZE);
    memcpy(&mp,   (const void *)BL_PARAM_MAIN_ADDR, sizeof(mp));

    if (mp.magic != BL_PARAM_MAGIC || mp.tail_magic != BL_PARAM_TAIL_MAGIC
        || mp.version != BL_PARAM_VERSION) {
        memset(&mp, 0, sizeof(mp));
        mp.magic = BL_PARAM_MAGIC; mp.version = BL_PARAM_VERSION;
        mp.tail_magic = BL_PARAM_TAIL_MAGIC;
    }
    mp.app_size    = s_app_size;
    mp.app_crc32   = s_app_crc;
    mp.app1_addr   = BL_APP1_START_ADDR;
    mp.app2_addr   = BL_APP2_START_ADDR;
    mp.update_flag = BL_UPDATE_FLAG_PENDING;
    mp.last_error  = BL_ERR_NONE;
    mp.param_crc32 = crc32_calc((const uint8_t *)&mp,
                                (uint32_t)offsetof(bl_param_t, param_crc32));
    memcpy(&cache[BL_PARAM_MAIN_ADDR   - BL_PARAM_PAGE_ADDR], &mp, sizeof(mp));
    memcpy(&cache[BL_PARAM_BACKUP_ADDR - BL_PARAM_PAGE_ADDR], &mp, sizeof(mp));

    if (!BSP_Flash_ErasePages(BL_PARAM_PAGE_ADDR, BL_PARAM_PAGE_SIZE)) return false;
    return BSP_Flash_Program(BL_PARAM_PAGE_ADDR, cache, BL_PARAM_PAGE_SIZE);
}

/* ---------- END ---------- */
static void handle_end(uint16_t seq, uint32_t offset, uint32_t whole_crc)
{
    uint32_t final;
    if (s_state != OTA_RECEIVING)   { NACK(seq, OTA_NACK_BAD_STATE);  return; }
    if (offset != s_recv)           { NACK(seq, OTA_NACK_BAD_OFFSET); return; }
    if (seq != s_exp_seq)           { NACK(seq, OTA_NACK_BAD_SEQ);    return; }

    final = crc32_final(s_roll);
    if (s_recv != s_app_size || final != s_app_crc || final != whole_crc) {
        NACK(seq, OTA_NACK_FINAL);
        s_state = OTA_IDLE;
        return;
    }
    if (!ota_commit_pending()) { NACK(seq, OTA_NACK_FLASH); return; }

    ACK(seq);
    HAL_Delay(20);                  /* 让 ACK 发完 */
    __disable_irq();
    NVIC_SystemReset();             /* 复位回 BootLoader */
}

/* ---------- 解析整帧 ---------- */
void ota_uart_task(void)
{
    static uint8_t payload[OTA_FRAME_MAX_PAYLOAD];
    ota_frame_header_t h;

    flow_control();

    while (rb_count(&s_rb) >= sizeof(ota_frame_header_t)) {
        rb_peek(&s_rb, (uint8_t *)&h, sizeof(h));

        if (h.magic != OTA_FRAME_MAGIC) { rb_drop(&s_rb, 1); continue; }   /* 重同步 */

        if (h.length > OTA_FRAME_MAX_PAYLOAD) {                            /* 头不可信，丢头 */
            rb_drop(&s_rb, sizeof(h));
            continue;
        }
        if (rb_count(&s_rb) < (uint32_t)sizeof(h) + h.length) return;      /* payload 未到齐 */

        rb_drop(&s_rb, sizeof(h));
        if (h.length) rb_read(&s_rb, payload, h.length);

        /* START/DATA 校验 payload CRC（END length=0，crc32 字段是整镜像 CRC） */
        if (h.type == OTA_FRAME_TYPE_START || h.type == OTA_FRAME_TYPE_DATA) {
            if (crc32_calc(payload, h.length) != h.crc32) {
                NACK(h.seq, OTA_NACK_BAD_CRC);
                continue;
            }
        }

        switch (h.type) {
        case OTA_FRAME_TYPE_START: handle_start(payload, h.length, h.seq);          break;
        case OTA_FRAME_TYPE_DATA:  handle_data (payload, h.length, h.seq, h.offset); break;
        case OTA_FRAME_TYPE_END:   handle_end  (h.seq, h.offset, h.crc32);          break;
        default:                   NACK(h.seq, OTA_NACK_BAD_STATE);                 break;
        }
    }
}
