#include "xs2184.h"
#include <inttypes.h>
#include <fcntl.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/socket.h>

#define MAX_AVERAGE 15
#define MAX_AVERAGE_UB 300
#define MAX_AVERAGE_LB MAX_AVERAGE

#define PERME (S_IRWXU | S_IRGRP | S_IROTH)
#define RECORD_FILE "/tmp/xs2184_record"
#define RECORD_BUF_FILE "/tmp/xs2184_tmp"
#define RECORD_DAY_FILE "/tmp/xs2184_record_day"
#define CONFIG_FN "xs2184"
#define CMD_OPTIONS "d:u:m:Mcr:s:t:"
#define MONITOR_LOCK_FILE "/var/lock/xs2184-monitor.lock"
#define CONTROL_SOCKET "/var/run/xs2184.sock"
#define COMMAND_LOCK_FILE "/var/lock/xs2184-command.lock"
#define HARDWARE_LOCK_FILE "/var/lock/xs2184.lock"

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
	uint32_t watch_counter;
	u8 rollback;
	u8 mark_has_pd;
	u8 restart_pending;

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
#define RECORD_FORMAT               "%" SCNu32 " %f %f %f %f %f %f %f %f %f %f \n"
/* One week in seconds. */
#define UPDATE_FILE2_TIME           (7 * 86400)

static u8 record_en = 0;
static uint32_t record_time_up = 0;

#define HZ (1000 / statistic_inteval)   // intv per second

typedef int (* ps_callback_t)(u8 en, u8 port_num, float volt, float curt);

struct monitor_config {
	uint32_t interval, round, records;
	u8 recording;
};

static struct monitor_config current_config(void)
{
	return (struct monitor_config) {
		statistic_inteval, max_average_watts, record_times, record_en
	};
}

static void apply_config(const struct monitor_config *cfg)
{
	statistic_inteval = cfg->interval;
	max_average_watts = cfg->round;
	record_times = cfg->records;
	record_en = cfg->recording;
}

static volatile sig_atomic_t reload_requested;
static sigset_t monitor_wait_mask;

static void request_reload(int signo)
{
	(void)signo;
	reload_requested = 1;
}

static int setup_reload(void)
{
	struct sigaction action = { .sa_handler = request_reload };
	sigset_t blocked;

	sigemptyset(&action.sa_mask);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &blocked, &monitor_wait_mask) ||
	    sigaction(SIGHUP, &action, NULL)) {
		perror("Failed to set up xs2184 reload");
		return CMD_ERROR;
	}
	/* Deliver reloads only while waiting, outside UCI and hardware operations. */
	sigdelset(&monitor_wait_mask, SIGHUP);
	return CMD_SUCCESS;
}

static int reload_config(void);
static int initialize_records(void);

/* This single-threaded process may nest port controls inside a status poll. */
static int hardware_lock_fd = -1;
static unsigned int hardware_lock_depth;

static int lock_hardware(void)
{
	int fd;

	if (hardware_lock_depth) {
		hardware_lock_depth++;
		return CMD_SUCCESS;
	}
	fd = open(HARDWARE_LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) {
		perror("Failed to open xs2184 hardware lock");
		return CMD_ERROR;
	}
	while (flock(fd, LOCK_EX)) {
		if (errno == EINTR)
			continue;
		perror("Failed to lock xs2184 hardware");
		close(fd);
		return CMD_ERROR;
	}
	hardware_lock_fd = fd;
	hardware_lock_depth = 1;
	return CMD_SUCCESS;
}

static void unlock_hardware(void)
{
	if (--hardware_lock_depth == 0) {
		close(hardware_lock_fd);
		hardware_lock_fd = -1;
	}
}

static int lock_monitor(void)
{
	int fd = open(MONITOR_LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0600);

	if (fd < 0) {
		perror("Failed to open xs2184 monitor lock");
		return CMD_ERROR;
	}
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		int busy = errno == EWOULDBLOCK;

		if (!busy)
			perror("Failed to lock xs2184 monitor");
		close(fd);
		return busy ? -2 : CMD_ERROR;
	}
	return fd;
}

/* Serialize standalone commands and monitor startup before checking ownership. */
static int lock_commands(void)
{
	int fd = open(COMMAND_LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0600);

	if (fd < 0)
		return CMD_ERROR;
	while (flock(fd, LOCK_EX)) {
		if (errno == EINTR)
			continue;
		close(fd);
		return CMD_ERROR;
	}
	return fd;
}

#define CONTROL_MAGIC 0x58535031U
#define CONTROL_MAX_OPS 64
struct control_op {
	int option, port, value;
};
struct control_request {
	uint32_t magic, count;
	struct control_op ops[CONTROL_MAX_OPS];
};
static int control_fd = -1;
static void handle_control(void);

static int open_control(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct stat st;
	mode_t mask;
	int ret;

	if (!lstat(CONTROL_SOCKET, &st)) {
		if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
			errno = EEXIST;
			return CMD_ERROR;
		}
		if (unlink(CONTROL_SOCKET))
			return CMD_ERROR;
	} else if (errno != ENOENT)
		return CMD_ERROR;
	control_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (control_fd < 0)
		return CMD_ERROR;
	if (control_fd >= FD_SETSIZE) {
		close(control_fd);
		control_fd = -1;
		errno = EMFILE;
		return CMD_ERROR;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_SOCKET);
	mask = umask(0077);
	ret = bind(control_fd, (struct sockaddr *)&addr, sizeof(addr));
	umask(mask);
	if (ret || listen(control_fd, 16)) {
		close(control_fd);
		control_fd = -1;
		if (!ret)
			unlink(CONTROL_SOCKET);
		return CMD_ERROR;
	}
	return CMD_SUCCESS;
}

static void close_control(void)
{
	if (control_fd >= 0) {
		close(control_fd);
		control_fd = -1;
		unlink(CONTROL_SOCKET);
	}
}

/* Requests must not accelerate sampling or postpone it under a busy client. */
static int wait_monitor(void)
{
	uint32_t interval = statistic_inteval;
	struct timespec deadline, now, timeout;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline))
		return CMD_ERROR;
	deadline.tv_sec += interval / 1000;
	deadline.tv_nsec += (interval % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}
	while (!reload_requested && interval == statistic_inteval) {
		fd_set readers;
		int ret;

		if (clock_gettime(CLOCK_MONOTONIC, &now))
			return CMD_ERROR;
		timeout.tv_sec = deadline.tv_sec - now.tv_sec;
		timeout.tv_nsec = deadline.tv_nsec - now.tv_nsec;
		if (timeout.tv_nsec < 0) {
			timeout.tv_nsec += 1000000000L;
			timeout.tv_sec--;
		}
		if (timeout.tv_sec < 0)
			break;
		FD_ZERO(&readers);
		FD_SET(control_fd, &readers);
		ret = pselect(control_fd + 1, &readers, NULL, NULL,
		              &timeout, &monitor_wait_mask);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return CMD_ERROR;
		}
		if (!ret)
			break;
		handle_control();
	}
	return CMD_SUCCESS;
}

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
	int res;
	char file_name[64];

	file = open_i2c_dev(bus_num, file_name, sizeof(file_name), 0);
	if (file < 0) {
		fprintf(stderr, "open dev error.\n");
		return CMD_ERROR;
	}
	if (open_chip(file, xs_i2c_addrs[bus_num])) {
		close(file);
		return CMD_ERROR;
	}

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
	int res;
	char file_name[20];

	file = open_i2c_dev(bus_num, file_name, sizeof(file_name), 0);
	if (file < 0) {
		fprintf(stderr, "open dev error.\n");
		return CMD_ERROR;
	}
	if (open_chip(file, xs_i2c_addrs[bus_num])) {
		close(file);
		return CMD_ERROR;
	}

	res = i2c_smbus_write_byte_data(file, reg_addr, reg_val);

	close(file);

	if (res < 0) {
		fprintf(stderr, "write failed\n");
		return CMD_ERROR;
	}

	return CMD_SUCCESS;
}

int open_chip(int file, u8 chip_addr)
{
	unsigned long funcs;
	/* check adapter functionality */
	if (ioctl(file, I2C_FUNCS, &funcs) < 0) {
		fprintf(stderr, "Error: Could not get the adapter "
		        "functionality matrix: %s\n", strerror(errno));
		return CMD_ERROR;
	}

	if (ioctl(file, I2C_SLAVE, chip_addr) < 0)
		return CMD_ERROR;

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

	if (get_PoE_Offset_Vmain_mV(bus_num, &V_offset_mv))
		return CMD_ERROR;
	if (get_PoE_Offset_En(bus_num, &Offset_En))
		return CMD_ERROR;

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
	int error;

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

	error = ferror(fp);
	if (fclose(fp))
		error = 1;
	if (error) {
		fprintf(stderr, "error in writing %s\n", RECORD_BUF_FILE);
		return CMD_ERROR;
	}

	if (rename(RECORD_BUF_FILE, fn)) {
		fprintf(stderr, "error in replacing %s\n", fn);
		return CMD_ERROR;
	}

	return CMD_SUCCESS;
}

static int read_data_file(uint32_t line_bound, data_file *date, char *fn)
{
	uint32_t times = 0;
	FILE *fp;
	int error;
	char buf_file_line[512];

	memset(&buf_file_line, 0, sizeof(buf_file_line));

	if (!(fp = fopen(fn, "r"))) {
		fprintf(stderr, "error in reading %s\n", fn);
		return CMD_ERROR;
	}

	while(fgets(buf_file_line, sizeof(buf_file_line), fp) != NULL) {
		data_file record = {0};
		/* Check data format and discard which does not conform to the format */
		if (sscanf(buf_file_line, RECORD_FORMAT,
		           &record.ts, &record.pwr[1], &record.pwr[2],
		           &record.pwr[3], &record.pwr[4], &record.pwr[5],
		           &record.pwr[6], &record.pwr[7], &record.pwr[8],
		           &record.pwr[9], &record.pwr[10]) != PORT_NUM + 1) {
			memset(&buf_file_line, 0, sizeof(buf_file_line));
			continue;
		}

		memset(&buf_file_line, 0, sizeof(buf_file_line));

		/* Keep the newest records in file order when the buffer is full. */
		if (times == line_bound) {
			memmove(date, date + 1, (line_bound - 1) * sizeof(*date));
			times--;
		}
		date[times++] = record;
	}
	error = ferror(fp);
	if (fclose(fp))
		error = 1;
	if (error) {
		fprintf(stderr, "error in reading %s\n", fn);
		return CMD_ERROR;
	}

	return times;
}

static int get_record(char *fn, int bound, data_file *date)
{
	FILE *fp;
	int ret;

	if (access(fn, R_OK|W_OK)) {
		if (!(fp = fopen(fn, "w"))) {
			fprintf(stderr, "error in writing %s\n", fn);
			return CMD_ERROR;
		}
		if (fclose(fp))
			return CMD_ERROR;
	} else {
		ret = read_data_file(bound, date, fn);

		if (ret < 0) {
			fprintf(stderr, "error in reading %s\n", fn);
			return CMD_ERROR;
		}
	}
	return CMD_SUCCESS;
}

static int port_status_locked(ps_callback_t cb)
{
	u8 reg = 0;
	int i;
	int result = CMD_SUCCESS;
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
			goto sample_err;
		itoa(reg, bs, sizeof(bs), 2);
		if (!cb)
			fprintf(stdout, "chip on bus %u addr. 0x7%01x state b'%s'\n", i, (reg & 0x7), bs);

		if (get_power_status(i, &status))
			goto sample_err;
		if (port_voltage(i, &volt)) //unit:V
			goto sample_err;
		if (get_current(i, chip_p_num[i], curr))
			goto sample_err;

		for (port_num=1; port_num<=chip_p_num[i]; port_num++, vp++) {
			float curt = 0.0;
			u8 en = PoE_ENABLE;

			//check power on status
			if (!((status >> (port_num - 1)) & 0x1))
				en = PoE_DISABLE;

			if (en == PoE_ENABLE)
				port_current(port_num, curr, &curt);//unit: mA

			if (cb) {
				if (cb(en, vp, volt, curt))
					result = CMD_ERROR;
			} else
				fprintf(stderr, "vp %u volt/V %.2f curt/mA %.2f m-watts/mW %.3f\n",
				        vp, volt, curt, volt * curt);
		}
	}

	if (record_time_up) {
		FILE *fp;
		int error;
		u8 port_num;
		uint32_t now_time = time(NULL);
		uint32_t update_file1_time =
			record_times * RECORD_INTERVAL * statistic_inteval / 1000;

		record_time_up = 0;

		foreach_port(port_num)
		g_pwatts[port_num].ave_watts_day_r += g_pwatts[port_num].ave_watts_r;

		/* Append the latest data in file1 */
		if (!(fp = fopen(RECORD_FILE, "a"))) {
			fprintf(stderr, "error in writing %s\n", RECORD_FILE);
			return CMD_ERROR;
		}

		fprintf(fp, "%" PRIu32 " ", now_time);
		foreach_port(port_num)
		fprintf(fp, "%.2f ", g_pwatts[port_num].ave_watts_r);
		fprintf(fp, "\n");

		error = ferror(fp);
		if (fclose(fp))
			error = 1;
		if (error) {
			fprintf(stderr, "error in writing %s\n", RECORD_FILE);
			return CMD_ERROR;
		}

		/**
		 * At each summary interval, keep that interval of data in file1
		 * and calculating the total power consumption and saving it in file2.
		 */
		if (now_time - last_save_time >= update_file1_time) {
			int ret;
			int times = 0;
			float total_pwr[PORT_NUM+1] = {0.0};
			data_file data_file1[max_lines_file1];
			data_file data_file2[max_lines_file2];

			memset(&data_file1, 0, sizeof(data_file1));
			memset(&data_file2, 0, sizeof(data_file2));

			foreach_port(port_num)
			total_pwr[port_num] = g_pwatts[port_num].ave_watts_day_r;

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

			if (times == max_lines_file2) {
				memmove(data_file2, data_file2 + 1,
				        (max_lines_file2 - 1) * sizeof(*data_file2));
				times--;
			}
			data_file2[times].ts = now_time;
			foreach_port(port_num)
			data_file2[times].pwr[port_num] = total_pwr[port_num];

			ret = save_date_file(now_time, max_lines_file2, UPDATE_FILE2_TIME, data_file2, (char *)RECORD_DAY_FILE);
			if (ret < 0) {
				return CMD_ERROR;
			}

			foreach_port(port_num)
			g_pwatts[port_num].ave_watts_day_r = 0.0;
			last_save_time = now_time;
		}
	}
	return result;

sample_err:
	/* Discard an incomplete window so port averages remain synchronized. */
	if (record_en) {
		foreach_port(port_num)
		g_pwatts[port_num].watch_counter_r = 0;
		record_time_up = 0;
	}
	return CMD_ERROR;
}

int port_status(ps_callback_t cb)
{
	int ret;

	if (lock_hardware())
		return CMD_ERROR;
	ret = port_status_locked(cb);
	unlock_hardware();
	return ret;
}

static int enable_port_locked(char port_num)
{
	u8 bus_num = port_idx_to_bus_num(port_num);
	if (set_PsE_page(bus_num, PsE_REG_PAGE1))
		return CMD_ERROR;
	return write_reg(bus_num, PORT_EN_REG(port_num), PoE_ENABLE);
}

int enable_port(char port_num)
{
	int ret;

	if (lock_hardware())
		return CMD_ERROR;
	ret = enable_port_locked(port_num);
	unlock_hardware();
	return ret;
}

static int disable_port_locked(char port_num)
{
	u8 bus_num = port_idx_to_bus_num(port_num);
	if (set_PsE_page(bus_num, PsE_REG_PAGE1))
		return CMD_ERROR;
	return write_reg(bus_num, PORT_EN_REG(port_num), PoE_DISABLE);
}

int disable_port(char port_num)
{
	int ret;

	if (lock_hardware())
		return CMD_ERROR;
	ret = disable_port_locked(port_num);
	unlock_hardware();
	return ret;
}

static int restore_port_power(u8 vp)
{
	if (enable_port(vp)) {
		fprintf(stderr, "Failed to enable port %u\n", vp);
		return CMD_ERROR;
	}
	g_pwatts[vp].restart_pending = 0;
	return CMD_SUCCESS;
}

int port_monitor(u8 en, u8 vp, float volt, float curt)
{
	float mWatt = volt * curt;
	port_watt_t* pw = &g_pwatts[vp];
	int reboot_value = rb_watts[vp];
	int ret = CMD_SUCCESS;

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

	if (pw->restart_pending) {
		ret = restore_port_power(vp);
		goto out;
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
			if (disable_port(vp)) {
				fprintf(stderr, "Failed to disable port %u\n", vp);
				ret = CMD_ERROR;
				goto out;
			}
			pw->mark_has_pd = 0;
			pw->rollback = 0;
			pw->watch_counter = 0;
			fprintf(stderr, "port %u, closed with avg %.3f mW, thd %d mW \n", vp, av_mWatt, reboot_value);
			pw->restart_pending = 1;
			sleep(1);
			ret = restore_port_power(vp);
			if (ret)
				goto out;
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
out:
	if (pw->watch_counter == max_average_watts)
		pw->watch_counter = 0;

	return ret;
}

static int records_initialized;

static int initialize_records(void)
{
	int i;
	u8 port_num;
	data_file data_file1[max_lines_file1];
	data_file data_file2[max_lines_file2];

	memset(&data_file1, 0, sizeof(data_file1));
	memset(&data_file2, 0, sizeof(data_file2));

	if (get_record(RECORD_FILE, max_lines_file1, data_file1) ||
	    get_record(RECORD_DAY_FILE, max_lines_file2, data_file2))
		return CMD_ERROR;

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
	records_initialized = 1;
	return CMD_SUCCESS;
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
	if (record_en && initialize_records())
		return CMD_ERROR;

	while(1) {
		if (reload_requested) {
			reload_requested = 0;
			if (reload_config())
				fprintf(stderr, "Failed to reload xs2184 configuration\n");
		}
		port_status((ps_callback_t)port_monitor);
		if (wait_monitor()) {
			perror("Failed to wait for xs2184 monitor");
			return CMD_ERROR;
		}
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
	        "\t         -M : monitor using the configured interval\n" \
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

static void config_parse_globals(struct uci_context *c, struct uci_section *s,
                                 struct monitor_config *cfg)
{
	const char *value = NULL;

	value = uci_lookup_option_string(c, s, "round");
	cfg->round = value ? atoi(value) : cfg->round;

	value = uci_lookup_option_string(c, s, "interval");
	cfg->interval = value ? atoi(value) : cfg->interval;

	value = uci_lookup_option_string(c, s, "record_en");
	cfg->recording = value ? atoi(value) : cfg->recording;

	value = uci_lookup_option_string(c, s, "record_times");
	cfg->records = value ? atoi(value) : cfg->records;

	if (cfg->round < MAX_AVERAGE_LB || cfg->round > MAX_AVERAGE_UB)
		cfg->round = MAX_AVERAGE;

	if (cfg->interval < MONITOR_INTERVAL_LB || cfg->interval > MONITOR_INTERVAL_UB)
		cfg->interval = MONITOR_INTERVAL_LB;

	if (cfg->recording != 0 && cfg->recording != 1)
		cfg->recording = 0;

	if (cfg->records < RECORD_TIME_LB || cfg->records > RECORD_TIME_UB)
		cfg->records = RECORD_TIME_UB;
}

static int reload_config(void)
{
	struct monitor_config cfg = {
		MONITOR_INTERVAL_LB, MAX_AVERAGE, RECORD_TIME_UB, 0
	};
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *p = NULL;
	struct uci_element *e;
	float *replacement[PORT_NUM + 1] = {0};
	unsigned int enabled[PORT_NUM + 1];
	int thresholds[PORT_NUM + 1];
	int i, ret = CMD_ERROR;

	if (!ctx)
		return CMD_ERROR;
	foreach_port(i) {
		enabled[i] = port_enable_flag[i];
		thresholds[i] = rb_watts[i];
	}
	if (uci_load(ctx, CONFIG_FN, &p)) {
		uci_perror(ctx, "Failed to load xs2184 configuration");
		goto out;
	}
	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *value;
		char name[16];

		if (!strcmp(s->type, "globals"))
			config_parse_globals(ctx, s, &cfg);
		if (strncmp(s->type, "port", 4))
			continue;
		foreach_port(i) {
			snprintf(name, sizeof(name), "port%d", i);
			if (!strcmp(s->e.name, name))
				break;
		}
		if (i > PORT_NUM)
			continue;
		value = uci_lookup_option_string(ctx, s, "enable");
		enabled[i] = value && !strcmp(value, "1");
		value = uci_lookup_option_string(ctx, s, "pwr_thd");
		thresholds[i] = value ? atoi(value) : MIN_LOAD_TRIG_mW_DEFAULT;
		if (thresholds[i] < MIN_LOAD_TRIG_mW_LB ||
		    thresholds[i] > MIN_LOAD_TRIG_mW_UB)
			thresholds[i] = MIN_LOAD_TRIG_mW_DEFAULT;
	}

	/* Prepare fallible allocations and history loading before touching ports. */
	if (cfg.round != max_average_watts) {
		foreach_port(i) {
			replacement[i] = calloc(cfg.round, sizeof(float));
			if (!replacement[i])
				goto out;
		}
	}
	if (cfg.recording && !records_initialized && initialize_records())
		goto out;
	if (lock_hardware())
		goto out;
	foreach_port(i) {
		if (enabled[i] != port_enable_flag[i] ||
		    (enabled[i] && g_pwatts[i].restart_pending)) {
			if (enabled[i] ? enable_port(i) : disable_port(i)) {
				fprintf(stderr, "Failed to apply reload to port %d\n", i);
				unlock_hardware();
				goto out;
			}
			/* Keep successful changes even if a later port fails; retry is safe. */
			port_enable_flag[i] = enabled[i];
			g_pwatts[i].restart_pending = 0;
			g_pwatts[i].watch_counter = 0;
			g_pwatts[i].rollback = 0;
			g_pwatts[i].mark_has_pd = 0;
		}
		if (!enabled[i])
			g_pwatts[i].restart_pending = 0;
	}
	unlock_hardware();

	foreach_port(i) {
		if (replacement[i]) {
			free(g_pwatts[i].watts);
			g_pwatts[i].watts = replacement[i];
			replacement[i] = NULL;
		}
		rb_watts[i] = thresholds[i];
		g_pwatts[i].watch_counter = 0;
		g_pwatts[i].rollback = 0;
		g_pwatts[i].mark_has_pd = 0;
		g_pwatts[i].watch_counter_r = 0;
		g_pwatts[i].ave_watts_r = 0;
	}
	/* Keep completed record totals, discard incomplete sampling windows. */
	record_time_up = 0;
	apply_config(&cfg);
	fprintf(stderr, "Reloaded xs2184 configuration\n");
	ret = CMD_SUCCESS;
out:
	foreach_port(i)
		free(replacement[i]);
	uci_free_context(ctx);
	return ret;
}

static int save_item_uci(struct uci_ptr ptr, struct uci_context *ctx, \
                          struct uci_package *p, char *section, char *option, char *value)
{
	ptr.package = CONFIG_FN;
	ptr.o = NULL;
	ptr.s = uci_lookup_section(ctx, p, section);
	ptr.section = section;
	ptr.option = option;
	ptr.value = value;
	return uci_set(ctx, &ptr);
}

static int valid_control_op(const struct control_op *op)
{
	switch (op->option) {
	case 'u':
	case 'd':
		return op->port >= 1 && op->port <= PORT_NUM &&
		       op->value == (op->option == 'u');
	case 'r':
		return op->port >= 1 && op->port <= PORT_NUM &&
		       op->value >= MIN_LOAD_TRIG_mW_LB && op->value <= MIN_LOAD_TRIG_mW_UB;
	case 's':
		return op->value >= MAX_AVERAGE_LB && op->value <= MAX_AVERAGE_UB;
	case 't':
		return op->value == 0 || op->value == 1;
	default:
		return 0;
	}
}

static int apply_control(const struct control_request *request)
{
	struct uci_context *ctx;
	struct uci_package *p = NULL;
	struct uci_ptr ptr = {0};
	uint32_t i;
	int ret = CMD_ERROR;

	if (request->magic != CONTROL_MAGIC || request->count > CONTROL_MAX_OPS)
		return CMD_ERROR;
	for (i = 0; i < request->count; i++)
		if (!valid_control_op(&request->ops[i]))
			return CMD_ERROR;
	if (!request->count)
		return reload_config();
	ctx = uci_alloc_context();
	if (!ctx)
		return CMD_ERROR;
	if (uci_load(ctx, CONFIG_FN, &p))
		goto out;
	ptr.p = p;
	for (i = 0; i < request->count; i++) {
		const struct control_op *op = &request->ops[i];
		char section[16], value[16];
		char *option;

		snprintf(section, sizeof(section), "port%d", op->port);
		snprintf(value, sizeof(value), "%d", op->value);
		switch (op->option) {
		case 'u': case 'd': option = "enable"; break;
		case 'r': option = "pwr_thd"; break;
		case 's': option = "round"; strcpy(section, "globals"); break;
		default: option = "record_en"; strcpy(section, "globals"); break;
		}
		if (save_item_uci(ptr, ctx, p, section, option, value))
			goto out;
		/* Preserve explicit command order, including a requested off/on cycle. */
		if (uci_save(ctx, p) || uci_commit(ctx, &p, false))
			goto out;
		ptr.p = p;
		if (reload_config())
			goto out;
	}
	ret = CMD_SUCCESS;
out:
	if (ret)
		fprintf(stderr, "Failed to apply xs2184 control request\n");
	uci_free_context(ctx);
	return ret;
}

static void handle_control(void)
{
	struct control_request request;
	struct ucred peer;
	socklen_t length = sizeof(peer);
	int client, ret = CMD_ERROR;
	struct pollfd pollfd;

	client = accept4(control_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (client < 0)
		return;
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &length) ||
	    peer.uid != geteuid())
		goto out;
	/* A client that connects without sending must not stall the monitor. */
	pollfd = (struct pollfd) { .fd = client, .events = POLLIN };
	if (poll(&pollfd, 1, 1000) <= 0)
		goto out;
	if (recv(client, &request, sizeof(request), MSG_TRUNC) != sizeof(request))
		goto out;
	ret = apply_control(&request);
out:
	if (send(client, &ret, sizeof(ret), MSG_NOSIGNAL) != sizeof(ret))
		fprintf(stderr, "Failed to reply to xs2184 control client\n");
	close(client);
}

static int forward_control(int argc, char **argv)
{
	struct control_request request = { .magic = CONTROL_MAGIC };
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct ucred peer;
	socklen_t length = sizeof(peer);
	struct timeval timeout = { .tv_sec = 30 };
	int option, port = 0, query = 0, fd, ret = CMD_ERROR;

	optind = 0;
	while ((option = getopt(argc, argv, CMD_OPTIONS)) != CMD_ERROR) {
		struct control_op op = { .option = option };

		if (option == 'c') {
			query = 1;
			continue;
		}
		if (option == 'u' || option == 'd') {
			port = atoi(optarg);
			op.value = option == 'u';
		} else if (option == 's' || option == 'r')
			op.value = atoi(optarg);
		else if (option == 't' && (!strcmp(optarg, "0") || !strcmp(optarg, "1")))
			op.value = atoi(optarg);
		else
			goto input_err;
		op.port = port;
		if (!valid_control_op(&op) || request.count == CONTROL_MAX_OPS)
			goto input_err;
		request.ops[request.count++] = op;
	}
	if (query && !request.count)
		return port_status(NULL);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return CMD_ERROR;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_SOCKET);
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
	    connect(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) ||
	    peer.uid != geteuid())
		goto out;
	if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request))
		goto out;
	if (recv(fd, &ret, sizeof(ret), MSG_TRUNC) != sizeof(ret)) {
		fprintf(stderr, "Control reply unavailable; check configuration before retrying\n");
		ret = CMD_ERROR;
	}
	if (!ret && query)
		ret = port_status(NULL);
out:
	close(fd);
	if (ret)
		fprintf(stderr, "xs2184 control request failed\n");
	return ret;
input_err:
	help();
	return CMD_ERROR;
}

int main(int argc, char *argv[])
{
	int c, i;
	int port = 0;
	int monitor_enable = 0;
	int monitor_fd = -1;
	int command_fd = -1;
	int monitor_requested = 0;
	int saved_opterr = opterr;
	char buf[10];
	struct uci_package *p = NULL;
	struct uci_context *ctx = NULL;
	struct uci_element *e;

	/* Claim monitor ownership before applying any configuration or hardware changes. */
	opterr = 0;
	while ((c = getopt(argc, argv, CMD_OPTIONS)) != CMD_ERROR) {
		if (c == 'm' || c == 'M')
			monitor_requested = 1;
	}
	opterr = saved_opterr;
	optind = 0;
	command_fd = lock_commands();
	if (command_fd < 0) {
		perror("Failed to lock xs2184 commands");
		return CMD_ERROR;
	}
	monitor_fd = lock_monitor();
	if (monitor_fd == -2) {
		int ret;

		if (monitor_requested) {
			fprintf(stderr, "Another xs2184 monitor is already running\n");
			ret = CMD_ERROR;
		} else
			ret = forward_control(argc, argv);
		close(command_fd);
		return ret;
	}
	if (monitor_fd < 0) {
		close(command_fd);
		return CMD_ERROR;
	}
	if (monitor_requested && setup_reload()) {
		close(monitor_fd);
		close(command_fd);
		return CMD_ERROR;
	}

	/* read configuration from uci */
	fprintf(stdout, "Get configuration\n");

	ctx = uci_alloc_context();
	if (!ctx) {
		fprintf(stderr, "Out of memory\n");
		close(monitor_fd);
		close(command_fd);
		return CMD_ERROR;
	}

	uci_load(ctx, CONFIG_FN, &p);
	if (!p) {
		fprintf(stderr, "Failed to load config file\n");
		goto operation_err;
	}

	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "globals")) {
			struct monitor_config cfg = current_config();

			config_parse_globals(ctx, s, &cfg);
			apply_config(&cfg);
		}

		if (!strncmp(s->type, "port", 4)) {
			char *enable = NULL, *pwr_thd = NULL;
			int port_uci;
			char port_name[16];

			for (port_uci = 1; port_uci <= PORT_NUM; port_uci++) {
				snprintf(port_name, sizeof(port_name), "port%d", port_uci);
				if (!strcmp(s->e.name, port_name))
					break;
			}
			if (port_uci > PORT_NUM) {
				fprintf(stderr, "Invalid port section %s\n", s->e.name);
				continue;
			}

			enable = uci_lookup_option_string(ctx, s, "enable");

			if (enable && !strcmp(enable, "1")) {
				port_enable_flag[port_uci] = PoE_ENABLE;
				if (enable_port(port_uci) < 0) {
					fprintf(stderr, "Failed to open port %d\n", port_uci);
					goto operation_err;
				} else
					usleep(5);
			} else {
				port_enable_flag[port_uci] = PoE_DISABLE;
				if (disable_port(port_uci) < 0) {
					fprintf(stderr, "Failed to close port %d\n", port_uci);
					goto operation_err;
				} else
					usleep(5);
			}
			pwr_thd = uci_lookup_option_string(ctx, s, "pwr_thd");
			rb_watts[port_uci] = pwr_thd ? atoi(pwr_thd) : MIN_LOAD_TRIG_mW_DEFAULT;
			if (rb_watts[port_uci] < MIN_LOAD_TRIG_mW_LB || rb_watts[port_uci] > MIN_LOAD_TRIG_mW_UB)
				rb_watts[port_uci] = MIN_LOAD_TRIG_mW_DEFAULT;
		}
	}

	struct uci_ptr ptr = {0};
	ptr.p = p;

	while ((c = getopt(argc, argv, CMD_OPTIONS)) != CMD_ERROR) {
		switch (c) {
		case 'c':
			if (port_status(NULL) < 0) {
				fprintf(stderr, "Failed to read port status\n");
				goto operation_err;
			}
			break;
		case 'u':
			port = atoi(optarg);
			if (port < 1 || port > PORT_NUM)
				goto input_err;

			if (enable_port(port) < 0) {
				fprintf(stderr, "Failed to enable port %d\n", port);
				goto operation_err;
			}

			memset(&buf, 0, sizeof(buf));
			sprintf(buf, "port%d", port);
			if (save_item_uci(ptr, ctx, p, buf, "enable", "1"))
				goto config_err;
			port_enable_flag[port] = PoE_ENABLE;
			break;
		case 'd':
			port = atoi(optarg);
			if (port < 1 || port > PORT_NUM)
				goto input_err;

			if (disable_port(port) < 0) {
				fprintf(stderr, "Failed to disable port %d\n", port);
				goto operation_err;
			}

			memset(&buf, 0, sizeof(buf));
			sprintf(buf, "port%d", port);
			if (save_item_uci(ptr, ctx, p, buf, "enable", "0"))
				goto config_err;
			port_enable_flag[port] = PoE_DISABLE;
			break;
		case 'M':
			monitor_enable = 1;
			break;
		case 'm':
			statistic_inteval = atoi(optarg);
			if (statistic_inteval < MONITOR_INTERVAL_LB || statistic_inteval > MONITOR_INTERVAL_UB)
				goto input_err;

			monitor_enable = 1;
			if (save_item_uci(ptr, ctx, p, "globals", "interval", optarg))
				goto config_err;
			break;
		case 's':
			max_average_watts = atoi(optarg);
			if (max_average_watts < MAX_AVERAGE_LB || max_average_watts > MAX_AVERAGE_UB)
				goto input_err;
			if (save_item_uci(ptr, ctx, p, "globals", "round", optarg))
				goto config_err;
			break;
		case 'r':
			if (port < 1 || port > PORT_NUM)
				goto input_err;

			rb_watts[port] =  atoi(optarg); //units: mV
			if (rb_watts[port] < MIN_LOAD_TRIG_mW_LB || rb_watts[port] > MIN_LOAD_TRIG_mW_UB)
				goto input_err;

			memset(&buf, 0, sizeof(buf));
			sprintf(buf, "port%d", port);
			if (save_item_uci(ptr, ctx, p, buf, "pwr_thd", optarg))
				goto config_err;
			break;
		case 't':
			if (!strcmp(optarg, "1"))
				record_en = 1;
			else if (!strcmp(optarg, "0"))
				record_en = 0;
			else
				goto input_err;

			if (save_item_uci(ptr, ctx, p, "globals", "record_en", optarg))
				goto config_err;
			break;
		case '?':
		default:
			goto input_err;
		}
	}

	show_config();

	if (uci_save(ctx, ptr.p))
		goto config_err;
	if (uci_commit(ctx, &ptr.p, false))
		goto config_err;

	if (monitor_enable) {
		int ret;

		if (open_control()) {
			perror("Failed to open xs2184 control channel");
			goto operation_err;
		}
		close(command_fd);
		command_fd = -1;
		ret = run_monitor();
		close_control();
		close(monitor_fd);
		uci_free_context(ctx);
		return ret;
	}

	if (monitor_fd >= 0)
		close(monitor_fd);
	if (command_fd >= 0)
		close(command_fd);
	uci_free_context(ctx);
	return 0;

config_err:
	uci_perror(ctx, "Failed to update xs2184 configuration");
operation_err:
	if (monitor_fd >= 0)
		close(monitor_fd);
	if (command_fd >= 0)
		close(command_fd);
	uci_free_context(ctx);
	return CMD_ERROR;

input_err:
	help();
	goto operation_err;
}
