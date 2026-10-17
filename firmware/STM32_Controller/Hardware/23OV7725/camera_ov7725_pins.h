/**
 * @file camera_ov7725_pins.h
 * 用途：当前扩展板 OV7725 FIFO 接线的唯一驱动映射。
 * 使用：摄像头驱动包含此文件；改板时集中改 GPIO/时钟定义，再检查其他外设冲突。
 * 时序与边界：F103 示例仅用于理解时序，不能照搬 PC0~PC7；本板 PC3 给麦克风、PC6/7 给 ESP。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef CAMERA_OV7725_PINS_H
#define CAMERA_OV7725_PINS_H

/*
 * OV7725 + FIFO candidate mapping from the project pin plan.
 * The sensor board must be wired by signal name. This is not the F103
 * example's PC0..PC7 mapping: the F407 data bus is intentionally scattered.
 */
#include "stm32f4xx.h"

#define CAM_D0_PORT GPIOC
#define CAM_D0_PIN GPIO_Pin_0
#define CAM_D1_PORT GPIOC
#define CAM_D1_PIN GPIO_Pin_1
#define CAM_D2_PORT GPIOC
#define CAM_D2_PIN GPIO_Pin_2
#define CAM_D3_PORT GPIOC
#define CAM_D3_PIN GPIO_Pin_4
#define CAM_D4_PORT GPIOE
#define CAM_D4_PIN GPIO_Pin_1
#define CAM_D5_PORT GPIOE
#define CAM_D5_PIN GPIO_Pin_5
#define CAM_D6_PORT GPIOE
#define CAM_D6_PIN GPIO_Pin_6
#define CAM_D7_PORT GPIOD
#define CAM_D7_PIN GPIO_Pin_3

#define CAM_SCL_PORT GPIOB
#define CAM_SCL_PIN GPIO_Pin_8
#define CAM_SDA_PORT GPIOB
#define CAM_SDA_PIN GPIO_Pin_11

#define CAM_RCLK_PORT GPIOD
#define CAM_RCLK_PIN GPIO_Pin_6
#define CAM_WRST_PORT GPIOD
#define CAM_WRST_PIN GPIO_Pin_12
#define CAM_WEN_PORT GPIOD
#define CAM_WEN_PIN GPIO_Pin_11
#define CAM_RRST_PORT GPIOA
#define CAM_RRST_PIN GPIO_Pin_1
#define CAM_OE_PORT GPIOA
#define CAM_OE_PIN GPIO_Pin_5
#define CAM_VSYNC_PORT GPIOA
#define CAM_VSYNC_PIN GPIO_Pin_8

/* The candidate map deliberately does not assign XCLK/PCLK/HREF. */
#define CAM_HAS_EXTERNAL_XCLK 0u

#endif
