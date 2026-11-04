#define ISP_PORT LPC_UART0
#define ISP_TX_PIN 2
#define ISP_RX_PIN 3



// prototypes
void vISPUart_Task(void *pvParameters);
void UART0_IRQHandler(void);
void isp_recv_isr(void);
void isp_xmit_isr(void);
//void decode_gprs_command(void);

