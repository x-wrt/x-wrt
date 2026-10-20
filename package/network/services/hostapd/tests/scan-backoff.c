/* Exercise production scanning, network selection, retries and state changes. */
#include <assert.h>
#include "wpa_supplicant/scan.c"

extern const struct wpa_driver_ops wpa_driver_none_ops;

static unsigned int checks, scans, associations, scheduled, stopped;
static int scan_error;
static bool network_available;
static struct wpa_supplicant *test_iface;

#define CHECK(expr) do { assert(expr); checks++; } while (0)

static int test_scan(void *priv, struct wpa_driver_scan_params *params)
{
	scans++;
	return scan_error;
}

static struct wpa_scan_results *test_results(void *priv)
{
	const char name[] = "unavailable-scan-test";
	struct wpa_scan_results *results = os_zalloc(sizeof(*results));
	struct wpa_scan_res *res;
	u8 *ie;

	assert(results);
	if (!network_available)
		return results;
	results->res = os_zalloc(sizeof(*results->res));
	res = os_zalloc(sizeof(*res) + sizeof(name) + 1);
	assert(results->res && res);
	results->num = 1;
	results->res[0] = res;
	res->bssid[0] = 2;
	res->bssid[5] = 1;
	res->freq = 2412;
	res->caps = IEEE80211_CAP_ESS;
	res->level = -40;
	res->flags = WPA_SCAN_LEVEL_DBM;
	res->ie_len = sizeof(name) + 1;
	ie = (u8 *) (res + 1);
	ie[0] = WLAN_EID_SSID;
	ie[1] = sizeof(name) - 1;
	os_memcpy(ie + 2, name, sizeof(name) - 1);
	return results;
}

static int test_associate(void *priv, struct wpa_driver_associate_params *params)
{
	associations++;
	return -ECONNREFUSED;
}

static int test_sched_scan(void *priv, struct wpa_driver_scan_params *params)
{
	scheduled++;
	return 0;
}

static void stopped_event(void *ctx, void *data)
{
	wpa_supplicant_event(test_iface, EVENT_SCHED_SCAN_STOPPED, NULL);
}

static int test_stop_sched_scan(void *priv)
{
	stopped++;
	return eloop_register_timeout(0, 0, stopped_event, NULL, NULL);
}

static void stop_loop(void *ctx, void *data)
{
	eloop_terminate();
}

static void run_radio_work(void)
{
	CHECK(eloop_register_timeout(0, 1000, stop_loop, NULL, NULL) == 0);
	eloop_run();
}

static void start_scan(struct wpa_supplicant *wpa_s, bool manual)
{
	unsigned int before = scans;

	wpa_supplicant_cancel_scan(wpa_s);
	wpa_s->scan_req = manual ? MANUAL_SCAN_REQ : NORMAL_SCAN_REQ;
	wpa_supplicant_scan(wpa_s, NULL);
	if (!manual)
		wpa_supplicant_req_scan(wpa_s, 0, 0);
	/* Run queued radio work without waiting for the next scan timer. */
	run_radio_work();
	CHECK(scans == before + 1);
	if (!scan_error) {
		CHECK(!wpas_scan_scheduled(wpa_s));
		wpa_supplicant_event(wpa_s, EVENT_SCAN_STARTED, NULL);
	}
}

static void finish_scan(struct wpa_supplicant *wpa_s)
{
	union wpa_event_data data = {};

	wpa_supplicant_event(wpa_s, EVENT_SCAN_RESULTS, &data);
	run_radio_work();
	CHECK(!wpa_s->scan_work);
}

static void check_delay(struct wpa_supplicant *wpa_s, unsigned int seconds)
{
	struct os_reltime remaining;
	long long usec;

	CHECK(eloop_cancel_timeout_one(wpa_supplicant_scan, wpa_s, NULL,
				       &remaining) == 1);
	usec = remaining.sec * 1000000LL + remaining.usec;
	CHECK(usec <= seconds * 1000000LL);
	CHECK(usec >= seconds * 1000000LL - 100000);
}

int main(int argc, char **argv)
{
	struct wpa_params params = { .wpa_debug_level = MSG_ERROR };
	struct wpa_interface iface = { .ifname = "scan-test", .driver = "none" };
	struct wpa_driver_ops driver = wpa_driver_none_ops;
	struct wpa_global *global;
	struct wpa_supplicant *wpa_s;
	struct wpa_ssid *ssid;
	const unsigned int delays[] = { 1, 2, 4, 8, 16, 32, 60, 60 };
	size_t i;

	assert(argc == 2);
	iface.confname = argv[1];
	global = wpa_supplicant_init(&params);
	CHECK(global);
	wpa_s = wpa_supplicant_add_iface(global, &iface, NULL);
	CHECK(wpa_s);
	test_iface = wpa_s;
	ssid = wpa_s->conf->ssid;
	driver.scan2 = test_scan;
	driver.get_scan_results2 = test_results;
	driver.associate = test_associate;
	driver.sched_scan = test_sched_scan;
	driver.stop_sched_scan = test_stop_sched_scan;
	wpa_s->driver = &driver;
	wpa_s->drv_flags |= WPA_DRIVER_FLAGS_VALID_ERROR_CODES;
	wpa_s->scan_interval = 1;

	/* Missing SSID: use the real scan-result/network-selection path. */
	for (i = 0; i < ARRAY_SIZE(delays); i++) {
		start_scan(wpa_s, false);
		finish_scan(wpa_s);
		CHECK(wpa_s->scan_retry_delay == delays[i]);
		check_delay(wpa_s, delays[i]);
	}

	/* A manual scan starts immediately without clearing or growing backoff. */
	wpa_s->scan_req = MANUAL_SCAN_REQ;
	wpa_supplicant_req_scan(wpa_s, 0, 0);
	check_delay(wpa_s, 0);
	start_scan(wpa_s, true);
	finish_scan(wpa_s);
	CHECK(wpa_s->scan_retry_delay == 60);
	wpa_supplicant_cancel_scan(wpa_s);
	wpa_supplicant_req_scan(wpa_s, 120, 0);
	check_delay(wpa_s, 120);

	/* Authentication/state churn must not reset the accumulated delay. */
	wpa_supplicant_set_state(wpa_s, WPA_AUTHENTICATING);
	CHECK(wpa_s->scan_retry_delay == 60);
	wpa_supplicant_set_state(wpa_s, WPA_DISCONNECTED);
	CHECK(wpa_s->scan_retry_delay == 60);

	/* Successful association resets it; the next loss gets a fast first scan. */
	wpa_s->new_connection = 0;
	wpa_supplicant_set_state(wpa_s, WPA_COMPLETED);
	CHECK(!wpa_s->scan_retry_delay);
	wpa_supplicant_set_state(wpa_s, WPA_DISCONNECTED);
	wpa_supplicant_req_scan(wpa_s, 0, 100000);
	{
		struct os_reltime remaining;
		CHECK(eloop_cancel_timeout_one(wpa_supplicant_scan, wpa_s, NULL,
					       &remaining) == 1);
		CHECK(remaining.sec == 0 && remaining.usec <= 100000);
	}

	/* Found-but-failing and missing-SSID rounds share one continuous backoff. */
	for (i = 0; i < ARRAY_SIZE(delays); i++) {
		network_available = !!(i & 1);
		wpa_bssid_ignore_clear(wpa_s);
		wpas_clear_temp_disabled(wpa_s, ssid, 0);
		start_scan(wpa_s, false);
		finish_scan(wpa_s);
		CHECK(wpa_s->scan_retry_delay == delays[i]);
		check_delay(wpa_s, delays[i]);
	}
	CHECK(associations >= 4);

	/* A driver rejection must back off too, rather than retry every second. */
	scan_error = -EBUSY;
	start_scan(wpa_s, false);
	check_delay(wpa_s, 60);
	scan_error = 0;

	/* The upstream aborted-scan recovery must also honor the retry floor. */
	start_scan(wpa_s, false);
	{
		union wpa_event_data data = {};
		unsigned int before = associations;

		data.scan_info.aborted = 1;
		wpa_supplicant_event(wpa_s, EVENT_SCAN_RESULTS, &data);
		run_radio_work();
		CHECK(!wpa_s->scan_work);
		CHECK(associations == before);
		CHECK(wpa_s->scan_abort_retries == 1);
		check_delay(wpa_s, 60);
	}

	/* Hardware scan offload cannot replace this policy with a fixed interval. */
	wpa_s->sched_scan_supported = 1;
	wpa_s->max_sched_scan_ssids = 1;
	wpa_s->max_scan_ssids = 1;
	wpa_s->max_sched_scan_plan_interval = 600;
	wpa_s->normal_scans = 3;
	CHECK(wpa_supplicant_delayed_sched_scan(wpa_s, 0, 0) == -1);
	CHECK(wpa_supplicant_req_sched_scan(wpa_s) == -1);
	CHECK(!scheduled);
	/* Also stop an offload scan already running before a connection loss. */
	wpa_s->conf->sched_scan_plans = os_strdup("10");
	CHECK(wpa_supplicant_req_sched_scan(wpa_s) == 0);
	CHECK(scheduled == 1 && wpa_s->sched_scanning);
	os_free(wpa_s->conf->sched_scan_plans);
	wpa_s->conf->sched_scan_plans = NULL;
	wpa_supplicant_req_scan(wpa_s, 0, 100000);
	run_radio_work();
	CHECK(stopped == 1 && !wpa_s->sched_scanning);
	check_delay(wpa_s, 60);

	/* Administrative restart also starts a fresh retry sequence. */
	wpa_supplicant_set_state(wpa_s, WPA_INTERFACE_DISABLED);
	CHECK(!wpa_s->scan_retry_delay);
	wpa_supplicant_set_state(wpa_s, WPA_DISCONNECTED);
	wpa_s->scan_retry_delay = 60;
	wpa_supplicant_enable_network(wpa_s, ssid);
	CHECK(!wpa_s->scan_retry_delay);
	wpa_s->scan_retry_delay = 60;
	wpa_supplicant_select_network(wpa_s, ssid);
	CHECK(!wpa_s->scan_retry_delay);

	/* Preserve explicit scan policies and non-infrastructure workflows. */
	wpa_s->conf->autoscan = os_strdup("periodic:10");
	CHECK(!wpas_scan_backoff_enabled(wpa_s));
	os_free(wpa_s->conf->autoscan);
	wpa_s->conf->autoscan = NULL;
	wpa_s->conf->sched_scan_plans = os_strdup("10");
	CHECK(!wpas_scan_backoff_enabled(wpa_s));
	os_free(wpa_s->conf->sched_scan_plans);
	wpa_s->conf->sched_scan_plans = NULL;
	ssid->mode = WPAS_MODE_MESH;
	CHECK(!wpas_scan_backoff_enabled(wpa_s));
	ssid->mode = WPAS_MODE_INFRA;
	ssid->p2p_group = 1;
	CHECK(!wpas_scan_backoff_enabled(wpa_s));
	ssid->p2p_group = 0;
	ssid->key_mgmt = WPA_KEY_MGMT_WPS;
	CHECK(!wpas_scan_backoff_enabled(wpa_s));
	ssid->key_mgmt = WPA_KEY_MGMT_NONE;
	wpa_s->pno = 1;
	CHECK(!wpas_scan_backoff_enabled(wpa_s));
	wpa_s->pno = 0;

	wpa_supplicant_deinit(global);
	printf("STA scan backoff: %u checks passed\n", checks);
	return 0;
}
