#include "ota_ringbuffer.h"

/* 本设计里生产者和消费者都在前台 uart_task() 里顺序执行（不在中断里），单线程，无需原子保护。 */

void rb_init(ringbuf_t *rb, uint8_t *mem, uint32_t size)
{
    rb->buf = mem; rb->size = size;
    rb->head = rb->tail = rb->count = 0;
}

uint32_t rb_count(const ringbuf_t *rb) { return rb->count; }
uint32_t rb_space(const ringbuf_t *rb) { return rb->size - rb->count; }

uint32_t rb_write(ringbuf_t *rb, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (rb->count >= rb->size) break;       /* 满，丢弃多余（流控负责防溢出） */
        rb->buf[rb->head] = data[i];
        rb->head = (rb->head + 1) % rb->size;
        rb->count++;
    }
    return i;
}

uint32_t rb_peek(const ringbuf_t *rb, uint8_t *out, uint32_t len)
{
    uint32_t i, t = rb->tail;
    if (len > rb->count) len = rb->count;
    for (i = 0; i < len; i++) { out[i] = rb->buf[t]; t = (t + 1) % rb->size; }
    return len;
}

uint32_t rb_read(ringbuf_t *rb, uint8_t *out, uint32_t len)
{
    uint32_t i;
    if (len > rb->count) len = rb->count;
    for (i = 0; i < len; i++) {
        out[i] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % rb->size;
        rb->count--;
    }
    return len;
}

void rb_drop(ringbuf_t *rb, uint32_t len)
{
    if (len > rb->count) len = rb->count;
    rb->tail = (rb->tail + len) % rb->size;
    rb->count -= len;
}
