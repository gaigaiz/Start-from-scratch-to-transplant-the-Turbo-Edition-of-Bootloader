/*
 * ota_uart_app.c —— APP_min 侧 OTA 集成层（DMA 增量喂数 + 周期解析）
 *
 * 独立源码，放 GD32_HAL\BL_APP\，只加进 APP_min 工程，CubeMX 重生成不碰。
 * 依赖 CubeMX 在 APP_min 里生成的 huart3 / hdma_usart3_rx（见“准备 C”）。
 */
#include "usart.h"            /* CubeMX: extern UART_HandleTypeDef huart3; */
#include "ota_uart.h"
#include "ota_uart_app.h"

#define OTA_UART_RXBUF_SIZE  2048U
static uint8_t  s_rxbuffer[OTA_UART_RXBUF_SIZE];
static uint32_t s_old_pos = 0;

/*
 * Robust RX启动：清掉上电/连线时的脏 ORE，启动循环 DMA，然后【关掉
 * HAL_UART_Receive_DMA 偷偷打开的 PE/ERR 错误中断】。
 *
 * 为什么必须关：500000 波特率下，BSP_Flash_Program 写一块会阻塞几 ms，
 * 加上连线噪声，USART3 极易 ORE 溢出。HAL 的错误中断路径会 abort 循环
 * DMA（UART_EndRxTransfer/DMAStop），DMA 计数器从此冻结 → 设备永久收不到
 * 数据（主循环还在跑，所以横幅照打，但 OTA 全聋）。关掉这两个中断后，
 * HAL 永不 abort；ORE 时 DMA 继续读 DR 自动清错，最多丢 1 字节，交由帧
 * payload CRC + 上位机重传兜底。这就是参考工程(GD32 原生直配 DMA)不犯
 * 此病的本质原因。
 */
void ota_uart_start(void)
{
    ota_reset_state();
    s_old_pos = 0;
    __HAL_UART_CLEAR_OREFLAG(&huart3);                       /* 读 SR+DR 清掉脏 ORE */
    HAL_UART_Receive_DMA(&huart3, s_rxbuffer, OTA_UART_RXBUF_SIZE);
    __HAL_UART_DISABLE_IT(&huart3, UART_IT_PE);              /* 别让 HAL 因错误 abort DMA */
    __HAL_UART_DISABLE_IT(&huart3, UART_IT_ERR);
}

static void ota_uart_dma_poll(void)
{
    uint32_t pos;

    /* 兜底：万一 ORE 仍卡住接收，主动清；万一 DMA 被 abort 过，重启它 */
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(&huart3);
    }
    if (huart3.RxState == HAL_UART_STATE_READY) {            /* HAL 把接收停了 → 拉起来 */
        __HAL_UART_CLEAR_OREFLAG(&huart3);
        HAL_UART_Receive_DMA(&huart3, s_rxbuffer, OTA_UART_RXBUF_SIZE);
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_PE);
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_ERR);
        s_old_pos = 0;
    }

    pos = OTA_UART_RXBUF_SIZE - __HAL_DMA_GET_COUNTER(huart3.hdmarx);
    if (pos == s_old_pos) return;
    if (pos > s_old_pos) {
        ota_feed(&s_rxbuffer[s_old_pos], pos - s_old_pos);
    } else {                                                  /* 环形回绕：先尾段后头段 */
        ota_feed(&s_rxbuffer[s_old_pos], OTA_UART_RXBUF_SIZE - s_old_pos);
        if (pos > 0) ota_feed(s_rxbuffer, pos);
    }
    s_old_pos = pos;
}

/* 周期任务（建议每 5ms 调一次）：喂 DMA 增量 + 解析整帧 */
void ota_poll_task(void)
{
    ota_uart_dma_poll();
    ota_uart_task();
}
