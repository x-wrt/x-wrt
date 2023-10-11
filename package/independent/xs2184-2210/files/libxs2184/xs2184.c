#include "xs2184.h"

#define MAX_AVERAGE 15
#define MAX_AVERAGE_UB 300
#define MAX_AVERAGE_LB MAX_AVERAGE

#define PERME (S_IRWXU | S_IRGRP | S_IROTH)
#define RECORD_FILE "/tmp/xs2184_record"
#define RECORD_BUF_FILE "/tmp/xs2184_tmp"
#define RECORD_DAY_FILE "/tmp/xs2184_record_day"
#define CONFIG_FN "xs2184"

static int xs_i2c_addrs[MAX_CHIPS] = {
	CHIP_ADDR,
	CHIP_ADDR,
};

static int xs_port_to_i2c[PORT_NUM+1] = {
	-1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1
    };

static int chip_p_num[MAX_CHIPS] = {
	LAN_PORT_NUM,
	WAN_PORT_NUM,
};

typedef struct {
	float* watts;
	u8 watch_counter;
	u8 rollback;
	u8 mark_has_pd;

	float* watts_r;
	u8 watch_counter_r;
	float ave_watts_r;
	float ave_watts_day_r;
} port_watt_t;

static port_watt_t g_pwatts[PORT_NUM + 1];

typedef struct {
	uint32_t ts;
	float pwr[PORT_NUM + 1];
} data_file;

#define MONITOR_INTERVAL_UB         100000
#define MONITOR_INTERVAL_LB         1000
static uint32_t statistic_inteval = MONITOR_INTERVAL_LB; //ms
static uint32_t max_average_watts = MAX_AVERAGE;

#define RECORD_TIME_UB              1080
#define RECORD_TIME_LB              1
static uint32_t record_times = RECORD_TIME_UB;
static uint32_t max_lines_file2 = 56; // file 2 caches data in a week
static uint32_t max_lines_file1 = RECORD_TIME_UB*2;
static uint32_t last_save_time;

static int rb_watts[PORT_NUM + 1];
static uint32_t port_enable_flag[PORT_NUM + 1];

#define MIN_LOAD_TRIG_mW_DEFAULT    1000
#define MIN_LOAD_TRIG_mW_UB         100000
#define MIN_LOAD_TRIG_mW_LB         0
#define RECORD_INTERVAL             10
#define RECORD_FORMAT               "%lu %f %f %f %f %f %f %f %f \n"
/* 86400 seconds per week*/
#define UPDATE_FILE2_TIME           86400 //units: s

static u8 record_en = 0;
static uint32_t record_time_up = 0;

#define HZ (1000 / statistic_inteval)   // intv per second

typedef int (* ps_callback_t)(u8 en, u8 port_num, float volt, float curt);

#ifndef strrev
void strrev(u8 *str)
{
	int i;
	int j;
	u8 a;
	unsigned len = strlen((const char *)str);
	for (i = 0, j = len - 1; i < j; i++, j--) {
		a = str[i];
		str[i] = str[j];
		str[j] = a;
	}
}
#endif

#ifndef itoa
int itoa(int num, u8* str, int len, int base)
{
	int sum = num;
	int i = 0;
	int digit;

	if (len == 0)
		return CMD_ERROR;
	do {
		digit = sum % base;
		if (digit < 0xA)
			str[i++] = '0' + digit;
		else
			str[i++] = 'A' + digit - 0xA;
		sum /= base;
	} while (sum && (i < (len - 1)));
	if (i == (len - 1) && sum)
		return CMD_ERROR;
	str[i] = '\0';
	strrev(str);
	return 0;
}
#endif

static int port_idx_to_bus_num(u8 port_num)
{
	return xs_port_to_i2c[port_num];
}

static int read_reg(u8 bus_num, u8 reg_addr, u8 *reg_val, u8 len)
{
	int file;
	u8 res;
	char file_name[64];

	file = open_i2c_dev(bus_num, file_name, sizeof(file_name), 0);
	if (file < 0)
		fprintf(stderr, "open dev error.\n");
	open_chip(file, xs_i2c_addrs[bus_num]);

	res = i2c_smbus_read_i2c_block_data(file, reg_addr, len, reg_val);

	close(file);

	if (res != len) {
		fprintf(stderr, "read failed\n");
		return CMD_ERROR;
	}

	return CMD_SUCCESS;
}

static int write_reg(u8 bus_num, u8 reg_addr, u8 reg_val)
{
	int file;
	u8 res;
	char file_name[20];

	file = open_i2c_dev(bus_num, file_name, sizeof(file_name), 0);
	if (file < 0)
		fprintf(stderr, "open dev error.\n");
	open_chip(file, xs_i2c_addrs[bus_num]);

	res = i2c_smbus_write_byte_data(file, reg_addr, reg_val);

	close(file);

	if (res < 0) {
		fprintf(stderr, "write failed\n");
		return CMD_ERROR;
	}

	return CMD_SUCCESS;
}

int open_chip(u8 file, u8 chip_addr)
{
	unsigned long funcs;
	/* check adapter functionality */
	if (ioctl(file, I2C_FUNCS, &funcs) < 0) {
		fprintf(stderr, "Error: Could not get the adapter "
		        "functionality matrix: %s\n", strerror(errno));
		return CMD_ERROR;
	}

	if (ioctl(file, I2C_SLAVE, chip_addr) < 0) {
		close(file);
		return CMD_ERROR;
	}

	return CMD_SUCCESS;
}

int set_PsE_page(u8 bus_num, uint32_t page)
{
	return write_reg(bus_num, PAGE_REG, (xs_i2c_addrs[bus_num] & 0x7) | (page<<6));
}

int get_current(u8 bus_num, u8 port_cnt, u8 *curr)
{

	if (set_PsE_page(bus_num, PsE_REG_PAGE0))
		return CMD_ERROR;

	if (read_reg(bus_num, P0_CRT_MSB_REG, curr, port_cnt*2))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

int port_current(u8 port_num, u8 *raw_cur, float *rslt_curr_mA)
{
	unsigned short curr ;
	uint32_t curr_frn, curr_int;
	u8 p_to_idx;
	p_to_idx = (port_num-1)*2;

	curr = raw_cur[p_to_idx]&0xF;
	curr <<= 8;
	curr |= raw_cur[p_to_idx+1];

	curr_frn = curr & 0x3;
	curr_int = curr >> 2;

	*rslt_curr_mA = (float)curr_int + (float)curr_frn*0.1;

	return CMD_SUCCESS;
}

int get_supply_voltage(u8 bus_num, unsigned short *voltage)
{
	u8 tmp[2]; // tmp[0] contains MSB.
	unsigned short voltage_;

	if (set_PsE_page(bus_num, PsE_REG_PAGE0))
		return CMD_ERROR;

	if (read_reg(bus_num, SUPPLY_VOLTAGE_MSB_REG, tmp, sizeof(tmp)))
		return CMD_ERROR;

	voltage_ = tmp[0]&0xF;
	voltage_ <<= 8;
	voltage_ |= tmp[1];
	*voltage = voltage_;

	return 0;
}

int get_E_Fuse(u8 bus_num, u8 address, u8 *result)
{
	u8 tmp[1];
	unsigned int cnt = 0;

	if (set_PsE_page(bus_num,PsE_REG_PAGE0))
		return CMD_ERROR;
	if (write_reg(bus_num, E_FUSE_ADDR, address))
		return CMD_ERROR;

	if (write_reg(bus_num, E_FUSE_CTEL, 0x80))
		return CMD_ERROR;

	while (1) {
		if (read_reg(bus_num, E_FUSE_CTEL, tmp, sizeof(tmp)))
			return CMD_ERROR;
		if (tmp[0] & 0x1)
			break;
		cnt ++;
		if (cnt > 500)
			return CMD_ERROR;
		//	mdelay(2);
	}
	if (read_reg(bus_num, E_FUSE_R_DAT, tmp, sizeof(tmp)))
		return CMD_ERROR;

	*result = tmp[0];
	return CMD_SUCCESS;
}

int get_PoE_Offset_Vmain_mV(u8 bus_num, unsigned int *V_offset_mv)
{
	unsigned int offset_v;
	u8 tmp_8;

	if (get_E_Fuse(bus_num, 0x1F, &tmp_8))
		return CMD_ERROR;
	offset_v = (tmp_8 >> 6) &0x03;
	offset_v <<= 2;
	if (get_E_Fuse(bus_num, 0x1E, &tmp_8))
		return CMD_ERROR;
	offset_v |= (tmp_8 >> 6) &0x03;
	if (offset_v >> 3)
		*V_offset_mv = (offset_v & 0x7)*100;
	else {
		*V_offset_mv = ((offset_v & 0x7)+1)*100;
		*V_offset_mv |= 0x8000;
	}

	return CMD_SUCCESS;
}

int get_PoE_Offset_En(u8 bus_num, u8 *En)
{
	u8 chip;
	u8 tmp_8;
	u8 trim_en;

	//get offset Enable
	if (get_E_Fuse(bus_num, 0x21, &tmp_8))
		return CMD_ERROR;
	if ((tmp_8 >> 6) == 0x01)
		trim_en = PoE_ENABLE;
	else
		trim_en = PoE_DISABLE;
	*En = trim_en;

	return CMD_SUCCESS;
}

int float_arith(unsigned int float_bit, u8 bit_size)
{
	u8 i;
	unsigned int temp32=0;
	unsigned int bit_table[] =
	{51200,25600,12800,6400,3200,1600,800,400,200,100,50,25};
	for (i=0; i<bit_size; i++) {
		if ( (float_bit >>(bit_size-i-1) )&0x1)
			temp32 += bit_table[i];
	}
	return temp32;
}

int get_fix_Vmian_mV(u8 bus_num, unsigned int *result_mV)
{
	unsigned short Vmain;
	unsigned int Vmain_mV, tmp32, V_offset_mv;
	u8 Offset_En;

	if (get_supply_voltage(bus_num, &Vmain))
		return CMD_ERROR;

	Vmain_mV = (Vmain >> 4);
	Vmain_mV = Vmain_mV * 1000;
	tmp32 = float_arith((Vmain & 0x000F),4)/100;
	Vmain_mV = Vmain_mV + tmp32;

	*result_mV = Vmain_mV;

	get_PoE_Offset_Vmain_mV(bus_num, &V_offset_mv);
	get_PoE_Offset_En(bus_num, &Offset_En);

	if (Offset_En == PoE_ENABLE)
	{
		if ((V_offset_mv & 0x8000) != 0)
			*result_mV = *result_mV - (V_offset_mv & 0x7FFF);
		else
			*result_mV = *result_mV + V_offset_mv;
	}

	return CMD_SUCCESS;
}

int port_voltage(u8 bus_num, float *result_voltage)
{
	int i;
	unsigned int voltage;
	for (i = 0; i < 50; i++)
	{
		if (get_fix_Vmian_mV(bus_num, &voltage))
			return CMD_ERROR;

		if (voltage > 45000 && voltage < 60000)
			break;
	}

	if (voltage <= 45000 || voltage >= 60000)
		voltage = 48000;

	*result_voltage = voltage * 0.001;

	return CMD_SUCCESS;
}

int get_power_status(u8 bus_num, u8 *status)
{
	u8 tmp[1];

	if (set_PsE_page(bus_num, PsE_REG_PAGE1))
		return CMD_ERROR;

	if (read_reg(bus_num, POWER_STATUS_REG, tmp, sizeof(tmp)))
		return CMD_ERROR;

	*status = tmp[0];

	return CMD_SUCCESS;
}

static int save_date_file(uint32_t time, uint32_t line_bound, uint32_t time_bound, data_file *data,  char *fn)
{
	uint32_t i;
	u8 port_num;
	FILE *fp;

	if (!(fp = fopen(RECORD_BUF_FILE, "w"))) {
		fprintf(stderr, "error in writing %s\n", RECORD_BUF_FILE);
		return CMD_ERROR;
	}

	for (i=0; i<line_bound; i++) {
		if (!data[i].ts)
			break;
		/* Data generated from the time before time_bound will be discarded */
		if (time - data[i].ts < time_bound) {
			fprintf(fp, "%lu ", (unsigned long)data[i].ts);

			foreach_port(port_num)
			fprintf(fp, "%.2f ", data[i].pwr[port_num]);

			fprintf(fp, "\n");
		}
	}

	fclose(fp);

	rename(RECORD_BUF_FILE, fn);

	return CMD_SUCCESS;
}

static uint32_t read_data_file(uint32_t line_bound, data_file *date, char *fn)
{
	uint32_t times = 0;
	FILE *fp;
	char buf_file_line[100];

	memset(&buf_file_line, 0, sizeof(buf_file_line));

	if (!(fp = fopen(fn, "r"))) {
		fprintf(stderr, "error in reading %s\n", fn);
		return CMD_ERROR;
	}

	while(fgets(buf_file_line, sizeof(buf_file_line), fp) != NULL) {
		/* Check data format and discard which does not conform to the format */
		if (sscanf(buf_file_line, RECORD_FORMAT,
		           &date[times].ts, &date[times].pwr[1], &date[times].pwr[2],
		           &date[times].pwr[3], &date[times].pwr[4], &date[times].pwr[5],
		           &date[times].pwr[6], &date[times].pwr[7], &date[times].pwr[8]) != PORT_NUM + 1) {
			memset(&buf_file_line, 0, sizeof(buf_file_line));
			continue;
		}

		memset(&buf_file_line, 0, sizeof(buf_file_line));

		times++;
		/* If 'times' greater than 'line_bound', the oldest data is discarded firstly. */
		if (times == line_bound)
			times = 0;
	}
	fclose(fp);

	return times;
}

static void get_record(char *fn, int bound, data_file *date)
{
	FILE *fp;
	int ret;

	if (access(fn, R_OK|W_OK)) {
		if (!(fp = fopen(fn, "w"))) {
			fprintf(stderr, "error in writing %s\n", fn);
			exit(CMD_ERROR);
		}
		fclose(fp);
	} else {
		ret = read_data_file(bound, date, fn);

		if (ret < 0) {
			fprintf(stderr, "error in reading %s\n", fn);
			exit(CMD_ERROR);
		}
	}
}

int port_status(ps_callback_t cb)
{
	u8 reg = 0;
	int i;
	char bs[40] = "\0";
	u8 port_num, vp = 1;

	for (i=0; i<MAX_CHIPS; i++) {
		int addr = xs_i2c_addrs[i]; // chip addr
		u8 curr[chip_p_num[i]*2];
		u8 status;
		float volt;

		if (addr < 0)
			continue;

		if (read_reg(i, PAGE_REG, &reg, 1))
			return CMD_ERROR;
		itoa(reg, bs, sizeof(bs), 2);
		if (!cb)
			fprintf(stdout, "chip on bus %u addr. 0x7%01x state b'%s'\n", i, (reg & 0x7), bs);

		if (get_power_status(i, &status))
			return CMD_ERROR;
		if (port_voltage(i, &volt)) //unit:V
			return CMD_ERROR;
		if (get_current(i, chip_p_num[i], curr))
			return CMD_ERROR;

		for (port_num=1; port_num<=chip_p_num[i]; port_num++, vp++) {
			float curt = 0.0;
			u8 en = PoE_ENABLE;

			//check power on status
			if (!((status >> (port_num - 1)) & 0x1))
				en = PoE_DISABLE;

			if (en == PoE_ENABLE)
				port_current(port_num, curr, &curt);//unit: mA

			if (cb)
				cb(en, vp, volt, curt);
			else
				fprintf(stderr, "vp %u volt/V %.2f curt/mA %.2f m-watts/mW %.3f\n",
				        vp, volt, curt, volt * curt);
		}
	}

	if (record_time_up) {
		FILE *fp;
		u8 port_num;
		uint32_t now_time = time(NULL);
		uint32_t update_file1_time = record_times*10;

		record_time_up = 0;

		foreach_port(port_num)
		g_pwatts[port_num].ave_watts_day_r += g_pwatts[port_num].ave_watts_r;

		/* Append the latest data in file1 */
		if (!(fp = fopen(RECORD_FILE, "a"))) {
			fprintf(stderr, "error in writing %s\n", RECORD_FILE);
			return CMD_ERROR;
		}

		fprintf(fp, "%lu ", now_time);
		foreach_port(port_num)
		fprintf(fp, "%.2f ", g_pwatts[port_num].ave_watts_r);
		fprintf(fp, "\n");

		fclose(fp);

		/**
		 * Every 3 hours, keeping the data of the last 3 hours in file1
		 * and calculating the total power consumption and saving it in file2.
		 */
		if (now_time - last_save_time >= update_file1_time) {
			int ret;
			uint32_t times = 0;
			float total_pwr[PORT_NUM+1] = {0.0};
			data_file data_file1[max_lines_file1];
			data_file data_file2[max_lines_file2];

			memset(&data_file1, 0, sizeof(data_file1));
			memset(&data_file2, 0, sizeof(data_file2));

			foreach_port(port_num) {
				total_pwr[port_num] = g_pwatts[port_num].ave_watts_day_r;
				g_pwatts[port_num].ave_watts_day_r = 0.0;
			}

			ret = read_data_file(max_lines_file1, data_file1, (char *)RECORD_FILE);
			if (ret < 0) {
				return CMD_ERROR;
			}

			ret = save_date_file(now_time, max_lines_file1, update_file1_time, data_file1, (char *)RECORD_FILE);
			if (ret < 0) {
				return CMD_ERROR;
			}

			/* Read previous data in file2 */
			times = 0;
			times = read_data_file(max_lines_file2, data_file2, (char *)RECORD_DAY_FILE);
			if (times < 0) {
				return CMD_ERROR;
			}

			data_file2[times].ts = now_time;
			foreach_port(port_num)
			data_file2[times].pwr[port_num] = total_pwr[port_num];

			ret = save_date_file(now_time, max_lines_file2, UPDATE_FILE2_TIME, data_file2, (char *)RECORD_DAY_FILE);
			if (ret < 0) {
				return CMD_ERROR;
			}

			last_save_time = now_time;
		}
	}
	return 0;
}

int enable_port(char port_num)
{
	u8 bus_num = port_idx_to_bus_num(port_num);
	if (set_PsE_page(bus_num, PsE_REG_PAGE1))
		return CMD_ERROR;
	return write_reg(bus_num, PORT_EN_REG(port_num), PoE_ENABLE);
}

int disable_port(char port_num)
{
	u8 bus_num = port_idx_to_bus_num(port_num);
	if (set_PsE_page(bus_num, PsE_REG_PAGE1))
		return CMD_ERROR;
	return write_reg(bus_num, PORT_EN_REG(port_num), PoE_DISABLE);
}

int port_monitor(u8 en, u8 vp, float volt, float curt)
{
	float mWatt = volt * curt;
	port_watt_t* pw = &g_pwatts[vp];
	int reboot_value = rb_watts[vp];

	pw->watts[pw->watch_counter++] = mWatt;
	if (record_en) {
		pw->watts_r[pw->watch_counter_r++] = mWatt;
		if (pw->watch_counter_r == RECORD_INTERVAL) {
			int i;
			pw->ave_watts_r = 0.0;
			record_time_up = 1;
			for (i=0; i<pw->watch_counter_r; i++)
				pw->ave_watts_r += pw->watts_r[i];
			if (pw->ave_watts_r)
				pw->ave_watts_r /= RECORD_INTERVAL;
			pw->watch_counter_r = 0;
		}
	}

	if (en) {
		int i, counter;
		float av_mWatt = 0.0;

		counter = pw->rollback ? max_average_watts : pw->watch_counter;
		for (i=0; i<counter; i++)
			av_mWatt += pw->watts[i];

		av_mWatt /= counter;

		if (pw->rollback && pw->mark_has_pd)
			fprintf(stderr, "[monitoring] ");
		else
			fprintf(stderr, "[waiting rollback] ");
		fprintf(stderr, "port %u, cnt %u, avg cnt %u, %0.2f mW, round avg %.3f mW\n",
		        vp, pw->watch_counter, counter, mWatt, av_mWatt);

		if (av_mWatt < reboot_value && pw->rollback && pw->mark_has_pd) {
			pw->mark_has_pd = 0;
			pw->rollback = 0;
			pw->watch_counter = 0;
			fprintf(stderr, "port %u, closed with avg %.3f mW, thd %d mW \n", vp, av_mWatt, reboot_value);
			disable_port(vp);
			sleep(1);
			enable_port(vp);
		} else if (av_mWatt >= reboot_value)
			pw->mark_has_pd = 1;

		if (pw->watch_counter == max_average_watts)
			pw->rollback = 1;

		/* Port will not be automatically opened since 2210e is in manual mode.
		   So, there is no need to shutdown it. */
		if (MODE == AUTO && !port_enable_flag[vp]) {
			fprintf(stdout, "port %u disable PD detection\n", vp);
			disable_port(vp);
			sleep(1);
		}
	} else {
		pw->watch_counter = 0;
		pw->rollback = 0;
		pw->mark_has_pd = 0;

		/* If the configuration sets the port to be open, try to open the port every 3 times. */
		if (MODE == AUTO && port_enable_flag[vp] && !(pw->watch_counter % 3)) {
			/* power on & wait PD */
			fprintf(stdout, "port %u enable PD detection\n", vp);
			enable_port(vp);
			sleep(1);   /* must keep it. */
		}
	}
	if (pw->watch_counter == max_average_watts)
		pw->watch_counter = 0;

	return 0;
}

static int run_monitor(void)
{
	int i;

	memset(&g_pwatts, 0, sizeof(g_pwatts));
	for (i=0; i<sizeof(g_pwatts)/sizeof(g_pwatts[0]); i++) {
		float *pwts = malloc(sizeof(float) * max_average_watts);
		float *pwts_r = malloc(sizeof(float) * RECORD_INTERVAL);
		if (!pwts || !pwts_r) {
			exit(CMD_ERROR);
		}
		memset(pwts, 0, max_average_watts * sizeof(float));
		memset(pwts_r, 0, RECORD_INTERVAL * sizeof(float));
		g_pwatts[i].watts = pwts;
		g_pwatts[i].watts_r = pwts_r;
	}
	if (record_en) {
		u8 port_num;
		data_file data_file1[max_lines_file1];
		data_file data_file2[max_lines_file2];

		memset(&data_file1, 0, sizeof(data_file1));
		memset(&data_file2, 0, sizeof(data_file2));

		get_record(RECORD_FILE, max_lines_file1, data_file1);
		get_record(RECORD_DAY_FILE, max_lines_file2, data_file2);

		/**
		 * If file2 has data, the timestamp of the latest data in it is used as the starting point.
		 */
		if (data_file2[0].ts) {
			last_save_time = 0;

			for (i=0; i<max_lines_file2; i++) {
				if (!data_file2[i].ts)
					break;

				last_save_time = data_file2[i].ts > last_save_time ? (uint32_t)data_file2[i].ts : last_save_time;
			}

			for (i=0; i<max_lines_file1; i++) {
				if (last_save_time < data_file1[i].ts) {
					foreach_port(port_num)
					g_pwatts[port_num].ave_watts_day_r += data_file1[i].pwr[port_num];
				}
			}
		} else {
			last_save_time = time(NULL);

			for (i=0; i < max_lines_file1; i++) {
				if (!data_file1[i].ts)
					break;

				last_save_time = data_file1[i].ts < last_save_time ? (uint32_t)data_file1[i].ts : last_save_time;

				foreach_port(port_num)
				g_pwatts[port_num].ave_watts_day_r += data_file1[i].pwr[port_num];
			}
		}
	}

	while(1) {
		port_status((ps_callback_t)port_monitor);
		usleep(statistic_inteval * 1000);
		fflush(stdout);
		fflush(stderr);
	}

	return 0;
}

static void help()
{
	fprintf(stdout, "xs2184 : View port status and switch control of a single port\n" \
	        "usage: xs2184 [option] [port num | param]\n" \
	        "\toption : -c : Command for viewing port status\n" \
	        "\t         -u : Single port enabled\n" \
	        "\t         -d : Single port disabled\n" \
	        "\t         -m : monitor interval, range is %d-%d ms\n" \
	        "\t         -s : average statistiacs count with range %d-%d \n" \
	        "\t         -r : time-average power consumption threshold with range %d-%d mV\n" \
	        "\t         -t : record function switch\n" \
	        "\tport num : 1-%d Counting from left, one port at a time\n\n" \
	        "Example: Set port 3 to calculate the time-average power consumption \n" \
	        "\tevery 30 rounds (1000ms/round), and restart port 3 as time-average \n" \
	        "\tpower consumption is lower than 2000mV\n" \
	        "\txs2184 -u 3 -s 30 -r 2000 -m 1000\n", \
	        MONITOR_INTERVAL_LB, MONITOR_INTERVAL_UB, MAX_AVERAGE_LB, MAX_AVERAGE_UB, \
	        MIN_LOAD_TRIG_mW_LB, MIN_LOAD_TRIG_mW_UB, PORT_NUM);
}

static void show_config()
{
	int i;
	fprintf(stdout, "----------------\n" \
	        "configuration : \n" \
	        "----------------\n" \
	        "round %d interval %d record_times %d record_en %d\n", \
	        max_average_watts, statistic_inteval, record_times, record_en);
	foreach_port(i)
	fprintf(stdout, "port: %d, thd: %d\n", i, rb_watts[i]);
}

static void config_parse_globals(struct uci_context *c, struct uci_section *s)
{
	char *cfg = NULL;

	cfg = uci_lookup_option_string(c, s, "round");
	max_average_watts = cfg ? atoi(cfg) : max_average_watts;

	cfg = uci_lookup_option_string(c, s, "interval");
	statistic_inteval = cfg ? atoi(cfg) : statistic_inteval;

	cfg = uci_lookup_option_string(c, s, "record_en");
	record_en = cfg ? atoi(cfg) : record_en;

	cfg = uci_lookup_option_string(c, s, "record_times");
	record_times = cfg ? atoi(cfg) : record_times;

	if (max_average_watts < MAX_AVERAGE_LB || max_average_watts > MAX_AVERAGE_UB)
		max_average_watts = MAX_AVERAGE;

	if (statistic_inteval < MONITOR_INTERVAL_LB || statistic_inteval > MONITOR_INTERVAL_UB)
		statistic_inteval = MONITOR_INTERVAL_LB;

	if (record_en != 0 && record_en != 1)
		record_en = 0;

	if (record_times < RECORD_TIME_LB || record_times > RECORD_TIME_UB)
		record_times = RECORD_TIME_UB;
}

static void save_item_uci(struct uci_ptr ptr, struct uci_context *ctx, \
                          struct uci_package *p, char *section, char *option, char *value)
{
	ptr.package = CONFIG_FN;
	ptr.o = NULL;
	ptr.s = uci_lookup_section(ctx, p, section);
	ptr.section = section;
	ptr.option = option;
	ptr.value = value;
	uci_set(ctx, &ptr);
}

int main(int argc, char *argv[])
{
	int c, i;
	int port = 0;
	int monitor_enable = 0;
	char buf[10];
	struct uci_package *p = NULL;
	struct uci_context *ctx = NULL;
	struct uci_element *e;

	/* read configuration from uci */
	fprintf(stdout, "Get configuration\n");

	ctx = uci_alloc_context();
	if (!ctx) {
		fprintf(stderr, "Out of memory\n");
		return CMD_ERROR;
	}

	uci_load(ctx, CONFIG_FN, &p);
	if (!p) {
		fprintf(stderr, "Failed to load config file\n");
		uci_free_context(ctx);
		return CMD_ERROR;
	}

	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "globals"))
			config_parse_globals(ctx, s);

		if (!strncmp(s->type, "port", 4)) {
			char *enable = NULL, *pwr_thd = NULL;
			int port_uci = 0;

			sscanf(s->e.name, "port%d", &port_uci);
			if (port_uci < 1 || port_uci > PORT_NUM)
				port_uci = 1;

			enable = uci_lookup_option_string(ctx, s, "enable");

			if (!strcmp(enable, "1")) {
				port_enable_flag[port_uci] = PoE_ENABLE;
				if (enable_port(port_uci) < 0)
					fprintf(stderr, "Failed to open port %d\n", port_uci);
				else
					usleep(5);
			} else {
				port_enable_flag[port_uci] = PoE_DISABLE;
				if (disable_port(port_uci) < 0)
					fprintf(stderr, "Failed to close port %d\n", port_uci);
				else
					usleep(5);
			}
			pwr_thd = uci_lookup_option_string(ctx, s, "pwr_thd");
			rb_watts[port_uci] = pwr_thd ? atoi(pwr_thd) : MIN_LOAD_TRIG_mW_DEFAULT;
			if (rb_watts[port_uci] < MIN_LOAD_TRIG_mW_LB || rb_watts[port_uci] > MIN_LOAD_TRIG_mW_UB)
				rb_watts[port_uci] = MIN_LOAD_TRIG_mW_DEFAULT;
		}
	}

	struct uci_ptr ptr;
	ptr.p = p;

	while ((c = getopt(argc, argv, "d:u:m:cr:s:t:")) != CMD_ERROR) {
		switch (c) {
		case 'c':
			if (port_status(NULL) < 0)
				fprintf(stderr, "no PD on port.\n");
			break;
		case 'u':
			port = atoi(optarg);
			if (port < 1 || port > PORT_NUM)
				goto input_err;

			enable_port(port);

			memset(&buf, 0, sizeof(buf));
			sprintf(buf, "port%d", port);
			save_item_uci(ptr, ctx, p, buf, "enable", "1");
			break;
		case 'd':
			port = atoi(optarg);
			if (port < 1 || port > PORT_NUM)
				goto input_err;

			disable_port(port);

			memset(&buf, 0, sizeof(buf));
			sprintf(buf, "port%d", port);
			save_item_uci(ptr, ctx, p, buf, "enable", "0");
			break;
		case 'm':
			statistic_inteval = atoi(optarg);
			if (statistic_inteval < MONITOR_INTERVAL_LB || statistic_inteval > MONITOR_INTERVAL_UB)
				goto input_err;

			monitor_enable = 1;
			save_item_uci(ptr, ctx, p, "globals", "interval", optarg);
			break;
		case 's':
			max_average_watts = atoi(optarg);
			if (max_average_watts < MAX_AVERAGE_LB || max_average_watts > MAX_AVERAGE_UB)
				goto input_err;
			save_item_uci(ptr, ctx, p, "globals", "round", optarg);
			break;
		case 'r':
			if (port < 1 || port > PORT_NUM)
				goto input_err;

			rb_watts[port] =  atoi(optarg); //units: mV
			if (rb_watts[port] < MIN_LOAD_TRIG_mW_LB || rb_watts[port] > MIN_LOAD_TRIG_mW_UB)
				goto input_err;

			memset(&buf, 0, sizeof(buf));
			sprintf(buf, "port%d", port);
			save_item_uci(ptr, ctx, p, buf, "pwr_thd", optarg);
			break;
		case 't':
			if (!strcmp(optarg, "1"))
				record_en = 1;
			else if (!strcmp(optarg, "0"))
				record_en = 0;
			else
				goto input_err;

			save_item_uci(ptr, ctx, p, "globals", "record_en", optarg);
			break;
		case '?':
		default:
			goto input_err;
		}
	}

	show_config();

	uci_save(ctx, ptr.p);
	uci_commit(ctx, &ptr.p, false);

	if (monitor_enable)
		return run_monitor();

	return 0;

input_err:
	help();
	return CMD_ERROR;
}
