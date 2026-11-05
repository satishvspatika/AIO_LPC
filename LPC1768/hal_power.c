#include "hal_power.h"
#include "lpc17xx_uart.h"
#include "lpc17xx_pinsel.h"
#include <string.h>

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

    /* Ensure 5V LCD power rail is active on LPC845 coprocessor */
    hal_power_lcd_on();
}

static void send_intermcu_cmd(const char *cmd)
{
    UART_Send(LPC_UART3, (uint8_t *)cmd, strlen(cmd), BLOCKING);
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

