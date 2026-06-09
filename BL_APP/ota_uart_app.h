#ifndef OTA_UART_APP_H
#define OTA_UART_APP_H

/* APP_min 集成层：把 CubeMX 的 huart3/循环DMA 接到 ota_uart.c 状态机上。
   开机调一次 ota_uart_start()，主循环每 ~5ms 调一次 ota_poll_task()。 */

void ota_uart_start(void);   /* 复位状态机 + 启动 USART3 循环 DMA 接收 */
void ota_poll_task(void);    /* 周期：喂 DMA 增量 + 解析整帧并处理 */

#endif /* OTA_UART_APP_H */
