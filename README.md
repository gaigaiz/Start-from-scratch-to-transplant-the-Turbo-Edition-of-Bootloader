STM32F407 Bootloader & UART OTA 远程升级工程
项目简介
本项目是一套从零移植的 STM32 Bootloader + 串口 OTA 固件升级解决方案，面向嵌入式开发者入门学习 IAP（在应用编程）、Bootloader 跳转、远程固件升级等核心技术。项目基于 STM32F407VET6 主控芯片与 HAL 库开发，完整实现 Bootloader 引导、APP 应用运行、串口 DMA 固件接收、CRC32 校验、Python 上位机一键升级 全流程，代码结构清晰，移植难度低，可直接用于学习、毕业设计及产品原型开发。
一、硬件与软件环境
1. 硬件平台
主控：STM32F407VET6（嘉立创天空星开发板）
通信外设：
USART3：专用 OTA 升级串口，波特率 500000 Bit/s，开启 DMA 接收
USART1：日志输出、状态监测串口
辅助工具：USB-TTL 串口模块（推荐用于固件升级）
2. 软件环境
配置工具：STM32CubeMX（外设初始化、时钟配置）
编译环境：Keil MDK-ARM（AC5/AC6 均可，最低支持 V5.32）
代码框架：STM32 HAL 库
上位机：Python（运行 ota_gui.py 可视化升级工具，需提前安装依赖库）
二、整体架构与目录说明
1. 全局文件夹结构
整个工程统一划分 5 个核心目录，职责分离，便于维护：
表格
目录名	功能说明
BL	Bootloader 引导程序主工程，占用 Flash 前 48KB
APP	业务应用程序主工程，独立分区运行
BL_APP	Bootloader 与 APP 共用接口文件
BL_min	Bootloader 最小驱动 / 基础组件
Common	公共组件：Flash 驱动、CRC32 校验、环形缓冲区等通用代码
2. Flash 分区规划（核心）
Bootloader 分区：0x08000000 起始，大小 0xC000（48KB）
APP 应用分区：0x08014000 起始，独立分区运行，与引导程序完全隔离
RAM：使用 STM32F407 片内 192KB 内存，部分 Flash 代码配置在 RAM 中运行
三、核心功能特性
Bootloader 引导逻辑
上电自动检测 Flash 内是否存在合法 APP 程序；
无有效 APP 时，串口打印日志并停机；检测正常则安全跳转到 APP 执行；
自定义分散加载文件（.sct），精准划分 Flash/RAM 空间。
APP 应用程序
启动时自动重定向中断向量表 SCB->VTOR，杜绝跳转后中断跑飞问题；
双串口分工：USART1 循环打印版本状态，USART3 + DMA 持续监听升级固件；
内置版本标记，可快速区分新旧固件版本。
OTA 固件升级
基于 DMA 实现串口大数据接收，提升传输稳定性；
集成 CRC32 校验算法，校验固件完整性，防止传输出错；
搭配 Python 可视化上位机，支持选择串口、固件文件、分块大小，一键完成升级；
升级过程实时展示传输进度、ACK/NACK 状态、重传 / 超时统计。
通用组件
封装 Flash 读写驱动、环形缓冲区、CRC32 校验，可复用至其他项目。
四、编译与运行全流程
步骤 1：工程初始化（STM32CubeMX）
选择芯片 STM32F407VET6，配置系统时钟为外部高速晶振（HSE）；
开启调试接口（Serial Wire）、USART1、USART3，并为 USART3 配置 DMA 接收；
配置外设参数：USART3 波特率 500000，关闭硬件流控；
生成 MDK-ARM 工程，工程路径请勿包含中文。
步骤 2：Keil 工程配置（BL 引导程序）
在工程分组中添加 Common、BL_min 等目录下的驱动、算法文件；
进入编译器配置，添加所有头文件包含路径；
进入 Linker 配置，取消默认内存布局勾选，加载自定义 BL.sct 分散加载文件；
在 main.c 中引入 bl_core.h，硬件初始化完成后调用 bootloader_run()；
编译并通过 ST-Link 下载 Bootloader 程序至单片机。
步骤 3：Keil 工程配置（APP 应用程序）
同上述步骤，创建 APP 工程，添加公共文件并配置头文件路径；
加载自定义 APP.sct 分散加载文件，匹配 APP 起始地址；
main.c 首行添加中断向量表重定向代码 SCB->VTOR = 0x08014000UL；
初始化 DMA、双串口，启动 OTA 监听任务与状态打印；
首次编译下载基础 APP 程序，串口查看版本日志。
步骤 4：远程 OTA 升级测试
修改 APP 内版本标记（如 APP v1.0 → APP v2.0），仅编译不下载；
从 Keil 工程 MDK-ARM 目录中提取 APP.hex 固件，可重命名便于区分；
运行 ota_gui.py 上位机脚本，配置：OTA 串口、日志串口、波特率、固件路径；
点击「开始升级」，等待传输 100%，串口查看 Bootloader 跳转日志与新版 APP 运行日志。
五、关键配置文件说明
BL.sct（Bootloader 分散加载文件）
限定 Bootloader 占用 48KB Flash，指定部分 Flash 代码在 RAM 中运行，适配 STM32F407 内存布局。
APP.sct（应用程序分散加载文件）
指定 APP 程序起始地址 0x08014000，与 Bootloader 分区隔离，保证程序独立运行。
公共文件
bsp_flash.c：片内 Flash 读写驱动；
crc32.c：固件校验算法；
环形缓冲区：串口数据缓存，提升传输稳定性。
六、注意事项 & 常见问题
路径规范：所有工程、固件、脚本存放路径严禁使用中文、中文标点、特殊字符，否则会导致编译失败、固件烧录异常。
工具要求：Keil 版本不低于 MDK-ARM V5.32；运行 Python 上位机需提前安装对应依赖库。
