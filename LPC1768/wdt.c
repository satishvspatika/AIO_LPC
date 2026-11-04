#include "globals.h"
#include "FreeRTOS.h"
#include "task.h"
#include "kbd.h"
#include "lcd.h"
#include "ui.h"
#include "lpc17xx_wdt.h"
#include "wdt.h"


/*-------------------------------------------------------------------------------------------------------------------------------------------*
* 	FUNCTION NAME : vWdt_Task
 * 	DESCRIPTION :
			•	WDT – Watchdog Timer. This Task is mainly used for Resetting the LPC
			•	Every now and then we are checking following tasks.
			o	GPRS
			o	DISK
			o	LOG
			o	RTC
			o	UI
			o	I2C
			•	If any one of the Task is not running properly, then WDT Task will reset LPC.
	Main Flow:
		1.	Wait 10 seconds before start the Task
		2.	Initialize WDT with proper register values.
		3.	Start the WDT Activity with given timeout value
		4.	If all the tasks are working properly call WDT_Feed function under critical region.
		5.	If WDT is not feed properly, then it will gets timeout and reset will happen
		Functions:
		1.	WDT_Init: This function is used to initialize the WDT
		2.	WDT_Start: this function Is used to start the WDT with given timeout
		3.	WDT_Feed: This function is used to check WDT is working or not. If WDT feeding is working properly then we can say all other tasks are working properly otherwise WDT will initiate reset condition.

* 	VERSION : 1.5
* 	ARGUMENTS : pvParameters
* 	RETURN VALUES : None
* 	GLOBALS INPUT : None
*	GLOBALS AFFECTED : iUi_State, iUi_Step, iUi_State, XITC_Ui_Lcd.disp_str
*	CALLED FUNCTION : main
*	CALLING FUNCTION : xUi_Steps
*

/*----------------------------------------------------------------------------------------------------------------------------------------------*/

void vWDT_Task(void *pvParameters)
{

	vTaskDelay(10000 / portTICK_RATE_MS);
	WDT_Init((WDT_CLK_OPT)WDT_CLKSRC_IRC, (WDT_MODE_OPT) WDT_MODE_RESET);
	WDT_Start(18*60*1000000); // 18 mins

	for(;;){
		vTaskDelay(10000 / portTICK_RATE_MS);
		if(((gi_wdt_task_ok_stat & WDT_MASK) == WDT_MASK) && (gi_restart_flag!=1)) {
			taskENTER_CRITICAL();
			WDT_Feed();
			gi_wdt_task_ok_stat = 0;
			taskEXIT_CRITICAL();
		}
	}
}
