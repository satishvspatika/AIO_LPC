/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define LPC845_PORT 0U

//One firmware for every station type: it always measures rain and the weather sensors, and the
//LPC1768 build uses what its profile needs. Reported in the status frame.
#define LPC845_FW_TYPE 2
//#define LED_PIN  14U

#define LPC845_VERSION 'b'

#define TICKRATE_HZ    (1000)	/* 1000 ticks per second */
#define TC0_MAX_1S 2000U

#define TC0_MAX_2MS_UART  2		/* every 2 ms, uart tx disable after transmission */
#define TC1_MAX_5MS_RF	  5		/* every 5 ms, debounce rf pin */
//#define TC2_MAX_1S		  1000    /* every 1000 ms, uart transmission for test */
#define TC2_MAX_1S		  1000    /* every 1000 ms, uart transmission for test */
#define TC3_MAX_100MS	  100		/* every 10 ms*/

//#define TC4_MAX_10S		  10000
#define TC4_MAX_10S		  5000

#define MAX_RF_DEBCNT 	2
#define RF_PIN		   19		//Rain gauge on RAINFALL_PC. Also the wind counter input (SCT): a TWS
							//unit's tip count is wind pulses, which its LPC1768 build ignores
#define RF_PIN_ALT	   14

#define UI_ROW_PIN  1U
#define UI_COL_PIN  15U

#define LPC1768_PWR_STDN 27U
#define SIM7600_PWR_STDN 26U
#define LCD_PWR_STDN 28U

#define USER_ACTIVE   0		//UI state
#define WAKE_KEY_SAMPLES   6		//DSP must read low for 6 x 5 ms before it counts as a press
#define SLEEP_HOLDOFF_100MS 100	//ignore COL2 for 10 s after power down, while the 3.3V rail of
								//the LPC1768 (whose P0.20 shares COL2) is still decaying


#define LPC845_UART0 USART0
#define LPC845_UART0_CLK_SRC kCLOCK_MainClk
#define LPC845_UART0_CLK_FREQ CLOCK_GetFreq(LPC845_UART0_CLK_SRC)
#define LPC845_UART0_IRQn USART0_IRQn
#define LPC845_UART1 USART1
#define LPC845_UART1_CLK_SRC kCLOCK_MainClk
#define LPC845_UART1_CLK_FREQ CLOCK_GetFreq(LPC845_UART1_CLK_SRC)
#define LPC845_UART1_IRQn USART1_IRQn
//#define LPC845_UARTHNDLR USART0_IRQHandler

#define RBUF_SIZE     	120
#define TBUF_SIZE       176

#define MAX_DAY_BLOCKS_RF 	 	   3
#define LOG_INTERVAL_1MIN_RF 	   1440
#define LOG_INTERVAL_15MIN_RF 		96
#define REC_INTERVAL		 		15

//A rain day runs from the 08:45 slot to the 08:30 slot next day, labelled by its closing date
#define RAIN_CLOSE_SLOT				34		//slot of the calendar day for 08:30
#define RAIN_INDEX_SHIFT			61		//calendar slot 35 (08:45) becomes rain-day sample 0


#define IDT_DEV_ADDR		0x68

#define I2C_MASTER_BASE (I2C0_BASE)
#define I2C_MASTER_CLOCK_FREQUENCY (12000000)
#define I2C_MASTER ((I2C_Type *)I2C_MASTER_BASE)
#define I2C_BAUDRATE 100000U


#define HDC_DEV_ADDR    0x40
#define WD_ADDR    0x36


#define I2C_MASTER_BASE1 (I2C1_BASE)
#define I2C_MASTER_CLOCK_FREQUENCY1 (12000000)
#define I2C_MASTER1 ((I2C_Type *)I2C_MASTER_BASE1)
#define I2C_BAUDRATE1 100000U


#define ADC_BASE ADC0
#define ADC_SAMPLE_CHANNEL_NUMBER9 9U
#define ADC_SAMPLE_CHANNEL_NUMBER8 8U
#define ADC_SAMPLE_CHANNEL_NUMBER5 5U

#define ADC_SAMPLE_CHANNEL_NUMBER0 9U

#define ADC_CLOCK_SOURCE kCLOCK_Fro
//#define ADC_CLOCK_SOURCE kCLOCK_MainClk

#define ADC_CLOCK_DIVIDER 3U
#define PRINTF printf
#define SCANF scanf

#define SCT_BASE SCT0



/*******************************************************************************
 * Variables
 ******************************************************************************/

volatile uint32_t tc0_2ms;
volatile uint32_t tc1_5ms;
volatile uint32_t tc2_1s;
volatile uint32_t tc3_100ms;
volatile uint32_t tc4_10s;

volatile int timer0_ovf_2ms_tx_en, timer1_ovf_5ms, timer2_ovf_uart, timer3_ovf_100ms, timer4_ovf_10s;

volatile int rfpin_oldstate, rfpin_prevstate, rfpin_newstate, rfpin_debouncecnt, rf_val, calib_rf, rf_local;




enum {WAITING_FOR_STX, RECEIVING_CMD, CMD_RECVD};
enum {WAITING_FOR_STX1, RECEIVING_CMD1, CMD_RECVD1};

char gc_recv_buf[RBUF_SIZE];
char gc_xmit_buf[TBUF_SIZE];
char gc_recv_buf1[32];

char *xmit_ptr, *xmit_ptr1;
volatile int pAck, pAck_xmit, pAck_xmit1;
volatile int pckt_recvd;


int time_since_reset_interval;

volatile int uart_recv_state, uart_recv_state1;
int recv_cnt, recv_cnt1;

volatile int calib_mode;
int calib_timeout_min;
int user_intr;
uint8_t i2c_clkval[40];
char i2c_clkval1[6];

int bat1_adc, bat2_adc, solar_adc;
char gprs_power, usb_power;

//float inst_ws;
float inst_ws1, avg_ws1;

uint8_t gi_cur_blk;
uint8_t gi_rf_15min;
uint8_t gi_get_rec;
uint8_t gi_rec_1min_rf;
uint16_t gi_rec_15min_rf;
uint16_t gi_rec_bat1_adc;
uint16_t gi_rec_bat2_adc;
uint8_t gi_rec_blk_no;
uint8_t gi_rec_blk_found;

uint16_t gi_last_rec_15min_rf;

uint16_t sc_bat1, sc_bat2;

char solar_status;
uint16_t last_rf;
uint32_t rain_day_tips;		//rainfall so far in this rain day
volatile uint8_t rain_total_restored;	//0 after a restart, until the LPC1768 has returned the stored total
volatile int wake_key_low_cnt;
volatile int wake_key_pressed;
int sleep_holdoff_100ms;
char wake_reason;			//why the LPC1768 was last powered: B=this chip restarted, K=DSP key, S=15-minute slot
uint32_t reset_status;		//why this chip last restarted (SYSRSTSTAT: 1=power on, 2=reset pin, 4=watchdog, 8=brown-out, 16=software)
//float last_rec_temperature, last_rec_humid, last_rec_ws, last_rec_wd, inst_ws;

uint32_t last_rec_temperature, last_rec_humid, last_rec_ws, last_rec_ws_sec, last_rec_wd, inst_ws, avg_ws, ws_sec_cnt;

uint32_t present_temperature, present_humid, present_ws, present_ws_sec, present_wd, inst_ws, present_avg_ws, present_ws_sec_cnt;



int cur_offset_1min, cur_offset_15min;

typedef struct{
	uint8_t century;
	uint8_t year;
	uint8_t month;
	uint8_t date;
	uint8_t hour;
	uint8_t minute;
	uint8_t sec;
}xDateTime_def;

xDateTime_def Present_Date_Time;
xDateTime_def Set_Date_Time;
xDateTime_def Get_Data_For_Date_Time;
xDateTime_def Last_Rec_Date_Time;

xDateTime_def next_date;
xDateTime_def rtc_read_date;
xDateTime_def rtc_read_date_copy;

/*
typedef struct{
	uint8_t cc;		// century
	uint8_t yy;		// year
	uint8_t mm;		// month
	uint8_t dd;		// date
	uint8_t rf_1min[LOG_INTERVAL_1MIN_RF];
	uint16_t rf_15min[LOG_INTERVAL_15MIN_RF];
	uint16_t adc1_15min[LOG_INTERVAL_15MIN_RF];
	uint16_t adc2_15min[LOG_INTERVAL_15MIN_RF];
} xDay_Samples_def;
xDay_Samples_def Day_Samples[MAX_DAY_BLOCKS_RF];
*/


typedef struct{
	uint8_t cc;		// century
	uint8_t yy;		// year
	uint8_t mm;		// month
	uint8_t dd;		// date
//	uint8_t rf_1min[LOG_INTERVAL_1MIN_RF];
	uint16_t rf_15min[LOG_INTERVAL_15MIN_RF];
	uint16_t bat_adc_15min[LOG_INTERVAL_15MIN_RF];

	uint16_t wd[LOG_INTERVAL_15MIN_RF];
	uint32_t temperature[LOG_INTERVAL_15MIN_RF];
	uint32_t humidity[LOG_INTERVAL_15MIN_RF];
	uint32_t ws[LOG_INTERVAL_15MIN_RF];
	uint32_t ws_sec[LOG_INTERVAL_15MIN_RF];


} xDay_Samples_def;
xDay_Samples_def Day_Samples[MAX_DAY_BLOCKS_RF];


//////////////////////////////////////////
// CONFIG register bit-field shifters
#define UNIFY       0
#define CLKMODE     1
#define CKSEL       3
#define NORELOAD_L  7
#define NORELOAD_H  8
#define INSYNC      9
#define AUTOLIMIT_L 17
#define AUTOLIMIT_H 18


// CONFIG register values
#define Bus_clock           0
#define Prescaled_bus_clock 1
#define Input               2

#define In0re               0
#define In0fe               1
#define In1re               2
#define In1fe               3
#define In2re               4
#define In2fe               5
#define In3re               6
#define In3fe               7

// CTRL register bit-field shifters
#define DOWN_L   0
#define Stop_L   1
#define Halt_L   2
#define CLRCTR_L 3
#define BIDIR_L  4
#define PRE_L    5
#define DOWN_H   16
#define Stop_H   17
#define Halt_H   18
#define CLRCTR_H 19
#define BIDIR_H  20
#define PRE_H    21


#define SWM       (1<<7)
#define SCT       (1<<8)

#define SCT0_RST_N    ~(1<<8)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

void Init_UART_I2C_ADC_Pins(void);
void debounce_rfpin(void);
void USART0_Init(void);
void USART1_Init(void);

void I2C0_Init(void);
void Init_Pins(void);
void uart0_isr(void);
void decode_command(void);
void store_data();
int get_cur_blk();

void init_ext_rtc(void);
//void Get_Time(xDateTime_def *xDateTime);
void Set_Time(void);
//static void ADC_Configuration(void);
void ADC_ClockPower_Configuration(void);


void read_adc1_values(void);
void read_adc2_values(void);
void read_adc3_values(void);

//static void ADC_Configuration1(void);
static void ADC_Configuration2(void);
static void ADC_Configuration3(void);

void row2_output_configuration();
void row2_input_configuration();


void read_th_val(void);
void I2C1_init(void);

void read_wd_val(void);

void read_pulse_counts_in_1sec(void);
void init_counter_ws(void);


void Next_Date(uint8_t *Nd, uint8_t *Nm, uint8_t *Ny);
void Get_Time();
void Get_Cur_Date_Time(xDateTime_def *DateTime);


