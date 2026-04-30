'use strict';
'require baseclass';
'require fs';
'require rpc';

var callUsbInfo = rpc.declare({
	object: 'luci.usb',
	method: 'info'
});

var callWwanStatus = rpc.declare({
	object: 'network.interface.wwan',
	method: 'status',
	expect: { }
});

var callWanStatus = rpc.declare({
	object: 'network.interface.wan',
	method: 'status',
	expect: { }
});

var callLanStatus = rpc.declare({
	object: 'network.interface.lan',
	method: 'status',
	expect: { }
});

return baseclass.extend({
	title: _('Printer Info'),

	load: function() {
		return Promise.all([
			L.resolveDefault(callUsbInfo(), {}),
			L.resolveDefault(callWwanStatus(), {}),
			L.resolveDefault(callWanStatus(), {}),
			L.resolveDefault(callLanStatus(), {})
		]);
	},

	render: function(data) {
		var usbinfo   = data[0];
		var wwan = data[1];
		var wan = data[2];
		var lan = data[3];

		if (wwan['ipv4-address'] && wwan['ipv4-address'][0] && wwan['ipv4-address'][0]['address'])
			wwan = wwan['ipv4-address'][0]['address'];
		else
			wwan = "-";

		if (wan['ipv4-address'] && wan['ipv4-address'][0] && wan['ipv4-address'][0]['address'])
			wan = wan['ipv4-address'][0]['address'];
		else
			wan = "-";

		if (lan['ipv4-address'] && lan['ipv4-address'][0] && lan['ipv4-address'][0]['address'])
			lan = lan['ipv4-address'][0]['address'];
		else
			lan = "-";

		var fields = [
			_('USB Info'),         usbinfo.info ? usbinfo.info : "-",
			_('Wi-Fi Bridge'),             wwan,
			_('LAN Port'),              wan,
			_('Management IP'),              lan,
			_('Wi-Fi AP'),            lan
		];

		var table = E('table', { 'class': 'table' });

		for (var i = 0; i < fields.length; i += 2) {
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, [ fields[i] ]),
				E('td', { 'class': 'td left' }, [ (fields[i + 1] != null) ? fields[i + 1] : '?' ])
			]));
		}

		return table;
	}
});
