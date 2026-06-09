#ifndef OTA_RINGBUFFER_H
#define OTA_RINGBUFFER_H
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t size;
    uint32_t head;     /* 写 */
    uint32_t tail;     /* 读 */
    uint32_t count;
} ringbuf_t;

void     rb_init (ringbuf_t *rb, uint8_t *mem, uint32_t size);
uint32_t rb_count(const ringbuf_t *rb);
uint32_t rb_space(const ringbuf_t *rb);
uint32_t rb_write(ringbuf_t *rb, const uint8_t *data, uint32_t len);  /* 满则丢多余，返回实际写入 */
uint32_t rb_peek (const ringbuf_t *rb, uint8_t *out, uint32_t len);   /* 不移动读指针 */
uint32_t rb_read (ringbuf_t *rb, uint8_t *out, uint32_t len);         /* 取出并移动读指针 */
void     rb_drop (ringbuf_t *rb, uint32_t len);                       /* 丢弃 len 字节 */

#endif /* OTA_RINGBUFFER_H */
