
//#include "board.h"
#include "fsl_gpio.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "stdio.h"
#include <string.h>
#include "globals.h"
#include "fsl_usart.h"
#include "fsl_common.h"
#include "fsl_iocon.h"
#include "fsl_swm.h"
#include "pin_mux.h"
#include "fsl_i2c.h"
#include "fsl_adc.h"
#include "fsl_clock.h"
#include "fsl_power.h"
#include <stdbool.h>
#include "LPC845.h"
#include "fsl_sctimer.h"
#include "fw845_app.h"
#include "fw845_recv.h"


uint32_t deviceAddress = 0x01U;

#define SCTIMER_CLK_FREQ CLOCK_GetFreq(kCLOCK_Fro)


gpio_pin_config_t gpio_output_config = {
		kGPIO_DigitalOutput, 0,
};

gpio_pin_config_t gpio_input_config = {
		kGPIO_DigitalInput,
};

//I2CM_XFER_T  XITC_I2C_IDT;
i2c_master_transfer_t XITC_I2C_IDT;
i2c_master_transfer_t XITC_I2C_TH;


adc_result_info_t adcResultInfoStruct;
adc_config_t adcConfigStruct;
adc_conv_seq_config_t adcConvSeqConfigStruct;

int chr1,chr2, chr3;

//////////////////////////////////////////////

void SysTick_Handler(void)
{
	/* if TICKRATE_HZ is (1000)	i.e., 1000 ticks per second
	 *   this handler will be activated every 1 ms
	 *   in other words, the time base is 1 ms	 */

	if(!timer0_ovf_2ms_tx_en){
		tc0_2ms--;
		if(!tc0_2ms){
			tc0_2ms = TC0_MAX_2MS_UART;
		}
	}

	tc1_5ms--;
	if(!tc1_5ms){
		tc1_5ms = TC1_MAX_5MS_RF;
		debounce_rfpin();

		//A wake-up needs a real key press, not a brief dip on COL2
		if(GPIO_PinRead(GPIO, LPC845_PORT, UI_COL_PIN) == USER_ACTIVE){
			if(wake_key_low_cnt < WAKE_KEY_SAMPLES){
				wake_key_low_cnt++;
			}
			else{
				wake_key_pressed = 1;
			}
		}
		else{
			wake_key_low_cnt = 0;
			wake_key_pressed = 0;
		}
	}

	tc3_100ms--;
	if(!tc3_100ms){
		tc3_100ms = TC3_MAX_100MS;
		timer3_ovf_100ms = 1;
	}

	tc4_10s--;
	if(!tc4_10s){
		tc4_10s = TC4_MAX_10S;
		timer4_ovf_10s = 1;
	}


	tc2_1s--;
	if(!tc2_1s){
		tc2_1s = TC2_MAX_1S;
		read_pulse_counts_in_1sec();
		timer2_ovf_uart = 1;
	}

}


void store_data()
{
	int i,m;
	uint8_t close_dd = Present_Date_Time.date;
	uint8_t close_mm = Present_Date_Time.month;
	uint8_t close_yy = Present_Date_Time.year;

	m = ((Present_Date_Time.hour*60) + Present_Date_Time.minute) / REC_INTERVAL ;

	//After 08:30 this sample belongs to the rain day closing tomorrow
	if(m > RAIN_CLOSE_SLOT){
		Next_Date(&close_dd, &close_mm, &close_yy);
	}

	//A wrong clock (year 00 after the RTC lost power) must not look like a new rain day
	static uint8_t day_started = 0;
	if ((Present_Date_Time.year >= 24) &&
			((Day_Samples[gi_cur_blk].dd != close_dd) ||
			(Day_Samples[gi_cur_blk].mm != close_mm) ||
			(Day_Samples[gi_cur_blk].yy != close_yy) ||
			(Day_Samples[gi_cur_blk].cc != Present_Date_Time.century))){

		gi_cur_blk++;
		gi_cur_blk %= MAX_DAY_BLOCKS_RF;
		Day_Samples[gi_cur_blk].dd = close_dd;
		Day_Samples[gi_cur_blk].mm = close_mm;
		Day_Samples[gi_cur_blk].yy = close_yy;
		Day_Samples[gi_cur_blk].cc = Present_Date_Time.century;
		//CUM_RF only restarts on a real change of rain day (08:45), not on the first slot after
		//a restart: that total may already hold what the LPC1768 returned
		if(day_started){
			rain_day_tips = 0;
			rain_total_restored = 1;	//A new day has nothing to restore
		}
		day_started = 1;

		/*	for(i=0; i<LOG_INTERVAL_1MIN_RF; i++){
			Day_Samples[gi_cur_blk].rf_1min[i] = 0xFF;
		}*/

		for(i=0; i<LOG_INTERVAL_15MIN_RF; i++){
			Day_Samples[gi_cur_blk].rf_15min[i] = 0xFFFF;
		}
	}

	m = (m + RAIN_INDEX_SHIFT) % LOG_INTERVAL_15MIN_RF;	//sample 0 = 08:45, sample 95 = 08:30

	//Take the count and clear it as one step: a tip arriving in between must not be lost
	__disable_irq();
	int slot_tips = rf_val;
	rf_val = 0;
	__enable_irq();

	Day_Samples[gi_cur_blk].rf_15min[m] = slot_tips; //15 minute RF data stored in structure
	rain_day_tips += slot_tips;
	Day_Samples[gi_cur_blk].bat_adc_15min[m] = bat1_adc;
	//for testing
	Day_Samples[gi_cur_blk].wd[m] = present_wd;
	Day_Samples[gi_cur_blk].temperature[m] = present_temperature;
	Day_Samples[gi_cur_blk].humidity[m] = present_humid;
	Day_Samples[gi_cur_blk].ws[m] = present_avg_ws;
	Day_Samples[gi_cur_blk].ws_sec[m] = present_ws_sec_cnt;
	//for testing

	/*	Day_Samples[gi_cur_blk].wd[m] = present_wd;
	Day_Samples[gi_cur_blk].temperature[m] = present_temperature;
	Day_Samples[gi_cur_blk].humidity[m] = present_humid;
	Day_Samples[gi_cur_blk].ws[m] = present_avg_ws;
	Day_Samples[gi_cur_blk].ws_sec[m] = present_ws_sec_cnt;
	 */

	//	Last_Rec_Date_Time.century = Present_Date_Time.century;
	Last_Rec_Date_Time.year = Present_Date_Time.year;
	Last_Rec_Date_Time.month = Present_Date_Time.month;
	Last_Rec_Date_Time.date = Present_Date_Time.date;
	Last_Rec_Date_Time.hour = Present_Date_Time.hour;
	Last_Rec_Date_Time.minute = Present_Date_Time.minute;

	last_rf = Day_Samples[gi_cur_blk].rf_15min[m];
	last_rec_temperature = Day_Samples[gi_cur_blk].temperature[m];
	last_rec_humid = Day_Samples[gi_cur_blk].humidity[m];
	last_rec_ws = Day_Samples[gi_cur_blk].ws[m];
	last_rec_wd = Day_Samples[gi_cur_blk].wd[m];
	last_rec_ws_sec = Day_Samples[gi_cur_blk].ws_sec[m];

}



int get_cur_blk(){
	int blk;
	int match=0;
	uint8_t close_dd = Get_Data_For_Date_Time.date;
	uint8_t close_mm = Get_Data_For_Date_Time.month;
	uint8_t close_yy = Get_Data_For_Date_Time.year;

	//Blocks are labelled by rain-day closing date
	if(((Get_Data_For_Date_Time.hour*60 + Get_Data_For_Date_Time.minute)/REC_INTERVAL) > RAIN_CLOSE_SLOT){
		Next_Date(&close_dd, &close_mm, &close_yy);
	}

	for(blk=0; blk<MAX_DAY_BLOCKS_RF; blk++){
		if ((Day_Samples[blk].dd == close_dd) &&
				(Day_Samples[blk].mm == close_mm) &&
				(Day_Samples[blk].yy == close_yy) &&
				(Day_Samples[blk].cc == Get_Data_For_Date_Time.century)){

			match = 1;
			break;
		}
	}
	if(!match){
		gi_rec_blk_found = 0;
		return(0xFFFF);
	}
	//return(6);
	else{
		gi_rec_blk_found = 1;
		return(blk);
	}

}


void debounce_rfpin(void)
{
	//GPIO_PinRead(GPIO_Type *base, uint32_t port, uint32_t pin)
	//rfpin_newstate = Chip_GPIO_GetPinState(LPC_GPIO_PORT, 0, RF_PIN);

	rfpin_newstate = GPIO_PinRead(GPIO, LPC845_PORT, RF_PIN);

	if(rfpin_newstate != rfpin_prevstate){
		rfpin_debouncecnt = MAX_RF_DEBCNT;
		rfpin_prevstate = rfpin_newstate;
	}
	else if (rfpin_debouncecnt){
		rfpin_debouncecnt--;
		if(rfpin_debouncecnt == 0){
			// count on the rising edge if old state and new are different
			if(rfpin_newstate != rfpin_oldstate){
				rfpin_oldstate = rfpin_newstate;
				//if(rfpin_newstate == 1)
				if((rfpin_newstate == 1) && (calib_mode == 0))
					rf_val++;
				else if((rfpin_newstate == 1) && (calib_mode == 1))
					calib_rf++;

			}
		}
	}
}




void Pin_init()
{
	GPIO_PortInit(GPIO, LPC845_PORT);
	GPIO_PinInit(GPIO, LPC845_PORT, RF_PIN, &gpio_input_config);
	GPIO_PinInit(GPIO, LPC845_PORT, RF_PIN_ALT, &gpio_input_config);
	GPIO_PinInit(GPIO, LPC845_PORT, UI_COL_PIN, &gpio_input_config);
	GPIO_PinInit(GPIO, LPC845_PORT, LPC1768_PWR_STDN, &gpio_output_config);
	GPIO_PinInit(GPIO, LPC845_PORT, SIM7600_PWR_STDN, &gpio_output_config);
	GPIO_PinInit(GPIO, LPC845_PORT, LCD_PWR_STDN, &gpio_output_config);

	GPIO_PinInit(GPIO, LPC845_PORT, UI_ROW_PIN, &gpio_input_config);
}


void row2_input_configuration()
{

	//	GPIO_PortInit(GPIO, LPC845_PORT);
	GPIO_PinInit(GPIO, LPC845_PORT, UI_ROW_PIN, &gpio_input_config);

}


void row2_output_configuration()
{
	//	GPIO_PortInit(GPIO, LPC845_PORT);
	GPIO_PinInit(GPIO, LPC845_PORT, UI_ROW_PIN, &gpio_output_config);

	GPIO_PortClear(GPIO, LPC845_PORT, 1u << UI_ROW_PIN);


}


//One UART buffer serves all frames: don't overwrite a frame that is still being sent
//(a full frame takes about 140 ms at 9600 baud)
static void wait_tx_idle(void)
{
	volatile uint32_t guard = 2000000;
	while(pAck_xmit && guard){
		guard--;
	}
}


void decode_command(void)
{

	if (uart_recv_state == CMD_RECVD){
		chr1 = gc_recv_buf[1];
		chr2 = gc_recv_buf[2];

		switch(chr1){

		//Request From LPC1765 to get particular date&time $RFGccyymmddhhmnss;
		case 'G':
			Get_Data_For_Date_Time.century = ((gc_recv_buf[2]-'0')*10 + (gc_recv_buf[3]-'0'));
			Get_Data_For_Date_Time.year = ((gc_recv_buf[4]-'0')*10 + (gc_recv_buf[5]-'0'));
			Get_Data_For_Date_Time.month = ((gc_recv_buf[6]-'0')*10 + (gc_recv_buf[7]-'0'));
			Get_Data_For_Date_Time.date = ((gc_recv_buf[8]-'0')*10 + (gc_recv_buf[9]-'0'));
			Get_Data_For_Date_Time.hour = ((gc_recv_buf[10]-'0')*10 + (gc_recv_buf[11]-'0'));
			Get_Data_For_Date_Time.minute = ((gc_recv_buf[12]-'0')*10 + (gc_recv_buf[13]-'0'));
			Get_Data_For_Date_Time.sec = ((gc_recv_buf[14]-'0')*10 + (gc_recv_buf[15]-'0'));
			gi_get_rec = 1;
			break;


		case 'S':
			Set_Date_Time.century = ((gc_recv_buf[2]-'0')*10 + (gc_recv_buf[3]-'0'));
			Set_Date_Time.year = ((gc_recv_buf[4]-'0')*10 + (gc_recv_buf[5]-'0'));
			Set_Date_Time.month = ((gc_recv_buf[6]-'0')*10 + (gc_recv_buf[7]-'0'));
			Set_Date_Time.date = ((gc_recv_buf[8]-'0')*10 + (gc_recv_buf[9]-'0'));
			Set_Date_Time.hour = ((gc_recv_buf[10]-'0')*10 + (gc_recv_buf[11]-'0'));
			Set_Date_Time.minute = ((gc_recv_buf[12]-'0')*10 + (gc_recv_buf[13]-'0'));
			Set_Date_Time.sec = ((gc_recv_buf[14]-'0')*10 + (gc_recv_buf[15]-'0'));

			Set_Time();

			Present_Date_Time.century = Set_Date_Time.century;
			Present_Date_Time.year = Set_Date_Time.year;
			Present_Date_Time.month = Set_Date_Time.month;
			Present_Date_Time.date = Set_Date_Time.date;
			Present_Date_Time.hour = Set_Date_Time.hour;
			Present_Date_Time.minute = Set_Date_Time.minute;
			Present_Date_Time.sec = Set_Date_Time.sec;
			break;

		case 'C':
			calib_mode = 1;
			calib_rf = 0;
			//Safety stop if the LPC1768 never ends it (it stops at 5 minutes itself): tips must
			//not keep going to the calibration counter instead of the rain total
			calib_timeout_min = (Present_Date_Time.minute + 6) % 60;
			break;

		case 'c':
			calib_mode = 0;
			calib_rf = 0;
			calib_timeout_min = 0;
			break;

		case 'R':									//Erase Data: restart the rain-day total
			rain_day_tips = 0;
			rain_total_restored = 1;
			break;

		case 'T':									//$T<tips>; total stored before this restart. Adds once only
			if(!rain_total_restored){
				uint32_t stored_tips = 0;
				for(int k = 2; k < 12 && gc_recv_buf[k] >= '0' && gc_recv_buf[k] <= '9'; k++){
					stored_tips = stored_tips*10 + (gc_recv_buf[k]-'0');
				}
				rain_day_tips += stored_tips;
				rain_total_restored = 1;
			}
			break;

		case 'H':									//LPC1768 heartbeat: still making progress
			time_since_reset_interval = 0;
			break;

		case 'U':{									//New firmware arriving from the LPC1768's SD card
			char reply[64];
			bool restart_now = false;
			time_since_reset_interval = 0;
			fw845_recv_command(gc_recv_buf, reply, sizeof(reply), &restart_now);
			wait_tx_idle();
			strcpy(gc_xmit_buf, reply);
			xmit_ptr = gc_xmit_buf;
			pAck_xmit = 1;
			LPC845_UART0->INTENSET = kUSART_TxReady;
			if(restart_now){
				wait_tx_idle();
				NVIC_SystemReset();	//The bootloader installs it
			}
			break;
		}

		case 'n':									//User active
			if((chr2 & 4) || (chr2 == '4')){
				GPIO_PortClear(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN); //turn on 5V
			}
			if((chr2 & 2) || (chr2 == '2')){
				GPIO_PortClear(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN); //turn on 3.8V modem
			}
			break;

		case 'f':
			if((chr2 & 4) || (chr2 == '4')){
				GPIO_PortSet(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN); //turn off 5V
			}
			if((chr2 & 2) || (chr2 == '2')){
				GPIO_PortSet(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN); //turn off 3.8V modem
			}

			if((chr2 & 1) || (chr2 == '1')){
				time_since_reset_interval = 0;
				GPIO_PortSet(GPIO, LPC845_PORT, 1u << LPC1768_PWR_STDN);
				GPIO_PortSet(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN);
				GPIO_PortSet(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN);
				//	GPIO_PortClear(GPIO, LPC845_PORT, 1u << UI_ROW_PIN);

				row2_output_configuration();
				sleep_holdoff_100ms = SLEEP_HOLDOFF_100MS;
			}
			break;

		default:
			break;
		}

		uart_recv_state = WAITING_FOR_STX;
	}

}



void USART0_IRQHandler(void)
{
	char c;
	//A received byte runs this too, so check the transmitter has room: writing while it's full
	//overwrites a byte still waiting to go out
	if((LPC845_UART0->INTENSET & kUSART_TxReady) && (LPC845_UART0->STAT & kUSART_TxReady)){
		if(*xmit_ptr != '\0'){
			LPC845_UART0->TXDAT = *xmit_ptr;
			xmit_ptr++;
		}

		else{
			LPC845_UART0->INTENCLR = kUSART_TxReady;
			timer0_ovf_2ms_tx_en = 0;
			pAck_xmit = 0;
		}
	}

	if(LPC845_UART0->STAT & kUSART_RxReady){
		c = LPC845_UART0->RXDAT;
		switch(uart_recv_state){
		case WAITING_FOR_STX:
			if(c == '$'){
				gc_recv_buf[0] = '$';
				recv_cnt = 1;
				uart_recv_state = RECEIVING_CMD;
			}
			break;
		case RECEIVING_CMD:
			if(c == '$')
				recv_cnt = 1;
			else{
				gc_recv_buf[recv_cnt++] = c;
			}
			//if(c == '\n')
			if(c == ';')
				uart_recv_state = CMD_RECVD;
			if(recv_cnt >= RBUF_SIZE)
				uart_recv_state = WAITING_FOR_STX;
			break;
		case CMD_RECVD:
			break;
		default:
			break;
		}
	}

}








void Init_UART_I2C_ADC_Pins(void)
{
	/* Enables clock for IOCON.: enable */
	CLOCK_EnableClock(kCLOCK_Iocon);
	/* Enables clock for switch matrix.: enable */
	CLOCK_EnableClock(kCLOCK_Swm);

	/* USART0_TXD connect to P0_25 */
	SWM_SetMovablePinSelect(SWM0, kSWM_USART0_TXD, kSWM_PortPin_P0_25);

	/* USART0_RXD connect to P0_24 */
	SWM_SetMovablePinSelect(SWM0, kSWM_USART0_RXD, kSWM_PortPin_P0_24);



	SWM_SetMovablePinSelect(SWM0, kSWM_USART1_RXD, kSWM_PortPin_P0_23);


	//*iocon_pio17 = (1 << 7);

	/*
	IOCON->PIO[IOCON_INDEX_PIO0_17] = (1 << 7);
	IOCON->PIO[IOCON_INDEX_PIO0_18] = (1 << 7);
	IOCON->PIO[IOCON_INDEX_PIO0_21] = (1 << 7);
	 */

	/* I2C0_SDA connect to P0_11 */
	SWM_SetFixedPinSelect(SWM0, kSWM_I2C0_SDA, true);

	/* I2C0_SCL connect to P0_10 */
	SWM_SetFixedPinSelect(SWM0, kSWM_I2C0_SCL, true);


	/* I2C0_SDA connect to P0_11 */
	//	SWM_SetMovablePinSelect(SWM0, kSWM_I2C1_SDA, kSWM_PortPin_P0_20);
	SWM_SetMovablePinSelect(SWM0, kSWM_I2C1_SDA, kSWM_PortPin_P0_20);


	/* I2C0_SCL connect to P0_10 */
	//	SWM_SetMovablePinSelect(SWM0, kSWM_I2C1_SCL, kSWM_PortPin_P0_22);
	SWM_SetMovablePinSelect(SWM0, kSWM_I2C1_SCL, kSWM_PortPin_P0_22);

	// Configure the SWM



	//	SWM_SetFixedPinSelect(SWM0, kSWM_ADC_CHN9, true);
	IOCON->PIO[IOCON_INDEX_PIO0_21] = 0x80;
	IOCON->PIO[IOCON_INDEX_PIO0_18] = 0x80;
	IOCON->PIO[IOCON_INDEX_PIO0_17] = 0x80;


	SWM_SetFixedPinSelect(SWM0, kSWM_ADC_CHN5, true);
	SWM_SetFixedPinSelect(SWM0, kSWM_ADC_CHN8, true);
	SWM_SetFixedPinSelect(SWM0, kSWM_ADC_CHN9, true);



	/* Disable clock for switch matrix. */


	CLOCK_DisableClock(kCLOCK_Swm);
}



void USART0_Init(void)
{
	usart_config_t config;

	/* Default config by using USART_GetDefaultConfig():
	 * config.baudRate_Bps = 9600U;
	 * config.parityMode = kUSART_ParityDisabled;
	 * config.stopBitCount = kUSART_OneStopBit;
	 * config.bitCountPerChar = kUSART_8BitsPerChar;
	 * config.loopback = false;
	 * config.enableRx = false;
	 * config.enableTx = false;
	 * config.syncMode = kUSART_SyncModeDisabled;
	 */
	USART_GetDefaultConfig(&config);
	config.baudRate_Bps = 9600;
	config.enableRx = true;
	config.enableTx = true;

	/* Initialize the USART with configuration. */
	USART_Init(LPC845_UART0, &config, LPC845_UART0_CLK_FREQ);


	// USART_EnableInterrupts(LPC845_UART0, kUSART_RxReadyInterruptEnable );//kUSART_RxReady
	//	USART_EnableInterrupts(LPC845_UART0, kUSART_RxReady );

	//	NVIC_EnableIRQ(LPC845_UART0_IRQn);
	USART_EnableInterrupts(LPC845_UART0, kUSART_RxReadyInterruptEnable);
	EnableIRQ(LPC845_UART0_IRQn);

}


void USART1_Init(void)
{
	usart_config_t config;

	/* Default config by using USART_GetDefaultConfig():
	 * config.baudRate_Bps = 9600U;
	 * config.parityMode = kUSART_ParityDisabled;
	 * config.stopBitCount = kUSART_OneStopBit;
	 * config.bitCountPerChar = kUSART_8BitsPerChar;
	 * config.loopback = false;
	 * config.enableRx = false;
	 * config.enableTx = false;
	 * config.syncMode = kUSART_SyncModeDisabled;
	 */
	USART_GetDefaultConfig(&config);
	config.baudRate_Bps = 9600;
	config.enableRx = true;
	config.enableTx = true;

	/* Initialize the USART with configuration. */
	USART_Init(LPC845_UART1, &config, LPC845_UART1_CLK_FREQ);


	// USART_EnableInterrupts(LPC845_UART0, kUSART_RxReadyInterruptEnable );//kUSART_RxReady
	//	USART_EnableInterrupts(LPC845_UART0, kUSART_RxReady );

	//	NVIC_EnableIRQ(LPC845_UART0_IRQn);
	USART_EnableInterrupts(LPC845_UART1, kUSART_RxReadyInterruptEnable);
	EnableIRQ(LPC845_UART1_IRQn);

}



void I2C0_init(void)
{

	i2c_master_config_t masterConfig;

	/*
	 * masterConfig.debugEnable = false;
	 * masterConfig.ignoreAck = false;
	 * masterConfig.pinConfig = kI2C_2PinOpenDrain;
	 * masterConfig.baudRate_Bps = 100000U;
	 * masterConfig.busIdleTimeout_ns = 0;
	 * masterConfig.pinLowTimeout_ns = 0;
	 * masterConfig.sdaGlitchFilterWidth_ns = 0;
	 * masterConfig.sclGlitchFilterWidth_ns = 0;
	 */

	I2C_MasterGetDefaultConfig(&masterConfig);

	/* Change the default baudrate configuration */
	masterConfig.baudRate_Bps = I2C_BAUDRATE;

	/* Initialize the I2C master peripheral */
	I2C_MasterInit(I2C_MASTER, &masterConfig, I2C_MASTER_CLOCK_FREQUENCY);

}


void I2C1_init(void)
{

	i2c_master_config_t masterConfig1;

	I2C_MasterGetDefaultConfig(&masterConfig1);

	masterConfig1.baudRate_Bps = I2C_BAUDRATE1;

	I2C_MasterInit(I2C_MASTER1, &masterConfig1, I2C_MASTER_CLOCK_FREQUENCY1);

}


/*
void read_wd_val(void){
	int i,j;
	for (i=0; i<9; i++)
		i2c_clkval[i] = 0;

	float wd;

	char wd_cmd[6]; //={0x02,0x90,0x00};

	wd_cmd[0] = 0x0C;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<1000;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	wd_cmd[1] = 0x0D;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<1000;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	wd = ((i2c_clkval1[0]<<8) | (i2c_clkval1[1]));
//	wd	= ((i2c_clkval1[0])*256) + (i2c_clkval1[1]);


//	  int ang_hi = _getRegister(_RAWANGLEAddressMSB);
//	  int ang_lo = _getRegister(_RAWANGLEAddressLSB);

//	wd  = i2c_clkval1[0] * 22.5 + i2c_clkval1[1] * 0.087890625;
	wd = wd * 0.087;

//	wd = (int) wd;

//	PRINTF("WD = %d\r\n", wd);

	PRINTF("Wind Direction = %03d.%02d \r\n", ((int)wd * 1000)/1000, (int)(wd * 1000) % 1000);


	for(i=0;i<100;i++)
		for(j=0;j<200;j++);
}

 */

/*
void read_wd_val(void){
	int i,j;
	for (i=0; i<9; i++)
		i2c_clkval[i] = 0;

	float wd;
	long revolutions = 0;   // number of revolutions the encoder has made
	double position = 0;    // the calculated value the encoder is at
	double output;          // raw value from AS5600
	long lastOutput;        // last output from AS5600

	char wd_cmd[6]; //={0x02,0x90,0x00};

	wd_cmd[0] = 0x0C;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	wd_cmd[1] = 0x0D;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	wd = ((i2c_clkval1[0]<<8) | (i2c_clkval1[1]));
//	wd	= ((i2c_clkval1[0])*256) + (i2c_clkval1[1]);

	  if ((lastOutput - wd) > 2047 )        // check if a full rotation has been made
	    revolutions++;
	  if ((lastOutput - wd) < -2047 )
	    revolutions--;

	  position = revolutions * 4096 + wd;   // calculate the position the the encoder is at based off of the number of revolutions

	  //Serial.println(position);

	  lastOutput = wd;                      // save the last raw value for the next loop

	PRINTF("WD = %02d.%02d \r\n", ((int)temperature1 * 100)/100, (int)(temperature1 * 100) % 100);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);
}

 */


void read_th_val(void)
{

	int i, j;
	char hdc_nor_cmd[6];

	hdc_nor_cmd[0] = 0x0E;
	hdc_nor_cmd[1] = 0x70;
	hdc_nor_cmd[2] = 0x0F;
	hdc_nor_cmd[3] = 0x00;

	i2c_clkval1[0]= 0;
	i2c_clkval1[1]= 0;
	i2c_clkval1[2]= 0;
	i2c_clkval1[3]= 0;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 4;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);


	for(i=0;i<800;i++)
		for(j=0;j<20;j++);

	hdc_nor_cmd[0] = 0x00;
	hdc_nor_cmd[1] = 0x01;
	hdc_nor_cmd[2] = 0x02;
	hdc_nor_cmd[3] = 0x03;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 4;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);


	for(i=0;i<800;i++)
		for(j=0;j<100;j++);

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[0];
	XITC_I2C_TH.dataSize = 4;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	present_temperature	= ((i2c_clkval1[1])*256) + (i2c_clkval1[0]);
	present_humid	= ((i2c_clkval1[3])*256) + (i2c_clkval1[2]);

	if((present_temperature == 0) || (present_humid == 0))
		I2C1_init();

	for(i=0;i<800;i++)
		for(j=0;j<100;j++);
}

/*
void read_th_val(void)
{

//	int temperature, humidity;
//	float temperature1, humidity1;

	int i, j;
	char hdc_nor_cmd[6]; //={0x02,0x90,0x00};

	hdc_nor_cmd[0] = 0x0E;
	hdc_nor_cmd[1] = 0x70;
	hdc_nor_cmd[2] = 0x0F;
	hdc_nor_cmd[3] = 0x00;

	i2c_clkval1[0]= 0;
	i2c_clkval1[1]= 0;
	i2c_clkval1[2]= 0;
	i2c_clkval1[3]= 0;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 4;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);


	for(i=0;i<800;i++)
		for(j=0;j<20;j++);

	hdc_nor_cmd[0] = 0x00;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);


	hdc_nor_cmd[0] = 0x01;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	hdc_nor_cmd[0] = 0x02;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[2];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	hdc_nor_cmd[0] = 0x03;

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &hdc_nor_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = HDC_DEV_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[3];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);


	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	present_temperature	= ((i2c_clkval1[1])*256) + (i2c_clkval1[0]);
	present_humid	= ((i2c_clkval1[3])*256) + (i2c_clkval1[2]);


//	temperature1 = ((float)present_temperature/65536) * 165 - 40;
//	humidity1 = ((float)present_humid/65534) * 100;


//	PRINTF("Temperature = %02d.%02d \r\n", ((int)temperature1 * 100)/100, (int)(temperature1 * 100) % 100);
//	PRINTF("HUMIDITY = %02d.%02d \r\n", ((int)humidity1 * 100)/100, (int)(humidity1 * 100) % 100);

	for(i=0;i<100;i++)
		for(j=0;j<100;j++);
}
 */

//   result = I2C_MasterReadBlocking(base, xfer->data, xfer->dataSize, xfer->flags);
//   result = I2C_MasterWriteBlocking(base, xfer->data, xfer->dataSize, xfer->flags);

void init_ext_rtc(void){
	int i, j;

	//Only start the RTC if it is stopped (bit 7 of register 0). Writing the seconds on every
	//restart lost up to 59 seconds of real time each time.
	i2c_clkval[0]= 0;		// start address is 0
	i2c_clkval[1]= 0;
	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[0];
	XITC_I2C_IDT.dataSize = 1;
	XITC_I2C_IDT.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);

	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[1];
	XITC_I2C_IDT.dataSize = 1;
	XITC_I2C_IDT.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);

	if(i2c_clkval[1] & 0x80){
		i2c_clkval[0]= 0;
		i2c_clkval[1] &= 0x7F;
		XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
		XITC_I2C_IDT.flags = 0;
		XITC_I2C_IDT.data = &i2c_clkval[0];
		XITC_I2C_IDT.dataSize = 2;
		XITC_I2C_IDT.direction = kI2C_Write;
		I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);
	}

	for(i=0;i<100;i++)
		for(j=0;j<100;j++);

	Get_Time();
}


void Get_Cur_Date_Time(xDateTime_def *DateTime)
{
	int i;

	for (i=0; i<9; i++)		//Version C
		i2c_clkval[i] = 0;

	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[0];
	XITC_I2C_IDT.dataSize = 1;		// start address and data for register 0
	XITC_I2C_IDT.direction = kI2C_Write;

	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);


	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[1];
	XITC_I2C_IDT.dataSize = 8;		// start address and data for register 0
	XITC_I2C_IDT.direction = kI2C_Read;


	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);

	DateTime->year		= (i2c_clkval[7]/16)*10+(i2c_clkval[7]%16);
	DateTime->month	= (i2c_clkval[6]/16)*10+(i2c_clkval[6]%16);
	DateTime->date		= (i2c_clkval[5]/16)*10+(i2c_clkval[5]%16);

	i2c_clkval[3]	&= 0x3F;	// remove unwanted bits
	DateTime->hour		= (i2c_clkval[3]/16)*10+(i2c_clkval[3]%16);

	DateTime->minute	= (i2c_clkval[2]/16)*10+(i2c_clkval[2]%16);

	i2c_clkval[1]	&= 0x7F;	// remove unwanted bits
	DateTime->sec	= (i2c_clkval[1]/16)*10+(i2c_clkval[1]%16);
}


void Get_Time()
{

	int i,j;

	next_date.date = Present_Date_Time.date;
	next_date.month = Present_Date_Time.month;
	next_date.year = Present_Date_Time.year;

	Next_Date(&next_date.date, &next_date.month, &next_date.year);

	Get_Cur_Date_Time(&rtc_read_date);


	if((Present_Date_Time.date == rtc_read_date.date) && (Present_Date_Time.month == rtc_read_date.month) && (Present_Date_Time.year == rtc_read_date.year)){
		Present_Date_Time.date = rtc_read_date.date;
		Present_Date_Time.month = rtc_read_date.month;
		Present_Date_Time.year = rtc_read_date.year;
		Present_Date_Time.hour = rtc_read_date.hour;
		Present_Date_Time.minute = rtc_read_date.minute;
		Present_Date_Time.sec = rtc_read_date.sec;
	}

	else if((next_date.date == rtc_read_date.date) && (next_date.month == rtc_read_date.month) && (next_date.year == rtc_read_date.year)){
		Present_Date_Time.date = rtc_read_date.date;
		Present_Date_Time.month = rtc_read_date.month;
		Present_Date_Time.year = rtc_read_date.year;
		Present_Date_Time.hour = rtc_read_date.hour;
		Present_Date_Time.minute = rtc_read_date.minute;
		Present_Date_Time.sec = rtc_read_date.sec;
	}

	else{
		for(i=0;i<500;i++)
			for(j=0;j<600;j++);

		Get_Cur_Date_Time(&rtc_read_date_copy);

		if((rtc_read_date_copy.date == rtc_read_date.date) && (rtc_read_date_copy.month == rtc_read_date.month) && (rtc_read_date_copy.year == rtc_read_date.year)){
			Present_Date_Time.date = rtc_read_date_copy.date;
			Present_Date_Time.month = rtc_read_date_copy.month;
			Present_Date_Time.year = rtc_read_date_copy.year;
			Present_Date_Time.hour = rtc_read_date_copy.hour;
			Present_Date_Time.minute = rtc_read_date_copy.minute;
			Present_Date_Time.sec = rtc_read_date_copy.sec;

		}
	}

}


void Next_Date(uint8_t *Nd, uint8_t *Nm, uint8_t *Ny)
{
	int no_of_days[14] = { 29, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	int prev_mm;

	*Nd = *Nd + 1;

	if ((*(Ny) % 4 == 0) && (*Nm == 2)) {
		prev_mm = 0;
	} else {
		prev_mm = *Nm;
	}
	if (*Nd > no_of_days[prev_mm]) {
		*Nd = 1;
		*Nm = *Nm + 1;
		if (*Nm > 12) {
			*Nm = 1;
			*Ny = *Ny + 1;
		}
	}

}



/*
void Get_Time(xDateTime_def *DateTime)
{
	int i;
	for (i=1; i<9; i++)
		i2c_clkval[i] = 0;

	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[0];
	XITC_I2C_IDT.dataSize = 1;		// start address and data for register 0
	XITC_I2C_IDT.direction = kI2C_Write;

	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);


	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[1];
	XITC_I2C_IDT.dataSize = 8;		// start address and data for register 0
	XITC_I2C_IDT.direction = kI2C_Read;


	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);

	DateTime->year		= (i2c_clkval[7]/16)*10+(i2c_clkval[7]%16);
	DateTime->month	= (i2c_clkval[6]/16)*10+(i2c_clkval[6]%16);
	DateTime->date		= (i2c_clkval[5]/16)*10+(i2c_clkval[5]%16);

	i2c_clkval[3]	&= 0x3F;	// remove unwanted bits
	DateTime->hour		= (i2c_clkval[3]/16)*10+(i2c_clkval[3]%16);

	DateTime->minute	= (i2c_clkval[2]/16)*10+(i2c_clkval[2]%16);

	i2c_clkval[1]	&= 0x7F;	// remove unwanted bits
	DateTime->sec	= (i2c_clkval[1]/16)*10+(i2c_clkval[1]%16);
}
 */


void Set_Time()
{
	i2c_clkval[0]= 0;		// start address
	i2c_clkval[1]= ((Set_Date_Time.sec)/10)*16+((Set_Date_Time.sec)%10);	// bit 7 clear keeps the clock running

	i2c_clkval[2]= ((Set_Date_Time.minute)/10)*16+((Set_Date_Time.minute)%10);
	i2c_clkval[3]= ((Set_Date_Time.hour)/10)*16+((Set_Date_Time.hour)%10);
	i2c_clkval[4]= 1; // ignore the week day presently;
	i2c_clkval[5]= ((Set_Date_Time.date)/10)*16+((Set_Date_Time.date)%10);
	i2c_clkval[6]= ((Set_Date_Time.month)/10)*16+((Set_Date_Time.month)%10);
	//xNewDateTime->year -= 2000;
	i2c_clkval[7]= ((Set_Date_Time.year)/10)*16+((Set_Date_Time.year)%10);
	i2c_clkval[8]= 0;	// do not opt for anything


	XITC_I2C_IDT.slaveAddress = IDT_DEV_ADDR;
	XITC_I2C_IDT.flags = 0;
	XITC_I2C_IDT.data = &i2c_clkval[0];
	XITC_I2C_IDT.dataSize = 9;		// start address and data for register 0
	XITC_I2C_IDT.direction = kI2C_Write;


	I2C_MasterTransferBlocking(I2C_MASTER, &XITC_I2C_IDT);

}


/*
static void ADC_Configuration(void)
{
	int i,j;
	//int i,j,temp2;

	adc_config_t adcConfigStruct;
	adc_conv_seq_config_t adcConvSeqConfigStruct;

	adcConfigStruct.clockMode = kADC_ClockSynchronousMode; // Using sync clock source.
	adcConfigStruct.clockDividerNumber = ADC_CLOCK_DIVIDER;
	adcConfigStruct.enableLowPowerMode = false;
	adcConfigStruct.voltageRange = kADC_HighVoltageRange;//kADC_LowVoltageRange,  kADC_HighVoltageRange

	ADC_Init(ADC_BASE, &adcConfigStruct);

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	ADC_EnableConvSeqA(ADC_BASE, false);

	//	adcConvSeqConfigStruct.channelMask = ((1 << ADC_SAMPLE_CHANNEL_NUMBER5) |(1 << ADC_SAMPLE_CHANNEL_NUMBER8) | (1 <<ADC_SAMPLE_CHANNEL_NUMBER9));
	//	adcConvSeqConfigStruct.channelMask = ((1 << ADC_SAMPLE_CHANNEL_NUMBER9) |(1 << ADC_SAMPLE_CHANNEL_NUMBER8) | (1 <<ADC_SAMPLE_CHANNEL_NUMBER5));

	//	adcConvSeqConfigStruct.channelMask =  (1 <<ADC_SAMPLE_CHANNEL_NUMBER5) ;

	//adcConvSeqConfigStruct.channelMask = 800;


	//	temp2 = ((1<<5) | (1<<8) | (1<<9));
	//	adcConvSeqConfigStruct.channelMask = temp2;

	adcConvSeqConfigStruct.channelMask = (1 <<ADC_SAMPLE_CHANNEL_NUMBER9);

	//	adcConvSeqConfigStruct.channelMask = ((1 << ADC_SAMPLE_CHANNEL_NUMBER5) |(1 << ADC_SAMPLE_CHANNEL_NUMBER8) | (1 <<ADC_SAMPLE_CHANNEL_NUMBER9));

	adcConvSeqConfigStruct.triggerMask = 0U;
	//	adcConvSeqConfigStruct.triggerPolarity = kADC_TriggerPolarityPositiveEdge;
	//	adcConvSeqConfigStruct.enableSingleStep = false;
	//	adcConvSeqConfigStruct.enableSyncBypass = false;
	//	adcConvSeqConfigStruct.interruptMode = kADC_InterruptForEachSequence;

	ADC_SetConvSeqAConfig(ADC_BASE, &adcConvSeqConfigStruct);
	ADC_EnableConvSeqA(ADC_BASE, true); // Enable the conversion sequence A.

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	// Clear the result register.

	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER5, &adcResultInfoStruct))
	{
	}
	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);


	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER8, &adcResultInfoStruct))
	{
	}
	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);


	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER9, &adcResultInfoStruct))
	{
	}
	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);

}
 */

/*
static void ADC_Configuration1(void){
	int i,j;

	adc_config_t adcConfigStruct;
	adc_conv_seq_config_t adcConvSeqConfigStruct;

	adcConfigStruct.clockMode = kADC_ClockSynchronousMode; // Using sync clock source.
	adcConfigStruct.clockDividerNumber = ADC_CLOCK_DIVIDER;
	adcConfigStruct.enableLowPowerMode = false;
	adcConfigStruct.voltageRange = kADC_HighVoltageRange;//kADC_LowVoltageRange,  kADC_HighVoltageRange

	ADC_Init(ADC_BASE, &adcConfigStruct);

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	ADC_EnableConvSeqA(ADC_BASE, false);
	//temp2 = ((1<<5) | (1<<8) | (1<<9));

	adcConvSeqConfigStruct.channelMask = (1 << ADC_SAMPLE_CHANNEL_NUMBER5);

	adcConvSeqConfigStruct.triggerMask = 0U;
	ADC_SetConvSeqAConfig(ADC_BASE, &adcConvSeqConfigStruct);
	ADC_EnableConvSeqA(ADC_BASE, true); // Enable the conversion sequence A.

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

}
 */

static void ADC_Configuration2(void){
	int i,j;

	adc_config_t adcConfigStruct;
	adc_conv_seq_config_t adcConvSeqConfigStruct;

	adcConfigStruct.clockMode = kADC_ClockSynchronousMode; /* Using sync clock source. */
	adcConfigStruct.clockDividerNumber = ADC_CLOCK_DIVIDER;
	adcConfigStruct.enableLowPowerMode = false;
	adcConfigStruct.voltageRange = kADC_HighVoltageRange;//kADC_LowVoltageRange,  kADC_HighVoltageRange

	ADC_Init(ADC_BASE, &adcConfigStruct);

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	ADC_EnableConvSeqA(ADC_BASE, false);
	//temp2 = ((1<<5) | (1<<8) | (1<<9));

	adcConvSeqConfigStruct.channelMask = (1 << ADC_SAMPLE_CHANNEL_NUMBER8);

	adcConvSeqConfigStruct.triggerMask = 0U;
	ADC_SetConvSeqAConfig(ADC_BASE, &adcConvSeqConfigStruct);
	ADC_EnableConvSeqA(ADC_BASE, true); /* Enable the conversion sequence A. */

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

}

static void ADC_Configuration3(void){
	int i,j;

	adc_config_t adcConfigStruct;
	adc_conv_seq_config_t adcConvSeqConfigStruct;

	adcConfigStruct.clockMode = kADC_ClockSynchronousMode; /* Using sync clock source. */
	adcConfigStruct.clockDividerNumber = ADC_CLOCK_DIVIDER;
	adcConfigStruct.enableLowPowerMode = false;
	adcConfigStruct.voltageRange = kADC_HighVoltageRange;//kADC_LowVoltageRange,  kADC_HighVoltageRange

	ADC_Init(ADC_BASE, &adcConfigStruct);

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	ADC_EnableConvSeqA(ADC_BASE, false);
	//temp2 = ((1<<5) | (1<<8) | (1<<9));

	adcConvSeqConfigStruct.channelMask = (1 << ADC_SAMPLE_CHANNEL_NUMBER9);

	adcConvSeqConfigStruct.triggerMask = 0U;
	ADC_SetConvSeqAConfig(ADC_BASE, &adcConvSeqConfigStruct);
	ADC_EnableConvSeqA(ADC_BASE, true); /* Enable the conversion sequence A. */

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

}

/*
void read_adc1_values(void){

//	float solar;
	int i,j;

	ADC_Configuration1();
	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER5, &adcResultInfoStruct))
	{
	}
	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	solar_adc = adcResultInfoStruct.result;
	//r1=620k r2=100k r2/(r1+r2)
	solar = (((float)(solar_adc) * 3.3/4095)/0.138);



//	PRINTF("ADC Value 5 = %04d\r\n", solar_adc);
//	PRINTF("Solar voltage = %02d.%02d \r\n", ((int)solar * 100)/100, (int)(solar * 100) % 100);

	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

}
 */

void read_adc3_values(void){

	//	float bat1;
	int i,j;

	ADC_Configuration3();
	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);


	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER9, &adcResultInfoStruct))
	{
	}
	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);
	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	bat1_adc = adcResultInfoStruct.result;
	//r1=620k r2=100k r2/(r1+r2)
	/*	bat1 = (((float)(bat1_adc) * 3.3/4095)/0.138);



	PRINTF("ADC Value 9 = %04d\r\n", bat1_adc);
	PRINTF("Bat1 Voltage = %02d.%02d \r\n", ((int)bat1 * 100)/100, (int)(bat1 * 100) % 100);

	 */


}


void read_adc2_values(void){

	//	float bat2, wind_direction;
	int i,j;

	ADC_Configuration2();
	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER8, &adcResultInfoStruct))
	{
	}
	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);
	for(i=0;i<100;i++)
		for(j=0;j<100;j++);

	//bat2_adc = adcResultInfoStruct.result;
	present_wd = adcResultInfoStruct.result;


	//r1=620k r2=100k r2/(r1+r2)
	//	bat2 = (((float)(bat2_adc) * 3.3/4095)/0.138);


	//	wind_direction = ((int)(present_wd * 0.088)) % 360;		// 360/4095 = 0.088
	//	PRINTF("ADC Value 8 = %04d\r\n", present_wd);
	//	PRINTF("Wind Direction = %02d.%02d \r\n", ((int)wind_direction * 100)/100, (int)(wind_direction * 100) % 100);

}

void ADC_ClockPower_Configuration(void){

	uint32_t frequency;

	/* Attach FRO clock to ADC0. */
	CLOCK_Select(kADC_Clk_From_Fro);
	CLOCK_SetClkDivider(kCLOCK_DivAdcClk, 1U);
	/* Power on ADC0. */
	POWER_DisablePD(kPDRUNCFG_PD_ADC0);

	frequency = CLOCK_GetFreq(ADC_CLOCK_SOURCE) / CLOCK_GetClkDivider(kCLOCK_DivAdcClk);

	ADC_DoSelfCalibration(ADC_BASE, frequency);

}


/*

void read_adc1_values(void){

	float solar;
	int i,j;

	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER5, &adcResultInfoStruct))
	{
	}
	//	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	solar_adc = adcResultInfoStruct.result;
	//r1=620k r2=100k r2/(r1+r2)
	solar = (((float)(solar_adc) * 3.3/4095)/0.138);


	PRINTF("ADC Value 5 = %04d\r\n", solar_adc);
	PRINTF("Solar voltage = %02d.%02d \r\n", ((int)solar * 100)/100, (int)(solar * 100) % 100);



	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

}


void read_adc2_values(void){

	float bat2;
	int i,j;

	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER8, &adcResultInfoStruct))
	{
	}
	//	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	bat2_adc = adcResultInfoStruct.result;
	//r1=620k r2=100k r2/(r1+r2)
	bat2 = (((float)(bat2_adc) * 3.3/4095)/0.138);


	PRINTF("ADC Value 8 = %04d\r\n", bat2_adc);
	PRINTF("bat2 volt = %02d.%02d \r\n", ((int)bat2 * 100)/100, (int)(bat2 * 100) % 100);



	for(i=0;i<100;i++)
		for(j=0;j<2000;j++);

}




void read_adc3_values(void){

	int i,j;

	ADC_DoSoftwareTriggerConvSeqA(ADC_BASE);
	while (!ADC_GetChannelConversionResult(ADC_BASE, ADC_SAMPLE_CHANNEL_NUMBER9, &adcResultInfoStruct))
	{
	}
	//	ADC_GetConvSeqAGlobalConversionResult(ADC_BASE, &adcResultInfoStruct);
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	bat1_adc = adcResultInfoStruct.result;
	//r1=620k r2=100k r2/(r1+r2)
	//	bat1 = (((float)(bat1_adc) * 3.3/4095)/0.138);


	//	PRINTF("ADC Value 9 = %04d\r\n", bat1_adc);
	//	PRINTF("Bat1 Voltage = %02d.%02d \r\n", ((int)bat1 * 100)/100, (int)(bat1 * 100) % 100);



	for(i=0;i<100;i++)
		for(j=0;j<500;j++);

}

 */






/*
void get_status(){
	int i,j;
	for (i=0; i<9; i++)
		i2c_clkval[i] = 0;

	int status;

	float wd;

	char wd_cmd[6]; //={0x02,0x90,0x00};

	wd_cmd[0] = 0x0B;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<1000;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);


	status = i2c_clkval1[0] & 0b00111000;

	wd_cmd[0] = 0x05;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<1000;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[0];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	wd_cmd[1] = 0x06;

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &wd_cmd[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Write;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<1000;i++)
		for(j=0;j<200;j++);

	XITC_I2C_TH.slaveAddress = WD_ADDR;
	XITC_I2C_TH.flags = 0;
	XITC_I2C_TH.data = &i2c_clkval1[1];
	XITC_I2C_TH.dataSize = 1;
	XITC_I2C_TH.direction = kI2C_Read;
	I2C_MasterTransferBlocking(I2C_MASTER1, &XITC_I2C_TH);

	for(i=0;i<100;i++)
		for(j=0;j<200;j++);

	wd = ((i2c_clkval1[0]<<8) | (i2c_clkval1[1]));
//	wd	= ((i2c_clkval1[0])*256) + (i2c_clkval1[1]);


//	  int ang_hi = _getRegister(_RAWANGLEAddressMSB);
//	  int ang_lo = _getRegister(_RAWANGLEAddressLSB);

//	wd  = i2c_clkval1[0] * 22.5 + i2c_clkval1[1] * 0.087890625;
//	wd = wd * 0.087;

//	wd = (int) wd;

//	PRINTF("WD = %d\r\n", wd);

//	PRINTF("Wind Direction = %03d.%02d \r\n", ((int)wd * 1000)/1000, (int)(wd * 1000) % 1000);

	for(i=0;i<5000;i++)
		for(j=0;j<1000;j++);

}

 */


void init_counter_ws(void){
	// Enable clocks to peripherals
	SYSCON->SYSAHBCLKCTRL0 |= (SWM|SCT);

	SWM_SetMovablePinSelect(SWM0, kSWM_SCT_PIN0, kSWM_PortPin_P0_19);

	INPUTMUX->SCT_INMUX[0] = 0x0;

	// Configure the SCT ...
	// Give the module a reset
	SYSCON->PRESETCTRL0 &= (SCT0_RST_N);
	SYSCON->PRESETCTRL0 |= ~(SCT0_RST_N);


	SCT0->CONFIG |= (0<<UNIFY) | (Input<<CLKMODE) | (In0re<<CKSEL);

	SCT0->CTRL |= (0<<Stop_L) | (1<<Halt_L) | (1<<CLRCTR_L) | (0<<BIDIR_L) | (0<<PRE_L);

	SCT0->CTRL &= ~(1<<Halt_L);

}



void read_pulse_counts_in_1sec(void){

	//int pulse;
	/////////////////////
	//pulse_count = 0;
	// Avg_windspeed



	//	Global_Data.avg_ws = ((float)(ws_tot / 20)) / time_gone;
	//	Global_Data.avg_ws = Global_Data.avg_ws + ((float)ws_cf/10);

	// inst wind speed
	//	Global_Data.inst_ws = ((float)inst_ws) / 20;
	present_ws_sec_cnt++;


	// Read the L counter, halt it, reset it, restart it
	inst_ws = SCT0->COUNT;      // Read
	SCT0->CTRL |= (1<<Halt_L);      // Halt
	SCT0->CTRL |= (1<<CLRCTR_L);    // Reset
	SCT0->CTRL &= ~(1<<Halt_L);     // Restart

	//	printf("Number of pulses in the last second = %d \r\n", inst_ws);
	//printf("Number of pulses in the last second1 = %d\n\n\r", pulse_count);


	present_avg_ws += inst_ws;

	//	printf("Number of pulses in the last 15 min = %d \r\n", present_avg_ws);

	//	inst_ws1 = (float)inst_ws /20 ;

	//	PRINTF("Wind Speed = %02d.%02d \r\n", ((int)inst_ws1 * 100)/100, (int)(inst_ws1 * 100) % 100);


	//	avg_ws1 = ((float)(present_avg_ws / 20)) / present_ws_sec_cnt;

	//	PRINTF("Avg. Wind Speed = %02d.%02d \r\n", ((int)avg_ws1 * 100)/100, (int)(avg_ws1 * 100) % 100);

	////////////////////
}


void USART1_IRQHandler(void)
{



	char c;
	/*	if(LPC845_UART1->INTENSET & kUSART_TxReady){
		if(*xmit_ptr1 != '\0'){
			LPC845_UART1->TXDAT = *xmit_ptr1;
			xmit_ptr1++;
		}

		else{
			LPC845_UART1->INTENCLR = kUSART_TxReady;
			timer0_ovf_2ms_tx_en = 0;
			pAck_xmit1 = 0;
		}
	}*/

	if(LPC845_UART1->STAT & kUSART_RxReady){
		c = LPC845_UART1->RXDAT;
		switch(uart_recv_state1){
		case WAITING_FOR_STX1:
			if(c == '$'){
				gc_recv_buf1[0] = '$';
				recv_cnt1 = 1;
				uart_recv_state1 = RECEIVING_CMD1;
			}
			break;
		case RECEIVING_CMD1:
			if(c == '$')
				recv_cnt1 = 1;
			else{
				gc_recv_buf1[recv_cnt1++] = c;
			}
			//if(c == '\n')
			if(c == ';'){

				pckt_recvd = 1;
				/*		sscanf(gc_recv_buf1,"$B%c,%04d,%04d%;",
						&solar_status,
						&sc_bat1,
						&sc_bat2);
				uart_recv_state1 = WAITING_FOR_STX1;*/
				//uart_recv_state1 = CMD_RECVD1;
			}
			if(recv_cnt1 >= RBUF_SIZE)
				uart_recv_state1 = WAITING_FOR_STX1;
			break;
		case CMD_RECVD1:
			break;
		default:
			break;
		}
	}

}


int main(void)
{

	int i, j, k;
	int last_rec_min;
	char rec_data;
	char lpc845_ver;
	// uint32_t sctimerClock;

	//	BOARD_BootClockFRO30M();


	SystemCoreClockUpdate();
	SysTick_Config(SystemCoreClock / TICKRATE_HZ);

	tc0_2ms = TC0_MAX_2MS_UART;
	tc1_5ms = TC1_MAX_5MS_RF;
	tc2_1s  = TC2_MAX_1S;
	tc3_100ms = TC3_MAX_100MS;
	tc4_10s  = TC4_MAX_10S;


	timer0_ovf_2ms_tx_en = 1;
	timer1_ovf_5ms = 0;
	timer2_ovf_uart = 0;
	timer3_ovf_100ms = 0;
	timer4_ovf_10s = 0;

	ws_sec_cnt = 0;

	rfpin_oldstate = 1;
	rfpin_prevstate = 1;
	rfpin_newstate = 1;
	rfpin_debouncecnt = 0;
	rf_val = 0;
	calib_rf = 0;
	rf_local = 0;
	calib_mode = 0;

	gi_rf_15min = 0;
	gi_last_rec_15min_rf = 0;

	gi_cur_blk = 0;
	gi_get_rec = 0;
	gi_rec_1min_rf = 0;
	gi_rec_15min_rf = 0;
	gi_rec_blk_no = 0;
	gi_rec_blk_found = 0;
	cur_offset_1min = 0;
	cur_offset_15min = 0;
	last_rec_min = -1;
	calib_timeout_min = 0;
	user_intr = 0;
	time_since_reset_interval = 0;

	Present_Date_Time.century = 20;
	Last_Rec_Date_Time.century = 20;

	uart_recv_state = WAITING_FOR_STX;
	uart_recv_state1 = WAITING_FOR_STX1;

	/* Select the main clock as source clock of USART0. */
	CLOCK_Select(kUART0_Clk_From_MainClk);
	CLOCK_Select(kI2C0_Clk_From_MainClk);
	CLOCK_Select(kI2C1_Clk_From_MainClk);
	CLOCK_Select(kUART1_Clk_From_MainClk);


	Pin_init();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	Init_UART_I2C_ADC_Pins();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	/* Initialize USART with configuration. */
	USART0_Init();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	USART1_Init();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	ADC_ClockPower_Configuration();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	//	ADC_Configuration();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	I2C0_init();
	for(i=0;i<100;i++)
		for(j=0;j<1000;j++);

	init_ext_rtc();
	for(i=0;i<1000;i++)
		for(j=0;j<2000;j++);


	//Wind counter and temperature/humidity bus. Always on: this firmware serves TRG and TWS alike,
	//and the LPC1768 build uses what its profile needs. PIO0_19 feeds both the wind and rain counters.
	init_counter_ws();
	for(i=0;i<1000;i++)
		for(j=0;j<2000;j++);


	I2C1_init();
	for(i=0;i<1000;i++)
		for(j=0;j<2000;j++);


	//	read_th_val();
	for(i=0;i<1000;i++)
		for(j=0;j<2000;j++);
	//for DMC
	bat2_adc = 0;

	inst_ws = 0;
	avg_ws = 0;

	last_rec_temperature = 0;
	last_rec_humid = 0;
	last_rec_ws = 0;
	last_rec_ws_sec = 0;
	last_rec_wd = 0;
	inst_ws = 0;
	avg_ws = 0;
	ws_sec_cnt = 0;
	last_rf = 0;
	rain_day_tips = 0;
	rain_total_restored = 0;
	wake_key_low_cnt = 0;
	wake_key_pressed = 0;
	sleep_holdoff_100ms = 0;
	wake_reason = 'B';
	reset_status = SYSCON->SYSRSTSTAT;
	SYSCON->SYSRSTSTAT = reset_status;	//writing the bits back clears them for the next restart
	pckt_recvd = 0;

	GPIO_PortClear(GPIO, LPC845_PORT, 1u << LPC1768_PWR_STDN);
	GPIO_PortClear(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN);
	GPIO_PortClear(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN);

	lpc845_ver = LPC845_VERSION;
	solar_status = 'N';
	sc_bat1 = 0;
	sc_bat2 = 0;

	/*	Set_Date_Time.year = 20;
	Set_Date_Time.month = 12;
	Set_Date_Time.date = 2;
	Set_Date_Time.hour = 15;
	Set_Date_Time.minute = 21;

	Set_Time();
	 */

	/* Init output LED GPIO. */
	//	GPIO_PortInit(GPIO, LPC845_PORT);
	//	GPIO_PinInit(GPIO, LPC845_PORT, LED_PIN, &gpio_output_config);

	//	GPIO_PortToggle(GPIO, LED_PORT, 1u << LED_PIN);
	//GPIO_PortSet(GPIO_Type *base, uint32_t port, uint32_t mask);
	//GPIO_PortClear(GPIO_Type *base, uint32_t port, uint32_t mask)

	/*   sctimer_config_t sctimerInfo;
    sctimerClock = SCTIMER_CLK_FREQ;
    SCTIMER_GetDefaultConfig(&sctimerInfo);
    SCTIMER_Init(SCT0, &sctimerInfo);
	 */


	Get_Time();
	last_rec_min = Present_Date_Time.minute;

	fw845_app_init();


	for(i=0;i<2000;i++)
		for(j=0;j<2000;j++);


	while (1)
	{

		fw845_app_feed_watchdog();

		//Only a wake-up key while the LPC1768 is powered down. While it is running, COL2 also
		//goes low on its normal keypad scan, which must not switch the rails.
		if(wake_key_pressed && (sleep_holdoff_100ms == 0) && GPIO_PinRead(GPIO, LPC845_PORT, LPC1768_PWR_STDN)){
			time_since_reset_interval = 0;
			wake_reason = 'K';
			GPIO_PortClear(GPIO, LPC845_PORT, 1u << LPC1768_PWR_STDN);//wake up LPC
			GPIO_PortClear(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN);//wake up communication module
			GPIO_PortClear(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN);//wake up lcd
			//	GPIO_PortSet(GPIO, LPC845_PORT, 1u << UI_ROW_PIN);//high row pin

			row2_input_configuration();

		}


		if(pckt_recvd == 1){

			pckt_recvd = 0;
		/*	rf_local =	(UART_RECV_Buffer[2]-'0')*1000+
						(UART_RECV_Buffer[3]-'0')*100+
						(UART_RECV_Buffer[4]-'0')*10+
						(UART_RECV_Buffer[5]-'0');*/

			//$BN1111,2222;

		//	gc_recv_buf1[3] = solar_status;

			solar_status = gc_recv_buf1[2];
			sc_bat1 =	(gc_recv_buf1[4]-'0')*1000+
									(gc_recv_buf1[5]-'0')*100+
									(gc_recv_buf1[6]-'0')*10+
									(gc_recv_buf1[7]-'0');

			sc_bat2 =	(gc_recv_buf1[9]-'0')*1000+
									(gc_recv_buf1[10]-'0')*100+
									(gc_recv_buf1[11]-'0')*10+
									(gc_recv_buf1[12]-'0');


		/*	sscanf(gc_recv_buf1,"$B%c,%04d,%04d%;",
					&solar_status,
					&sc_bat1,
					&sc_bat2);*/

			uart_recv_state1 = WAITING_FOR_STX1;

		}
		//get status of gprs & lcd
		//g -> GPRS_OFF		G->GPRS_ON
		//u -> USB_OFF		U->USB_ON

		gprs_power = ((GPIO_PinRead(GPIO, LPC845_PORT, SIM7600_PWR_STDN))?'g':'G');
		usb_power = ((GPIO_PinRead(GPIO, LPC845_PORT, LCD_PWR_STDN))?'u':'U');

		if(uart_recv_state == CMD_RECVD){
			decode_command();
		}


		if(timer3_ovf_100ms == 1){
			//Hang timer only runs while the LPC1768 is powered: asleep is the normal state
			if(GPIO_PinRead(GPIO, LPC845_PORT, LPC1768_PWR_STDN)){
				time_since_reset_interval = 0;
			}
			else{
				time_since_reset_interval += 100;
			}
			if(sleep_holdoff_100ms > 0){
				sleep_holdoff_100ms--;
			}
			Get_Time();
			timer3_ovf_100ms = 0;
		}

		if(timer4_ovf_10s == 1){
			read_th_val();
			for(i=0;i<100;i++)
				for(j=0;j<200;j++);

			read_adc2_values();
			for(i=0;i<100;i++)
				for(j=0;j<200;j++);

			read_adc3_values();
			for(i=0;i<100;i++)
				for(j=0;j<200;j++);

			timer4_ovf_10s = 0;

		}


		if(last_rec_min != Present_Date_Time.minute){

			//
			if((Present_Date_Time.minute % 15 ) == 0){

				store_data();

				time_since_reset_interval = 0;

				//Power on LPC1768 and GPRS. Keep LCD 5V OFF for an unattended wake, but
				//leave it alone if the LPC1768 is already running (operator may be using it)
				if(GPIO_PinRead(GPIO, LPC845_PORT, LPC1768_PWR_STDN)){
					GPIO_PortSet(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN);
					wake_reason = 'S';
				}
				GPIO_PortClear(GPIO, LPC845_PORT, 1u << LPC1768_PWR_STDN);
				GPIO_PortClear(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN);
				row2_input_configuration();


				present_avg_ws = 0;
				present_ws_sec_cnt = 0;


			}

			last_rec_min = Present_Date_Time.minute;

		}

		//if more than 11 minutes no heartbeat from LPC1768, LPC845 forcefully cuts all power.
		//The next 15-minute slot or DSP key powers it back up.
		if(time_since_reset_interval > (11*60*1000)){

			time_since_reset_interval = 0;
			GPIO_PortSet(GPIO, LPC845_PORT, 1u << LPC1768_PWR_STDN);
			GPIO_PortSet(GPIO, LPC845_PORT, 1u << SIM7600_PWR_STDN);
			GPIO_PortSet(GPIO, LPC845_PORT, 1u << LCD_PWR_STDN);

			row2_output_configuration();

			avg_ws = 0;
			ws_sec_cnt = 0;

		}



		if(calib_mode){
			if(calib_timeout_min == Present_Date_Time.minute)
				calib_mode = 0;
		}


		if(timer2_ovf_uart == 1){		//1 sec loop

			//	read_pulse_counts_in_1sec();

			timer2_ovf_uart = 0;

			if(calib_mode == 1){

				wait_tx_idle();
				sprintf(gc_xmit_buf, "$C%02d%02d%02d%02d%02d%02d%02d,%04d,%04d,%04d;",
						Present_Date_Time.century,
						Present_Date_Time.year,
						Present_Date_Time.month,
						Present_Date_Time.date,
						Present_Date_Time.hour,
						Present_Date_Time.minute,
						Present_Date_Time.sec,
						calib_rf,
						bat1_adc,
						bat2_adc);

				xmit_ptr = gc_xmit_buf;
				LPC845_UART0->INTENSET = kUSART_TxReady;
				pAck_xmit = 1;

				for(i=0;i<200;i++)
					for(j=0;j<300;j++);
			}

			//	uint16_t last_rf;
			//	float last_rec_temperature, last_rec_humid, last_rec_ws, last_rec_wd, inst_ws;

			//sprintf(gc_xmit_buf, "$D%04d;", rf_val);
			wait_tx_idle();
			sprintf(gc_xmit_buf, "$D%02d%02d%02d%02d%02d%02d%02d,%02d%02d%02d%02d%02d%02d%02d,%04d,%04d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%c,%c,%c,%04d,%04d,%c,%d,%d,%c,%d,%d,%d;",
					Present_Date_Time.century,
					Present_Date_Time.year,
					Present_Date_Time.month,
					Present_Date_Time.date,
					Present_Date_Time.hour,
					Present_Date_Time.minute,
					Present_Date_Time.sec,
					Last_Rec_Date_Time.century,
					Last_Rec_Date_Time.year,
					Last_Rec_Date_Time.month,
					Last_Rec_Date_Time.date,
					Last_Rec_Date_Time.hour,
					Last_Rec_Date_Time.minute,
					Last_Rec_Date_Time.sec,
					last_rf,
					bat1_adc,
					last_rec_temperature,
					last_rec_humid,
					last_rec_ws,
					last_rec_wd,
					inst_ws,
					last_rec_ws_sec,
					present_temperature,
					present_humid,
					present_avg_ws,
					present_wd,
					present_ws_sec_cnt,
					gprs_power,
					usb_power,
					lpc845_ver,
					sc_bat1,
					sc_bat2,
					solar_status,
					(int)rain_day_tips,
					LPC845_FW_TYPE,
					wake_reason,
					(int)reset_status,
					(int)rf_val,	//Tips so far in this slot, for the LCD
					(int)rain_total_restored);

			//XOR of everything between '$' and ';', sent as a last field: the LPC1768 drops a frame
			//that doesn't match, as a lost byte could otherwise move digits between fields
			{
				int frame_len = (int)strlen(gc_xmit_buf);
				uint8_t frame_xor = 0;
				for(int k = 1; k < frame_len - 1; k++){
					frame_xor ^= (uint8_t)gc_xmit_buf[k];
				}
				sprintf(&gc_xmit_buf[frame_len - 1], ",%02X;", frame_xor);
			}

			xmit_ptr = gc_xmit_buf;
			LPC845_UART0->INTENSET = kUSART_TxReady;
			pAck_xmit = 1;

			for(i=0;i<200;i++)
				for(j=0;j<110;j++);
			//			PRINTF("%d:%d\r\n",Present_Date_Time.minute, Present_Date_Time.sec);
		}


		//gi_get_rec flag is enabled when request from LPC1768 for data
		//Getting recorded data from structure and send it to LPC1765
		if (gi_get_rec == 1){
			wait_tx_idle();
			gi_rec_blk_no = get_cur_blk();		//for getting the block_number

			if(gi_rec_blk_found == 1){

				rec_data = 'Y';

				cur_offset_1min = (Get_Data_For_Date_Time.hour*60 + Get_Data_For_Date_Time.minute);
				cur_offset_15min = (((Get_Data_For_Date_Time.hour*60 + Get_Data_For_Date_Time.minute)/REC_INTERVAL) + RAIN_INDEX_SHIFT) % LOG_INTERVAL_15MIN_RF;

				//		gi_rec_1min_rf = Day_Samples[gi_rec_blk_no].rf_1min[cur_offset_1min];
				gi_rec_15min_rf = Day_Samples[gi_rec_blk_no].rf_15min[cur_offset_15min];
				gi_rec_bat1_adc= Day_Samples[gi_rec_blk_no].bat_adc_15min[cur_offset_15min];
				//		gi_rec_bat2_adc= Day_Samples[gi_rec_blk_no].adc2_15min[cur_offset_15min];

				//Rain-day total up to and including this sample
				uint32_t rec_cum_tips = 0;
				for(i=0; i<=cur_offset_15min; i++){
					if(Day_Samples[gi_rec_blk_no].rf_15min[i] != 0xFFFF){
						rec_cum_tips += Day_Samples[gi_rec_blk_no].rf_15min[i];
					}
				}


				for(i=0; i<80;i++){
					gc_xmit_buf[i] = 0;
				}

				if(gi_rec_15min_rf != 0xFFFF){
					//sprintf(gc_xmit_buf, "$R%04d;", gi_rec_15min_rf);
					//sprintf(gc_xmit_buf, "$R%04d;", gi_rec_15min_rf);
					sprintf(gc_xmit_buf, "$G%c,%02d%02d%02d%02d%02d%02d%02d,%02d%02d%02d%02d%02d%02d%02d,%d,%d,%d,%d,%d,%d,%d,%d;",
							rec_data,
							Present_Date_Time.century,
							Present_Date_Time.year,
							Present_Date_Time.month,
							Present_Date_Time.date,
							Present_Date_Time.hour,
							Present_Date_Time.minute,
							Present_Date_Time.sec,
							Get_Data_For_Date_Time.century,
							Get_Data_For_Date_Time.year,
							Get_Data_For_Date_Time.month,
							Get_Data_For_Date_Time.date,
							Get_Data_For_Date_Time.hour,
							Get_Data_For_Date_Time.minute,
							Get_Data_For_Date_Time.sec,
							gi_rec_15min_rf,
							gi_rec_bat1_adc,
							(int)Day_Samples[gi_rec_blk_no].temperature[cur_offset_15min],
							(int)Day_Samples[gi_rec_blk_no].humidity[cur_offset_15min],
							(int)Day_Samples[gi_rec_blk_no].ws[cur_offset_15min],
							(int)Day_Samples[gi_rec_blk_no].wd[cur_offset_15min],
							(int)Day_Samples[gi_rec_blk_no].ws_sec[cur_offset_15min],
							(int)rec_cum_tips);

					xmit_ptr = gc_xmit_buf;
					LPC845_UART0->INTENSET = kUSART_TxReady;
					pAck_xmit = 1;

					for(i=0;i<500;i++)
						for(j=0;j<600;j++);
				}

				else if((gi_rec_blk_found != 1)  || (gi_rec_15min_rf == 0xFFFF)){

					rec_data = 'N';

					sprintf(gc_xmit_buf, "$G%c,%02d%02d%02d%02d%02d%02d%02d,%02d%02d%02d%02d%02d%02d%02d;",
							rec_data,
							Present_Date_Time.century,
							Present_Date_Time.year,
							Present_Date_Time.month,
							Present_Date_Time.date,
							Present_Date_Time.hour,
							Present_Date_Time.minute,
							Present_Date_Time.sec,
							Get_Data_For_Date_Time.century,
							Get_Data_For_Date_Time.year,
							Get_Data_For_Date_Time.month,
							Get_Data_For_Date_Time.date,
							Get_Data_For_Date_Time.hour,
							Get_Data_For_Date_Time.minute,
							Get_Data_For_Date_Time.sec);

					xmit_ptr = gc_xmit_buf;
					LPC845_UART0->INTENSET = kUSART_TxReady;
					pAck_xmit = 1;

					for(i=0;i<500;i++)
						for(j=0;j<600;j++);

				}

				for(i=0;i<500;i++)
					for(j=0;j<500;j++);
			}
			else{
				//This rain day is no longer held (or the LPC845 lost power)
				rec_data = 'N';

				sprintf(gc_xmit_buf, "$G%c,%02d%02d%02d%02d%02d%02d%02d,%02d%02d%02d%02d%02d%02d%02d;",
						rec_data,
						Present_Date_Time.century,
						Present_Date_Time.year,
						Present_Date_Time.month,
						Present_Date_Time.date,
						Present_Date_Time.hour,
						Present_Date_Time.minute,
						Present_Date_Time.sec,
						Get_Data_For_Date_Time.century,
						Get_Data_For_Date_Time.year,
						Get_Data_For_Date_Time.month,
						Get_Data_For_Date_Time.date,
						Get_Data_For_Date_Time.hour,
						Get_Data_For_Date_Time.minute,
						Get_Data_For_Date_Time.sec);

				xmit_ptr = gc_xmit_buf;
				LPC845_UART0->INTENSET = kUSART_TxReady;
				pAck_xmit = 1;
			}

			gi_get_rec = 0;

		}

	}
}
