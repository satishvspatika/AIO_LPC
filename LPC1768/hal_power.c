#include "hal_power.h"
#include "FreeRTOS.h"
#include "task.h"
#include "fw_app.h"
#include "lpc17xx_uart.h"
#include "lpc17xx_pinsel.h"
#include <string.h>
#include <stdio.h>

static volatile bool s_is_5v_on = true;
static volatile bool s_is_modem_on = false;

void hal_power_init(void)
{
    PINSEL_CFG_Type pin_cfg;

    /* UART3 TXD3 on P4.28 */
    pin_cfg.Portnum = 4;
    pin_cfg.Pinnum = 28;
    pin_cfg.Funcnum = 3;
    pin_cfg.Pinmode = 0;
    pin_cfg.OpenDrain = 0;
    PINSEL_ConfigPin(&pin_cfg);

    /* UART3 RXD3 on P4.29 */
    pin_cfg.Portnum = 4;
    pin_cfg.Pinnum = 29;
    pin_cfg.Funcnum = 3;
    PINSEL_ConfigPin(&pin_cfg);

    UART_CFG_Type uart_cfg;
    UART_ConfigStructInit(&uart_cfg);
    uart_cfg.Baud_rate = 9600;
    UART_Init(LPC_UART3, &uart_cfg);

    UART_FIFO_CFG_Type fifo_cfg;
    UART_FIFOConfigStructInit(&fifo_cfg);
    UART_FIFOConfig(LPC_UART3, &fifo_cfg);

    UART_TxCmd(LPC_UART3, ENABLE);

    /* Enable UART3 RX Interrupt for reliable non-blocking inter-MCU frame reception */
    UART_IntConfig(LPC_UART3, UART_INTCFG_RBR, ENABLE);
    NVIC_SetPriority(UART3_IRQn, 6);
    NVIC_EnableIRQ(UART3_IRQn);

    /* Don't touch the LCD rail here: its state in the LPC845's first status frame is
     * how the UI tells a scheduled wake (off) from an operator wake (on). */
}

static void send_intermcu_cmd(const char *cmd)
{
    /* Sent whole: UI, modem and main tasks all send, and two frames interleaved byte by byte are
     * both lost on the CTRL ($S, $f7, $G; review, v6.9.51). Only task switches are held off, not
     * interrupts, so frames from the CTRL still come in. About 20 ms at 9600 baud. */
    vTaskSuspendAll();
    UART_Send(LPC_UART3, (uint8_t *)cmd, strlen(cmd), BLOCKING);
    xTaskResumeAll();
}

void hal_power_lcd_on(void)
{
    send_intermcu_cmd("$n4;\012");
    s_is_5v_on = true;
}

void hal_power_lcd_off(void)
{
    send_intermcu_cmd("$f4;\012");
    s_is_5v_on = false;
}

void hal_power_modem_on(void)
{
    send_intermcu_cmd("$n2;\012");
    s_is_modem_on = true;
}

void hal_power_modem_off(void)
{
    send_intermcu_cmd("$f2;\012");
    s_is_modem_on = false;
}

void hal_power_request_record(const char *timestamp14)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "$G%.14s;\012", timestamp14);
    send_intermcu_cmd(cmd);
}

void hal_power_calib_start(void)
{
    send_intermcu_cmd("$C;\012");
}

void hal_power_calib_stop(void)
{
    send_intermcu_cmd("$c;\012");
}

void hal_power_rain_reset(void)
{
    send_intermcu_cmd("$R;\012");
}

void hal_power_rain_restore(unsigned long tips)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "$T%lu;\012", tips);
    send_intermcu_cmd(cmd);
}

void hal_power_send_frame(const char *frame)
{
    send_intermcu_cmd(frame);
}

void hal_power_heartbeat(void)
{
    wd_checkin();
    send_intermcu_cmd("$H;\012");
}

void hal_power_sleep_system(void)
{
    send_intermcu_cmd("$f7;\012");
    s_is_5v_on = false;
    s_is_modem_on = false;
}

void hal_power_sync_rtc(const char *timestamp14)
{
    if (!timestamp14 || strlen(timestamp14) < 14) return;
    char sync_cmd[32];
    sync_cmd[0] = '$';
    sync_cmd[1] = 'S';
    strncpy(&sync_cmd[2], timestamp14, 14);
    sync_cmd[16] = ';';
    sync_cmd[17] = '\n';
    sync_cmd[18] = '\0';
    send_intermcu_cmd(sync_cmd);
}

bool hal_power_is_5v_on(void)
{
    return s_is_5v_on;
}

bool hal_power_is_modem_on(void)
{
    return s_is_modem_on;
}

