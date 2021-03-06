/*******************************************************************************
*                                 AMetal
*                       ----------------------------
*                       innovating embedded platform
*
* Copyright (c) 2001-2018 Guangzhou ZHIYUAN Electronics Co., Ltd.
* All rights reserved.
*
* Contact information:
* web site:    http://www.zlg.cn/
*******************************************************************************/

/**
 * \file
 * \brief bootloader 例程，本demo是作为bootloader。
 *
 * - 操作步骤：
 *   参考AMmetal-AM845-Core-bootloader操作手册
 *
 *
 * \internal
 * \par Modification History
 * - 1.00 19-03-25  yrh, first implementation
 * \endinternal
 */

/**
 * \addtogroup demo_if_am845_core_bootloader
 * \copydoc demo_am845_core_bootloader.c
 */

#include "ametal.h"
#include "am_board.h"
#include "am_vdebug.h"
#include "am_lpc84x.h"
#include "lpc84x_pin.h"
#include "am_boot.h"
#include "am_uart.h"
#include "am_softimer.h"
#include "am_boot_firmware.h"
#include "am_lpc84x_inst_init.h"
#include "am_bootconf_lpc845.h"
#include "am_int.h"
#include "am_boot_config.h"

/** \brief 串口数据接收缓存大小 */
#define UART_CALLBACK_BUF_SIZE      4096

#define RECEIVE_BUF_SIZE            64

/** \brief 启动延时时间(秒)*/
#define BOOT_WAITE_TIME             5

/** \brief 串口波特率 */
#define UART_BAUD                   9600

/**
 * \name bootloader状态
 * @{
 */
/** \brief 启动延时等待状态 */
#define BOOT_WAIT_STATE             0

/** \brief bootloader启动状态 */
#define BOOT_STARTUP_STATE          1

/** \brief 应用启动状态 */
#define APP_STARTUP_STATE           2

/** \brief 应用固件接收状态 */
#define APP_RECEIVE_STATE           3
/** @} */

static volatile uint32_t write_offset = 0, read_offset = 0;

/** \brief bootloader启动延时软件定时器结构体 */
static am_softimer_t timeout_timer;


/** \brief 一些常用的标志和初始值 */
static volatile uint8_t second_timeout = 0, firmware_timeout = 0, timer = BOOT_WAITE_TIME, boot_state = BOOT_WAIT_STATE;


/** \brief 串口中断回调数据接收缓存 */
static uint8_t  uart_callback_buffer[UART_CALLBACK_BUF_SIZE] = {0};

static void __delay(void)
{
    volatile int i = 1000000;
    while(i--);
}

/**
 * \brief 串口中断接收回调函数
 */
static void __uart_callback(void *p_arg, char inchar)
{
    if(boot_state == BOOT_WAIT_STATE) {
        timer = BOOT_WAITE_TIME;
        boot_state = BOOT_STARTUP_STATE;
    }

    if(boot_state == APP_RECEIVE_STATE) {
        uart_callback_buffer[write_offset++] = (uint8_t)inchar;
        write_offset &= UART_CALLBACK_BUF_SIZE - 1;
    }
}
/**
 * \brief 定时器回调函数
 */
static void __timer_handle(void *p_arg)
{
    if(boot_state == BOOT_WAIT_STATE) {
        if(timer != 0) {
            timer--;
            if(timer == 0) {
                am_softimer_stop(&timeout_timer);
                timer = BOOT_WAITE_TIME;
                boot_state = APP_STARTUP_STATE;
                return;
            }
        second_timeout = 1;
      }
    }
    if (boot_state == APP_RECEIVE_STATE) {
        firmware_timeout = 1;
    }
}

/**
 * \brief  从串口接收缓冲区读数据
 */
static int __read_data(uint8_t *p_buffer, uint32_t byte_count)
{
    uint32_t current_bytes_read = 0;
    am_softimer_start(&timeout_timer, 15000);
    while (current_bytes_read != byte_count) {
        if(firmware_timeout == 1) {
            firmware_timeout = 0;
            am_softimer_stop(&timeout_timer);
            return AM_ERROR;
        }

        if(read_offset != write_offset) {
            p_buffer[current_bytes_read++] = uart_callback_buffer[read_offset++];
            read_offset &= UART_CALLBACK_BUF_SIZE - 1;
        }
    }
    firmware_timeout = 0;
    am_softimer_stop(&timeout_timer);
    return AM_OK;
}

/**
 * \brief bootloader demo入口
 */
void demo_am845_core_bootloader_entry (void)
{
    am_boot_firmware_verify_info_t firmware_head;
    am_boot_firmware_handle_t      firmware_handle;
    int ret = -1, i, count;
    /* bootloader flash初始化  */
    am_boot_flash_handle_t flash_handle = am_lpc845_boot_flash_inst_init();
    /* bootloader 标准接口初始化  */
    am_lpc845_std_boot_inst_init(flash_handle);

    am_uart_handle_t uart_handle = am_lpc84x_usart0_inst_init ();
    am_debug_init(uart_handle, UART_BAUD);

    /* 使能串口中断模式  */
    am_uart_ioctl(uart_handle, AM_UART_MODE_SET, (void *)AM_UART_MODE_INT);
    /* 注册发送回调函数  */
    am_uart_callback_set(uart_handle, AM_UART_CALLBACK_RXCHAR_PUT, __uart_callback, NULL);
    /* 启动延时回调定时器初始化  */
    am_softimer_init(&timeout_timer, __timer_handle, NULL);
    am_kprintf("Device will enter application, if don't input anything after %ds\r\n",timer);
    am_softimer_start(&timeout_timer, 1000);

    /* 延时等待，如果用户有数据发来，就进入bootloader，否者跳转到应用程序 */
    while(boot_state == BOOT_WAIT_STATE) {
        if(second_timeout == 1) {
            am_kprintf("Device will enter application, if don't input anything after %ds\r\n",timer);
            second_timeout = 0;
        }
    }
    am_softimer_stop(&timeout_timer);

#if AM_DOUBLE_BOOT
    /* 用户有数发送过来表示要进入bootloader本地升级，则设置bootloader升级标志为双区无效，重新接受固件  */
    if( boot_state == BOOT_STARTUP_STATE) {
        am_boot_update_flag_set(AM_BOOTLOADER_FLAG_NO);
    }
#endif

    /* 在延时等待期间没有用户输入，跳转到应用程序，如果应用程序没准备好，直接进入bootloader本地升级  */
    if(boot_state == APP_STARTUP_STATE && am_boot_app_is_ready()) {
        __delay();
        am_lpc84x_usart0_inst_deinit(uart_handle);
        am_lpc84x_clk_inst_deinit();
        am_boot_go_application();
    }

    am_kprintf("bootloader : running......\r\n");
    while(1) {
        am_kprintf("bootloader : update init\r\n");
        /* 固件存放标准接口初始化  */
        firmware_handle = am_lpc845_boot_firmware_flash(flash_handle);
        /* 开始固件存放  */
#if AM_DOUBLE_BOOT
        am_boot_firmware_store_start(firmware_handle, 26 * 1024);
#else
        am_boot_firmware_store_start(firmware_handle, 49 * 1024);
#endif
        /* 切换为固件接收状态  */
        boot_state = APP_RECEIVE_STATE;
        am_kprintf("bootloader : firmware transmission is ready\r\n");

        /* 从串口数据缓冲区中读取固件的头部信息  */
        if(AM_OK != __read_data((uint8_t *)&firmware_head, sizeof(firmware_head))) {
            am_kprintf("bootloader : firmware transmission is timeout, bootloader will restart!\r\n");
            continue;
        }

        count = firmware_head.len / RECEIVE_BUF_SIZE;
        uint8_t buf_t[RECEIVE_BUF_SIZE] = {0};
        am_kprintf("bootloader : receiving firmware ...\r\n");
        for(i = 0; i < count; i++) {

            ret = __read_data((uint8_t *)buf_t, sizeof(buf_t));


            if(AM_OK != ret) {
                am_kprintf("bootloader : firmware transmission is timeout, bootloader will restart!\r\n");
                break;
            }
            /* 固件存放   */
            ret = am_boot_firmware_store_bytes(firmware_handle, (uint8_t *)buf_t, sizeof(buf_t));
            if(ret != AM_OK) {
                am_kprintf("bootloader : firmware store is error, bootloader will restart!\r\n");
                break;
            }
        }
        if(AM_OK != ret) {
            continue;
        }

        if (firmware_head.len & (RECEIVE_BUF_SIZE - 1)) {
            if(AM_OK != __read_data((uint8_t *)buf_t, firmware_head.len & (RECEIVE_BUF_SIZE - 1))) {

                am_kprintf("bootloader : firmware transmission is error, bootloader will restart!\r\n");
                continue;
            }

            if(AM_OK != am_boot_firmware_store_bytes (firmware_handle,
                                                     (uint8_t *)buf_t,
                                                     firmware_head.len & (RECEIVE_BUF_SIZE - 1))) {
                am_kprintf("bootloader : firmware store is error, bootloader will restart!\r\n");
                continue;
            }
        }
        /* 固件存放结束  */
        am_boot_firmware_store_final(firmware_handle);
        am_kprintf("bootloader : firmware receive successful\r\n");
        /* 固件校验  */
        ret = am_boot_firmware_verify(firmware_handle, &firmware_head);
        if(ret != AM_OK) {
            am_kprintf("bootloader : firmware verify error, bootloader will restart!\r\n");
            continue;
        } else {
            am_kprintf("bootloader : firmware verify successful\r\n");
        }

#if  AM_DOUBLE_BOOT
        /* 设置 bootloader 的升级标志为应用程序有效  */
        ret = am_boot_update_flag_set(AM_BOOTLOADER_FLAG_APP);
        if(ret != AM_OK) {
            am_kprintf("bootloader : set update flag fail\r\n");
            continue;
        }
#endif

        if(am_boot_app_is_ready()) {
            am_kprintf("bootloader : go to application...\r\n");
            __delay();
            am_lpc84x_usart0_inst_deinit(uart_handle);
            am_lpc84x_clk_inst_deinit();
            /* 跳转到应用程序  */
            if(AM_OK != am_boot_go_application()){
                continue;
            }
        }
    }
}

/* end of file */
