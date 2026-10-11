// Run from the repository root with fs and the apsta_freq native test module.
import * as fs from 'fs';
import { freq_info } from 'apsta_freq';

let checks = 0;
function check(ok, message) {
	if (!ok)
		die(`FAIL: ${message}\n`);
	checks++;
}

let published = {}, calls = [], timers = [], log = [];
let bus;
bus = {
	publish: (name, methods) => {
		published[name] = methods;
		return { notify: () => null };
	},
	call: (name, method, args) => {
		push(calls, { name, method, args });
		return published[name]?.[method]?.call({ args });
	},
	defer: (name, method, args) => {
		push(timers, { callback: () => bus.call(name, method, args) });
		return { abort: () => {} };
	},
};
global.require = () => ({ connect: () => bus, guard: () => {} });
global.uloop = {
	timer: (delay, callback) => {
		let timer = { callback, cancelled: false };
		timer.cancel = () => { timer.cancelled = true; };
		push(timers, timer);
		return timer;
	},
};
global.open = fs.open;
global.readfile = fs.readfile;
global.access = fs.access;
global.is_equal = (a, b) => sprintf('%J', a) == sprintf('%J', b);
global.wdev_remove = () => {};
global.hostapd = { data: {}, interfaces: {}, freq_info,
	printf: (msg) => push(log, msg), udebug_set: () => {} };
global.wpas = { data: {}, interfaces: {},
	printf: (msg) => push(log, msg), udebug_set: () => {},
	remove_iface: () => {} };

// Import adapters are mocked, but both complete production scripts are compiled
// and executed. Expose private helpers only in the in-memory test source.
function load_script(name, extra) {
	let source = fs.readfile(`package/network/services/hostapd/files/${name}.uc`);
	source = join('\n', filter(split(source, '\n'),
		(line) => substr(line, 0, 7) != 'import '));
	let pos = rindex(source, '\nreturn {');
	check(pos >= 0, `${name} return table`);
	source = substr(source, 0, pos) + `\nreturn { test: { ${extra} },` +
		substr(source, pos + length('\nreturn {'));
	return loadstring(source, { raw_mode: true, strict_declarations: true })();
}
let hap = load_script('hostapd', 'iface_gen_config, iface_add, iface_freq_info, radio_reload_class');
let sta = load_script('wpa_supplicant', 'iface_stop, iface_hostapd_notify, iface_hostapd_disabled, mld_set_iface_config');

function tick() {
	let next = timers;
	timers = [];
	for (let timer in next)
		if (!timer.cancelled)
			timer.callback();
}

let switches = [], stops = 0, starts = 0, state, inflight;
let allowed_width = 2, reject_csa = false;
let iface = {
	state: () => 'ENABLED',
	status: () => ({ ...state, beacon: true }),
	csa_in_progress: () => !!inflight,
	check_freq: (info) => info.oper_chwidth <= allowed_width,
	switch_channel: (info) => {
		if (reject_csa)
			return false;
		push(switches, { ...info });
		inflight = { ...info };
		return true;
	},
	stop: () => { stops++; },
	start: () => { starts++; return true; },
};
function setup(freq, data, sec) {
	timers = []; switches = []; calls = []; inflight = null;
	state = freq_info(freq, sec ?? 1, freq < 4000 ? 0 : 1);
	hostapd.data.config = { phy0: {
		radio: { frequency: freq, channel: state.channel, channel_follow: true,
			data: data ?? [ 'hw_mode=a', 'ht_capab=[HT40+]', 'vht_oper_chwidth=2' ] },
		bss: [ { ifname: 'ap0', bssid: '00:11:22:33:44:55', data: [] } ],
	} };
	hostapd.data.apsta_freq = {};
	hostapd.data.apsta_default = {};
	hostapd.data.apsta_pending = {};
	hostapd.interfaces = { phy0: iface };
	wpas.data.iface_sta = { sta0: true };
	wpas.data.iface_phy = { sta0: 'phy0' };
	wpas.data.mld = {};
	allowed_width = 2; reject_csa = false;
}
function event(args) {
	return bus.call('hostapd', 'apsta_state', { phy: 'phy0', up: true, sta: true, ...args });
}
function complete() {
	check(!!inflight, 'CSA is pending');
	state = inflight;
	inflight = null;
	tick();
}
function notify(status, name) {
	sta.test.iface_hostapd_notify('sta0', { status: () => status }, name);
}

setup(5180);
check(index(hap.test.iface_gen_config(hostapd.data.config.phy0, false), 'start_disabled=1') < 0,
	'AP starts without waiting for STA');
for (let name in [ 'SCANNING', 'AUTHENTICATING', 'DISCONNECTED', 'INACTIVE' ])
	notify({ disabled: false }, name);
check(!length(switches) && state.frequency == 5180, 'no uplink keeps configured channel');

check(!event({ frequency: 5500, sec_chan_offset: 1, prepare: true }).ready,
	'authentication waits for CSA');
check(switches[0].oper_chwidth == 2, 'try configured 160 MHz ceiling first');
complete();
check(event({ frequency: 5500, sec_chan_offset: 1, prepare: true }).ready,
	'authentication proceeds after CSA');
notify({ disabled: false, frequency: 5500, sec_chan_offset: 1 }, 'COMPLETED');
for (let name in [ 'DISCONNECTED', 'SCANNING', 'AUTHENTICATING', 'INACTIVE' ])
	notify({ disabled: false }, name);
check(state.frequency == 5500 && length(switches) == 1,
	'natural loss and failed authentication retain followed channel and width');
notify({ disabled: true }, 'INACTIVE');
check(inflight?.frequency == 5180 && inflight?.oper_chwidth == 2,
	'explicit disable restores configured channel and width');
complete();

// Disable during an outstanding follow switch: wait, then restore; never stop.
event({ frequency: 5745, sec_chan_offset: 1, prepare: true });
notify({ disabled: true }, 'DISCONNECTED');
check(inflight.frequency == 5745, 'do not overlap channel switches');
complete();
check(inflight?.frequency == 5180, 'disable queued behind in-flight CSA');
complete();
check(!hostapd.data.apsta_freq.phy0, 'disabled STA releases follow policy');

// Actual removal, versus temporary recreation during a configuration update.
event({ frequency: 5500, sec_chan_offset: 1, prepare: true }); complete();
wpas.remove_iface = () => notify({ disabled: true }, 'DISCONNECTED');
sta.test.iface_stop({ running: true, config: { iface: 'sta0' } }, false);
tick();
check(!inflight && state.frequency == 5500, 'internal STA recreation does not reset AP');
wpas.data.iface_sta.sta0 = true; wpas.data.iface_phy.sta0 = 'phy0';
sta.test.iface_stop({ running: true, config: { iface: 'sta0' } }, true);
tick();
check(inflight?.frequency == 5180, 'removing STA restores configured channel'); complete();
wpas.remove_iface = () => {};

let base = { channel: 36, data: [ 'hw_mode=a', 'ht_capab=[HT40+]', 'vht_oper_chwidth=1' ] };
check(hap.test.radio_reload_class(base, { ...base, channel_follow: true,
	data: [ ...base.data, 'noscan=1' ] }) == 'same', 'adding STA does not restart AP');
check(hap.test.radio_reload_class({ ...base, channel_follow: true,
	data: [ ...base.data, 'noscan=1' ] }, base) == 'same', 'removing STA does not restart AP');
check(hap.test.radio_reload_class(base, { ...base, data: [ ...base.data, 'ieee80211be=1' ] }) == 'restart',
	'actual AP capability changes still restart');
check(hap.test.radio_reload_class(base, { ...base, data: [ ...base.data, 'noscan=1' ] }) == 'restart',
	'explicit AP-only noscan changes still restart');

setup(5180); allowed_width = 1;
event({ frequency: 5500, sec_chan_offset: 1, prepare: true });
check(inflight.oper_chwidth == 1, 'invalid 160 MHz falls back to 80 MHz'); complete();
setup(5180, [ 'vht_oper_chwidth=0' ]);
state = freq_info(5180, 0, 0); state.center_seg0_idx = 0;
check(event({ frequency: 5180, sec_chan_offset: 0, ht20: true, prepare: true }).ready,
	'20 MHz zero-center representation is already ready');

setup(2412, [ 'hw_mode=g', 'ht_capab=[HT40+]' ]);
event({ frequency: 2462, sec_chan_offset: -1, prepare: true });
check(inflight.center_freq1 == 2452 && inflight.center_seg0_idx == 0,
	'2.4 GHz HT40 minus center frequency'); complete();
event({ disabled: true });
check(inflight.center_freq1 == 2422 && inflight.sec_channel == 1,
	'restore configured HT40 plus direction'); complete();

let info = freq_info(7055, null, 3);
check(info.oper_chwidth == 9 && info.center_seg0_idx == 191 && info.center_freq1 == 6905,
	'last 6 GHz 320 MHz block');
check(freq_info(7115, null, 3) == null, 'no 320 MHz block covers 6 GHz channel 233');
check(freq_info(5180, 0, 1) == null, 'wide channel requires a secondary offset');
setup(5955, [ 'hw_mode=a', 'ieee80211be=1', 'eht_oper_chwidth=9' ]);
check(hap.test.iface_freq_info(iface, hostapd.data.config.phy0, { frequency: 6275 }).oper_chwidth == 9,
	'EHT 320 MHz ceiling is recognized');
let normal_check = iface.check_freq;
iface.check_freq = (candidate) => candidate.oper_chwidth == 9 && candidate.center_seg0_idx == 63;
event({ frequency: 6275, prepare: true });
check(inflight?.oper_chwidth == 9 && inflight?.center_seg0_idx == 63,
	'overlapping 320 MHz placement is tried before narrowing'); complete();
hostapd.data.config.phy0.radio.data = [ 'eht_oper_chwidth=9', 'eht_oper_centr_freq_seg0_idx=63' ];
hostapd.data.config.phy0.radio.frequency = 6275;
check(event({ disabled: true, prepare: true }).ready,
	'restore configured 320 MHz center placement');
iface.check_freq = normal_check;

// An AP created after STA association also observes the width ceiling/fallback.
setup(5180); allowed_width = 1;
hostapd.add_iface = () => 0;
let startup_info;
let normal_start = iface.start;
iface.start = (candidate) => { startup_info = candidate; return true; };
check(hap.test.iface_add('phy0', hostapd.data.config.phy0,
	{ frequency: 5500, sec_chan_offset: 1 }), 'AP starts after existing STA association');
check(startup_info.frequency == 5500 && startup_info.oper_chwidth == 1,
	'already-associated STA startup falls back to supported 80 MHz');
iface.start = normal_start;

setup(5180); hostapd.data.config.phy0.radio.frequency = 0;
event({ frequency: 5500, sec_chan_offset: 1, prepare: true }); complete();
event({ disabled: true });
check(inflight.frequency == 5180 && inflight.oper_chwidth == 1,
	'channel=auto restores startup selection'); complete();

setup(5180); reject_csa = true;
check(!event({ frequency: 5500, sec_chan_offset: 1, prepare: true }).ready,
	'unsupported CSA blocks connection instead of stopping AP');
event({ frequency: 5500, sec_chan_offset: 1 });
for (let i = 0; i < 51; i++) tick();
check(state.frequency == 5180 && !hostapd.data.apsta_pending.phy0,
	'CSA timeout leaves existing AP running');
check(!stops && !starts, 'STA lifecycle never calls AP stop/start');

// MLD loss and no-link notifications obey the same retention policy.
setup(5180);
hostapd.data.config['phy0.0'] = hostapd.data.config.phy0;
hostapd.data.config = { 'phy0.0': hostapd.data.config['phy0.0'] };
hostapd.interfaces = { 'phy0.0': iface };
wpas.data.mld = { sta0: { phy: 'phy0', radio_mask: 3,
	phy_config: [ { freq_list: [ 5180, 5500 ] }, { freq_list: [ 2412 ] } ] } };
check(!sta.prepare('sta0', { freq: 5500, sec_chan_offset: 1,
	links: [ { freq: 2412, sec_chan_offset: 1 } ] }), 'MLD primary waits for AP CSA');
check(filter(calls, (c) => c.method == 'apsta_state' && c.args.prepare && c.args.radio == 1)[0],
	'MLD secondary radio is prepared before authentication'); complete();
check(sta.prepare('sta0', { freq: 5500, sec_chan_offset: 1 }), 'MLD ready after CSA');
notify({ disabled: false, links: [] }, 'COMPLETED');
notify({ disabled: false }, 'DISCONNECTED');
check(state.frequency == 5500 && length(switches) == 1, 'MLD no-link and loss retain channel');
sta.test.iface_hostapd_disabled('sta0'); tick();
check(inflight?.frequency == 5180, 'MLD explicit removal restores channel'); complete();

setup(5180); wpas.data.iface_sta.sta0 = false;
notify({ disabled: false }, 'DISCONNECTED');
check(stops == 1, 'mesh/adhoc legacy AP stop policy is preserved');
hostapd.interfaces = {}; hostapd.data.config = {};
wpas.data.iface_sta.sta0 = true;
check(sta.prepare('sta0', { freq: 5500, sec_chan_offset: 1 }), 'standalone STA can connect');

// Operating classes take precedence over stale width fields after reload.
for (let op_class, width in { '131': 0, '132': 0, '133': 1, '134': 2, '137': 9 }) {
	setup(5955, [ `op_class=${op_class}`, 'eht_oper_chwidth=9' ]);
	let candidate = hap.test.iface_freq_info(iface, hostapd.data.config.phy0,
		{ frequency: 6275 });
	check(candidate?.oper_chwidth == width, `operating class ${op_class} sets the width`);
	check(candidate?.sec_channel == (op_class == '131' ? 0 : 1),
		`operating class ${op_class} sets the secondary channel`);
}
setup(5180, [ 'op_class=130', 'vht_oper_chwidth=3' ]);
check(hap.test.iface_freq_info(iface, hostapd.data.config.phy0,
	{ frequency: 5500 }) == null, 'generic CSA refuses unsupported 80+80');
event({ frequency: 5500, prepare: true });
check(inflight?.oper_chwidth == 1, 'AP/STA follows the primary 80 MHz segment of 80+80');

let colored = { ...base, data: [ ...base.data, 'he_bss_color=1' ] };
let changed_color = { ...base, data: [ ...base.data, 'he_bss_color=2' ] };
check(hap.test.radio_reload_class(colored, changed_color) == 'spatial_reuse',
	'upstream live BSS color updates remain available');
check(hap.test.radio_reload_class(colored, { ...colored, channel_follow: true,
	data: [ ...colored.data, 'noscan=1' ] }) == 'same',
	'STA-only changes with BSS color do not trigger a color update');
check(hap.test.radio_reload_class(colored, { ...changed_color, channel_follow: true,
	data: [ ...changed_color.data, 'noscan=1' ] }) == 'spatial_reuse',
	'color and STA-presence changes can be applied together');

// Upstream moved MLD config handling and distinguishes held radios from removal.
let mld = { name: 'sta0', phy: 'phy0', phy_config: [], radio_mask: 3,
	radio_mask_present: 0, radio_mask_kept: 0 };
let radio_config = { freq_list: [ 5180, 5500 ] };
wpas.interfaces = {};
sta.test.mld_set_iface_config('sta0', mld, { radio: 0, stopped: false }, radio_config);
check(mld.radio_mask_present == 1 && length(mld.freq_list) == 2,
	'available MLD radio contributes its configured frequencies');
calls = []; timers = [];
sta.test.mld_set_iface_config('sta0', mld, { radio: 0, stopped: true }, radio_config);
check(mld.radio_mask_present == 0 && !length(mld.freq_list) && !length(timers),
	'held MLD radio keeps its config without restoring the AP channel');
sta.test.mld_set_iface_config('sta0', mld, { radio: 0, stopped: false }, null);
tick();
check(calls[0]?.args.disabled && calls[0]?.args.radio == 0,
	'removed MLD radio config requests AP channel restoration');

printf('AP/STA: %d checks passed\n', checks);
