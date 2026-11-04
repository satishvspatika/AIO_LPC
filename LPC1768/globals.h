#include <stdint.h>
#include "user_config.h"

#define DIR_IN  	0
#define DIR_OUT 	1

#define local_write_buf_st 0x2007f000
#define MAX_NO_OF_ISPs 5

#define MAX_NO_OF_DAYS 7
#define ST_DATA_SECTOR 2
#define END_DATA_SECTOR 8

#define LOG_INTERVAL 15
#define REPORT_INTERVAL 15

#define CLOSING_HR 8
#define CLOSING_MIN 30

#define LINE_FEED	0x0A
#define CARRIAGE_RETN	0x0D

//char write_buf[512];

enum {primary_sim, secondary_sim};

int sim_select;
//int write_buf[512];

int next_sect, cur_sect, prev_sect, old_sect, recent_sect;

int c_rainfall;
int u_rainfall;


int ftp_line_cnt;
int ftp_rec_no;
int ftp_flag[20];


#define WDT_DISK_OK 	1
#define WDT_SENSOR_OK	2
#define WDT_MEMORY_OK	4
#define WDT_UI_OK		8
#define WDT_GPRS_OK 	16


#define WDT_MASK (WDT_DISK_OK | WDT_SENSOR_OK | WDT_MEMORY_OK  | WDT_UI_OK | WDT_GPRS_OK )



//FLAGS

int auto_time_set;
int firmware_updated;
int file_saved;
int log_task_started;
int memory_task_started;
int sensor_task_started;
int disk_task_started;
int kbd_task_started;
int ui_task_started;
int wdt_task_started;
int lcd_task_started;

int set_date_time;
char gc_station_id1[8];

int log_interval_ovf;
int report_interval_ovf;
int gi_disk_ok;
int gi_recv_pkt_ready;
int gi_save_params_in_flash;
int data_not_stored_in_flash;
int gi_data_ready_for_report;
int gi_manual_http_trigger;
int gprs_modem_active;
int gi_http_in_progress;
int update_present_day_flag;
int update_previous_day_flag;
int sleep_mode;

int check_prev_day_ftp;

int gi_sim_state, gi_sim_reg, gi_http_error;
int gUi_Active;
volatile int shutdown_pwr;
int erase_data_sector;
char gprs_pwr, usb_lcd_pwr;

int gi_wdt_task_ok_stat;

int gi_update_firmware;
int gi_update_firmware1;
int firmware_copied;
int del_fw_file;
int gi_load_params;
int gi_restart_flag;

int gi_start_dota;
int gi_start_dota_lpc824;
int auto_dump_flag;
int gi_gprs_module_ready;

int gi_sig_timeout;
int calib_on;
int calib_rf;
//int ui_crf;

//FLAGS


//int bat_vol;

//int rainfall;
//int gi_rf_before_calib;

int gi_signal_level;
float bat_volt;

float gf_bat_volt1;
float gf_bat_volt2;

char lpc845_ver;

int gi_log_date, gi_log_month, gi_log_year, gi_log_hour, gi_log_minute;
int gi_present_error, gi_net_error;

char gc_dota_file_name[32];
int auto_dump_date, auto_dump_month, auto_dump_year;
float bat_volt1, bat_volt2;

int present_sector_no;
int fill_data_in_flash;

int present_sample_no;
int set_time_to_rtc;
int store_flags_in_flash;
uint8_t store_in_disk, http_sent, ftp_sent;

int gps_found;
int http_status;
int ftp_status;
char dump_buf[200];

char gc_version_sdc[16];

int h_date, h_month, h_year, h_hour, h_minute;

int send_msg_data;
int get_gps;
int wait_for_sms_process;

char msg_data[40];

int latest_Sector, oldest_Sector;

int store_ftp_sdc;
char ftp_rec[64];
float f_bat_volt1, f_bat_volt2;
int ftp_rf;
int ftp_in_sdc;
float f_temp, f_humid, f_ws;
int f_wd;

///////////temp_var








/////////////////////////////////


char gc_apn_str[20], gc_apn_user_name[16], gc_apn_user_pwd[16];
char gc_operator_name[16];
//int oldPosn, recentPosn;
char gc_ftp_put_file[64];


typedef struct{
	int date;
	int month;
	int year;
	int status;
} Cailb_Status_Details_def;

Cailb_Status_Details_def Cailb_Status_Details;

typedef struct{
	int year;
	int month;
	int date;
	int hour;
	int minute;
	int second;
}xDateTime_def;

xDateTime_def xPresent_Date_Time;
xDateTime_def xSet_Date_Time;
xDateTime_def xLast_Rec_15min_Date_Time;
xDateTime_def xGet_15min_Date_Time;

uint16_t last_rec_15min_rf;
uint16_t last_rec_15min_bat1;
uint16_t last_rec_15min_bat2;

uint16_t temp_bat_adc;

char solar_status;

uint16_t last_rec_temperature, last_rec_humid, last_rec_ws, last_rec_ws_sec, last_rec_wd, inst_ws;
uint16_t present_temperature, present_humid, present_ws, present_wd, present_ws_sec_cnt;

uint16_t ws_ce, ws_sec_ce;

typedef struct  {
	char svc[16];
	char apn[20];
	char apn_user[16];
	char apn_pwd[16];
	char mobile_network[16];
}ISP_APN_def;

typedef struct  {
	float latitude;
	char lat_dir;
	float longitude;
	char lon_dir;
	float altitude;
	int date;
	int month;
	int year;
	int hour;
	int min;
	int sec;
}GPS_def;


typedef struct{

	char gc_station_id[8];
	char gc_station_name[32];
	char gc_model[16];

	char gc_network_name[16];
	char gc_station_type[16];

	char gc_version[16];

	char gc_http_server_name1[32];
	char gc_http_server_name2[32];
	char gc_http_link1[50];
	char gc_http_link2[50];
	char gc_http_key1[16];
	char gc_http_key2[16];
	int gi_http_server_port1;
	int gi_http_server_port2;

	char gc_ftp_server_name1[32];
	char gc_ftp_server_name2[32];
	char gc_ftp_user1[32];
	char gc_ftp_user2[32];
	char gc_ftp_pwd1[32];
	char gc_ftp_pwd2[32];
	char gc_ftp_path1[32];
	char gc_ftp_path2[32];
	int gi_ftpserver_port1;
	int gi_ftpserver_port2;

	char gc_master_gsm[16];
	char gc_universal_gsm[16];
	char gc_secondary_gsm[16];
	char gc_secondary_gsm1[16];
	char gc_secondary_gsm2[16];
	char gc_secondary_gsm3[16];

	int gi_log_interval;
	int gi_report_interval;

	int gi_sim_select;
	int gi_send_data_sms;

	ISP_APN_def ISP_APN_PARAMS[MAX_NO_OF_ISPs];

	float gps_latitude;
	char gps_lat_dir;
	float gps_longitude;
	char gps_lon_dir;

}Global_Data_Params_def;

Global_Data_Params_def Global_Data_Params;
Global_Data_Params_def *Global_Data_Params_From_Flash;
GPS_def GPS;

int recSt_flash, recBndry, offset_addr;
float ws_actual1, ws_measued1, ws_actual2, ws_measued2, ws_calib_factor, ws_offset;
float temp_actual1, temp_measued1, temp_actual2, temp_measued2, temp_calib_factor, temp_offset;
float humid_actual1, humid_measued1, humid_actual2, humid_measued2,humid_calib_factor, humid_offset;
float wd_actual1, wd_measued1, wd_actual2, wd_measued2,wd_calib_factor, wd_offset;

int latest_date, latest_month, latest_year;
int oldest_date, oldest_month, oldest_year;
int sampleNo;
int rf_close_date, rf_close_month, rf_close_year;

int new_sect_no;
int get_gps_time, get_gprs_time;
int latest_sample_num;
int data_sectors_not_allocated;
int erase_status, write_status;

char plus_minus_sym;


typedef struct  {
	float temperature;
	float humidity;
	float avg_ws;
	float inst_ws;
	int wind_dir;
}Global_WS_Data_def;

Global_WS_Data_def Global_WS_Data;
Global_WS_Data_def Packet_Data;

typedef struct{

	uint16_t temperature;
	uint16_t humidity;
	uint16_t wind_speed;
	uint16_t wind_direction;
	uint16_t ws_sec_cnt;


	uint16_t c_rf;
	uint16_t rf;
	uint16_t battery1;
	uint16_t battery2;
	uint16_t year;
	int16_t signal;


	uint8_t http_status;
	uint8_t ftp_status;
	uint8_t sms_status;
	uint8_t disk_status;

	uint8_t date;
	uint8_t month;
	uint8_t hour;
	uint8_t minute;

	uint8_t fill_byte[2];
}Samples_def;		//32 bytes

//Samples_def local_data;
Samples_def *write_buf_st;

typedef struct{
	uint8_t date;
	uint8_t month;
	uint16_t year;
	uint8_t last_rec_hr;
	uint8_t last_rec_min;
	uint8_t fill_byte[10];
	Samples_def samples[96];
}Days_Data_def;

Days_Data_def *dateVal;

Days_Data_def  *days_sector[MAX_NO_OF_DAYS];

/*
#ifndef DAYS_SECTOR_STR_DEFINED
#define DAYS_SECTOR_STR_DEFINED 1

Days_Data_def  *days_sector[MAX_NO_OF_DAYS]= {
		(Days_Data_def *) 0x00002000,
		(Days_Data_def *) 0x00003000,
		(Days_Data_def *) 0x00004000,
		(Days_Data_def *) 0x00005000,
		(Days_Data_def *) 0x00006000,
		(Days_Data_def *) 0x00007000,
		(Days_Data_def *) 0x00008000 };

#endif
 */


char *look_for_str(char *source_str, char *match_str);
int find_matching_str( char *given_str, char *string_set[], char **matching_str,
		int max_str, int opt);

