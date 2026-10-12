// SPDX-License-Identifier: Apache-2.0

'use strict';
'require dom';
'require form';
'require fs';
'require ui';
'require view';

var labels = {
	pon_sn: _('PON serial number'),
	device_sn: _('Device serial number'),
	pon_mac: _('PON MAC address'),
	board_mac: _('Board MAC address'),
	lan_base_mac: _('LAN base MAC address')
};

function runIdentity(args) {
	return fs.exec('/usr/libexec/pon-board-identity', args).then(function(result) {
		if (result.code != 0)
			throw new Error(result.stderr || result.stdout || _('Board identity operation failed.'));
		return result.stdout.trim();
	});
}

return view.extend({
	load: function() {
		return runIdentity([ 'list' ]).then(function(output) {
			var layout = JSON.parse(output);
			var data = {};
			var sequence = Promise.resolve();

			Object.keys(layout.targets || {}).forEach(function(target) {
				data[target] = {};
				Object.keys(layout.targets[target].fields || {}).forEach(function(field) {
					sequence = sequence.then(function() {
						return runIdentity([ 'read', target, field ]).then(function(value) {
							data[target][field] = value;
						});
					});
				});
			});

			return sequence.then(function() {
				return { layout: layout, data: data };
			});
		});
	},

	render: function(identity) {
		var m = new form.JSONMap(identity.data, _('Hardware identity'));

		this.layout = identity.layout;
		this.data = identity.data;
		this.original = JSON.parse(JSON.stringify(identity.data));
		m.readonly = !L.hasViewPermission();

		Object.keys(identity.layout.targets || {}).forEach(function(target) {
			var fields = identity.layout.targets[target].fields || {};
			var s = m.section(form.NamedSection, target, 'identity', target);

			Object.keys(fields).forEach(function(field) {
				var o = s.option(form.Value, field, labels[field] || field);

				if (fields[field].kind == 'mac')
					o.datatype = 'macaddr';
				o.rmempty = true;
			});
		});

		return m.render();
	},

	handleSave: function() {
		var map = document.querySelector('.cbi-map');
		var self = this;

		return dom.callClassMethod(map, 'save').then(function() {
			var writes = [];
			var results = [];

			Object.keys(self.layout.targets || {}).forEach(function(target) {
				var args = [ 'write', target ];

				Object.keys(self.layout.targets[target].fields || {}).forEach(function(field) {
					var value = self.data[target][field] || '';

					if (value != self.original[target][field])
						args.push(field + '=' + value);
				});
				if (args.length > 2)
					writes.push(args);
			});

			var sequence = Promise.resolve();
			writes.forEach(function(args) {
				sequence = sequence.then(function() {
					return runIdentity(args).then(function(result) { results.push(result); });
				});
			});
			return sequence.then(function() {
				if (writes.length)
					ui.addNotification(null, E('p', [
						_('Hardware identity written. Reboot the device to use the new values.'),
						E('br'), results.join('; ')
					]), 'info');
				self.original = JSON.parse(JSON.stringify(self.data));
			}).catch(function(error) {
				ui.addNotification(null, E('p', [
					_('Hardware identity write failed: %s').format(error.message)
				]), 'danger');
			});
		});
	},

	handleSaveApply: null
});
