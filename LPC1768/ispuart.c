#include "ispuart.h"
#include "globals.h"
#include "lpc17xx_uart.h"
#include "lpc17xx_pinsel.h"
#include "lpc17xx_libcfg_default.h"
#include "FreeRTOS.h"


char isp_rx_data[16];
char isp_tx_data[16];

char *isp_xmit_ptr;
int isp_uart_in_count;


void define_isp_uart_pins(LPC_UART_TypeDef* isp_port);
void init_isp_uart(LPC_UART_TypeDef* isp_port);

void UART0_IRQHandler(void)
{
	UART0_StdIntHandler();
}

void define_isp_uart_pins(LPC_UART_TypeDef* isp_port)
{
	PINSEL_CFG_Type isp_config;

	isp_config.OpenDrain = 0;
	isp_config.Pinmode = 0;
	isp_config.Funcnum = 1;
	isp_config.Portnum = 0;
	isp_config.Pinnum = ISP_TX_PIN;
	PINSEL_ConfigPin(&isp_config);
	isp_config.Pinnum = ISP_RX_PIN;
	PINSEL_ConfigPin(&isp_config);

}


void init_isp_uart(LPC_UART_TypeDef* isp_port)
{
	UART_CFG_Type UART_InitStruct;
	UART_FIFO_CFG_Type UART_FIFOInitStruct;
	UART_ConfigStructInit(&UART_InitStruct);
	UART_InitStruct.Baud_rate = 115200;

	UART_Init(isp_port, &UART_InitStruct);
	UART_SetupCbs(isp_port, 0, &isp_recv_isr);
	UART_SetupCbs(isp_port, 1, &isp_xmit_isr);
	UART_FIFOConfigStructInit(&UART_FIFOInitStruct);
	UART_FIFOConfig(isp_port, &UART_FIFOInitStruct);
	UART_IntConfig(isp_port, UART_INTCFG_RBR, ENABLE);
	UART_IntConfig(isp_port, UART_INTCFG_THRE, ENABLE);
	UART_TxCmd(isp_port,ENABLE);
	NVIC_EnableIRQ(UART0_IRQn);
}


void isp_xmit_isr(void)
{
	if(*isp_xmit_ptr){
		UART_SendData((LPC_UART_TypeDef*) ISP_PORT, *isp_xmit_ptr);
		isp_xmit_ptr++;
	}
}

void isp_recv_isr(void){
	isp_rx_data[isp_uart_in_count] = UART_ReceiveData((LPC_UART_TypeDef*) ISP_PORT);
	isp_uart_in_count = (isp_uart_in_count+1)%(sizeof(isp_rx_data));

}


void vISPUart_Task(void *pvParameters){


	vTaskDelay(500 / portTICK_RATE_MS);

	define_isp_uart_pins((LPC_UART_TypeDef*) ISP_PORT);
	vTaskDelay(200 / portTICK_RATE_MS);

	init_isp_uart((LPC_UART_TypeDef*) ISP_PORT);
	vTaskDelay(200 / portTICK_RATE_MS);

	for(;;){

		vTaskDelay(500 / portTICK_RATE_MS);

		sprintf(isp_tx_data, "$D OK;");

		isp_xmit_ptr = isp_tx_data;
		UART_SendData((LPC_UART_TypeDef*) ISP_PORT, *isp_xmit_ptr++);
		vTaskDelay(10 / portTICK_RATE_MS);


	}
}

