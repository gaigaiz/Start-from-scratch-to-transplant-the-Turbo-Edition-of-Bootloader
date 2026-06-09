#ifndef OTA_UART_H
#define OTA_UART_H
#include <stdint.h>

#define OTA_RING_BUFFER_SIZE  (16U * 1024U)

void ota_reset_state(void);                       /* 上电初始化 */
void ota_feed(const uint8_t *data, uint32_t len); /* DMA 增量喂数入口 */
void ota_uart_task(void);                         /* 周期调用：解析整帧并处理 */

#endif /* OTA_UART_H */
