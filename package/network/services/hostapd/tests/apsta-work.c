/* Test the real pending-work code; only daemon/driver/event-loop edges are mocked. */
#include <assert.h>
#include "wpa_supplicant/ucode.c"

static uc_vm_t test_vm;
static bool ready, fail_timer;
static unsigned int freed, scans, deauths, auths, checks;
static eloop_timeout_handler timer_handler;
static void *timer_ctx, *timer_work;

int wpa_ucode_call_prepare(const char *name)
{
	assert(!strcmp(name, "prepare"));
	return 0;
}

uc_value_t *wpa_ucode_call(size_t nargs)
{
	assert(nargs == 2);
	while (nargs--)
		ucv_put(uc_vm_stack_pop(vm));
	return ucv_boolean_new(ready);
}

const u8 *wpa_bss_get_ie(const struct wpa_bss *bss, u8 id)
{
	return NULL;
}

int eloop_register_timeout(unsigned int secs, unsigned int usecs,
			   eloop_timeout_handler handler, void *ctx, void *work)
{
	assert(!timer_handler);
	if (fail_timer)
		return -1;
	timer_handler = handler; timer_ctx = ctx; timer_work = work;
	return 0;
}

int eloop_cancel_timeout(eloop_timeout_handler handler, void *ctx, void *work)
{
	if (timer_handler == handler && timer_ctx == ctx && timer_work == work) {
		timer_handler = NULL; timer_ctx = NULL; timer_work = NULL;
		return 1;
	}
	return 0;
}

/* Linked wpas_connect_work_done/free are the production functions. */
void radio_work_done(struct wpa_radio_work *work)
{
	assert(!timer_handler);
	freed++;
	free(work);
}

void wpa_supplicant_req_scan(struct wpa_supplicant *wpa_s, int sec, int usec)
{
	scans++;
}

void wpa_supplicant_deauthenticate(struct wpa_supplicant *wpa_s, u16 reason)
{
	deauths++;
	wpas_connect_work_done(wpa_s);
}

void _wpa_msg(void *ctx, int level, const char *fmt, ...)
{
}

static void connect_cb(struct wpa_radio_work *work, int deinit)
{
	if (!wpas_ucode_prepare_channel(work))
		auths++;
}

static struct wpa_radio_work *new_work(struct wpa_supplicant *wpa_s,
				      struct wpa_bss *bss, struct wpa_ssid *ssid)
{
	struct wpa_radio_work *work = calloc(1, sizeof(*work));
	struct wpa_connect_work *ctx = calloc(1, sizeof(*ctx));
	assert(work && ctx && !wpa_s->connect_work);
	ctx->bss = bss; ctx->ssid = ssid;
	work->wpa_s = wpa_s; work->ctx = ctx; work->cb = connect_cb;
	wpa_s->connect_work = work;
	return work;
}

#define CHECK(expr) do { assert(expr); checks++; } while (0)

int main(void)
{
	uc_parse_config_t config = { .raw_mode = true };
	struct wpa_supplicant wpa_s = { .ifname = "sta0" };
	struct wpa_bss bss = { .freq = 5500 };
	struct wpa_ssid ssid = { .mode = WPAS_MODE_INFRA };
	struct wpa_radio_work *work;
	eloop_timeout_handler handler;
	void *ctx, *arg;

	uc_vm_init(&test_vm, &config);
	vm = &test_vm;
	ready = true;
	work = new_work(&wpa_s, &bss, &ssid);
	CHECK(!wpas_ucode_prepare_channel(work));
	CHECK(!timer_handler);
	wpas_connect_work_done(&wpa_s);

	ready = false;
	work = new_work(&wpa_s, &bss, &ssid);
	CHECK(wpas_ucode_prepare_channel(work));
	CHECK(timer_handler && timer_work == work);
	ready = true;
	handler = timer_handler; ctx = timer_ctx; arg = timer_work;
	timer_handler = NULL;
	handler(ctx, arg);
	CHECK(auths == 1 && !timer_handler);
	wpas_connect_work_done(&wpa_s);

	ready = false;
	work = new_work(&wpa_s, &bss, &ssid);
	CHECK(wpas_ucode_prepare_channel(work));
	wpas_connect_work_done(&wpa_s);
	CHECK(!timer_handler && !wpa_s.connect_work);

	work = new_work(&wpa_s, &bss, &ssid);
	((struct wpa_connect_work *)work->ctx)->apsta_retries = 50;
	CHECK(wpas_ucode_prepare_channel(work));
	CHECK(!timer_handler && !wpa_s.connect_work && scans == 1);

	work = new_work(&wpa_s, &bss, &ssid);
	wpa_s.wpa_state = WPA_COMPLETED; wpa_s.assoc_freq = 5180;
	CHECK(wpas_ucode_prepare_channel(work));
	CHECK(!timer_handler && !wpa_s.connect_work && deauths == 1 && scans == 2);

	work = new_work(&wpa_s, &bss, &ssid);
	wpa_s.assoc_freq = 5500;
	CHECK(wpas_ucode_prepare_channel(work));
	CHECK(timer_handler && deauths == 1);
	wpas_connect_work_done(&wpa_s);

	wpa_s.wpa_state = WPA_DISCONNECTED;
	work = new_work(&wpa_s, &bss, &ssid);
	fail_timer = true;
	CHECK(wpas_ucode_prepare_channel(work));
	CHECK(!wpa_s.connect_work && scans == 3);
	fail_timer = false;

	work = new_work(&wpa_s, &bss, &ssid);
	ssid.mode = WPAS_MODE_MESH;
	CHECK(!wpas_ucode_prepare_channel(work));
	CHECK(!timer_handler);
	wpas_connect_work_done(&wpa_s);
	CHECK(freed == 8);
	uc_vm_free(&test_vm);
	printf("AP/STA work: %u checks passed\n", checks);
	return 0;
}
