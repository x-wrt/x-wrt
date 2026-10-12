// SPDX-License-Identifier: Apache-2.0

'use strict';
'require fs';
'require form';
'require ui';
'require uci';
'require view';

/* Printable ASCII keeps JavaScript character counts equal to wire byte counts. */
function validatePrintableAscii(value, minimum, maximum) {
	if (value == null || value === '')
		return true;

	if (!/^[\x20-\x7e]+$/.test(value))
		return _('Only printable ASCII characters are allowed.');

	if (value.length < minimum || value.length > maximum) {
		if (minimum === maximum)
			return _('The value must contain exactly %d bytes.').format(maximum);

		return _('The value must contain between %d and %d bytes.')
			.format(minimum, maximum);
	}

	return true;
}

function asciiLength(minimum, maximum) {
	return function(sectionId, value) {
		return validatePrintableAscii(value, minimum, maximum);
	};
}

function validateSerialNumber(sectionId, value) {
	if (value == null || value === '')
		return true;

	if (/^[0-9a-fA-F]{16}$/.test(value) ||
	    /^[A-Za-z0-9]{4}[0-9a-fA-F]{8}$/.test(value))
		return true;

	return _('Use 16 hexadecimal digits or VEND followed by 8 hexadecimal digits.');
}

function validateChipsetId(sectionId, value) {
	if (value == null || value === '' || /^[0-9a-fA-F]{16}$/.test(value))
		return true;

	return _('Use 16 hexadecimal digits.');
}

function isEponMode(mode) {
	return (mode || '').indexOf('epon-') === 0;
}

function sectionUsesEpon(sectionId) {
	var line = uci.get('pon', sectionId, 'line');

	return isEponMode(uci.get('pon', line, 'mode'));
}

function formatStorageSize(bytes) {
	var units = [ 'B', 'KiB', 'MiB', 'GiB' ];
	var size = Number(bytes);
	var unit = 0;

	while (size >= 1024 && unit < units.length - 1) {
		size /= 1024;
		unit++;
	}

	return '%s %s'.format(size >= 10 || unit === 0 ? size.toFixed(0) : size.toFixed(1), units[unit]);
}

function parseStorageList(output) {
	var targets = JSON.parse(output);
	return Object.keys(targets).map(function(name) {
		var target = targets[name];
		return {
			id: name,
			label: '%s · %s · %s'.format(
				name, target.type.toUpperCase(), formatStorageSize(target.size))
		};
	});
}

return view.extend({
	load: function() {
		return fs.exec('/usr/libexec/airoha-pon-data', [ 'list' ]).then(function(result) {
			if (result.code != 0)
				throw new Error(result.stderr || result.stdout || _('Unable to list storage.'));

			return parseStorageList(result.stdout);
		});
	},

	handleBoardDataUpload: function(storages, selector) {
		var storage = storages[Number(selector.value)];

		return ui.uploadFile('/tmp/pon-board-data.bin').then(function() {
			return fs.exec('/usr/libexec/airoha-pon-data', [ 'write', storage.id ]).then(function(result) {
				if (result.code != 0)
					throw new Error(result.stderr || result.stdout || _('Write failed.'));

				var message = [
					_('Board data written to %s and verified. Reboot the device to apply it.')
						.format(storage.label)
				];
				if (result.stdout.trim())
					message.push(E('br'), result.stdout.trim());
				ui.addNotification(null, E('p', message));
			}).catch(function(error) {
				ui.addNotification(null, E('p', [
					_('Writing board data failed: %s').format(error.message)
				]));
			});
		});
	},

	render: function(storages) {
		var m, s, o, selector;

		m = new form.Map('pon', _('PON'),
			_('Project source:') +
			' <a href="https://github.com/pbs05/openwrt-pon-userspace/tree/main/luci-app-pon" target="_blank" rel="noreferrer noopener">openwrt-pon-userspace/luci-app-pon</a>');
		m.readonly = !L.hasViewPermission();

		s = m.section(form.TypedSection, 'xpon', _('PON line'));
		s.anonymous = false;
		s.addremove = false;
		s.description = _('Line mode and registration identity take effect after the PON interface restarts.');

		o = s.option(form.DummyValue, 'device', _('PON interface'));
		o.default = '-';

		o = s.option(form.ListValue, 'mode', _('Line mode'));
		o.default = '';
		o.value('', _('Driver default'));
		o.value('xgpon', _('XG-PON'));
		o.value('xgspon', _('XGS-PON'));
		o.value('epon-10g-1g', _('10G-EPON 10G/1G'));
		o.value('epon-10g-10g', _('10G-EPON 10G/10G'));

		o = s.option(form.Value, 'serial_number', _('Serial number (SN)'));
		o.placeholder = _('Board serial number');
		o.rmempty = true;
		o.validate = validateSerialNumber;
		o.depends('mode', '');
		o.depends('mode', 'xgpon');
		o.depends('mode', 'xgspon');

		o = s.option(form.Value, 'registration_id', _('Registration-ID'));
		o.password = true;
		o.rmempty = true;
		o.depends('mode', '');
		o.depends('mode', 'xgpon');
		o.depends('mode', 'xgspon');
		o.validate = asciiLength(1, 36);
		o.description = _('Optional; sent as all zeros when empty.');

		s = m.section(form.TypedSection, 'omci', _('OMCI settings'));
		s.anonymous = false;
		s.addremove = false;
		s.filter = function(sectionId) {
			return !sectionUsesEpon(sectionId);
		};
		s.tab('authentication', _('Authentication'));
		s.tab('identity', _('ONU identity'));
		s.tab('compatibility', _('Compatibility'));

		o = s.taboption('authentication', form.DummyValue, 'line', _('XG-PON configuration'));
		o.default = '-';

		o = s.taboption('authentication', form.DummyValue, 'device', _('OMCI interface'));
		o.default = '-';

		o = s.taboption('authentication', form.Value, 'loid', _('LOID'));
		o.rmempty = true;
		o.validate = asciiLength(1, 24);

		o = s.taboption('authentication', form.Value, 'loid_password', _('LOID password'));
		o.password = true;
		o.rmempty = true;
		o.validate = asciiLength(1, 12);

		o = s.taboption('identity', form.Value, 'vendor_id', _('Vendor ID'));
		o.rmempty = true;
		o.validate = asciiLength(4, 4);
		o.description = _('Usually the same as the first four characters of the serial number.');

		o = s.taboption('identity', form.Value, 'equipment_id', _('Equipment ID'));
		o.rmempty = true;
		o.validate = asciiLength(1, 20);

		o = s.taboption('identity', form.Value, 'hardware_version', _('Hardware version'));
		o.rmempty = true;
		o.validate = asciiLength(1, 14);

		o = s.taboption('identity', form.Value, 'software_version', _('Software version'));
		o.rmempty = true;
		o.validate = asciiLength(1, 14);

		o = s.taboption('identity', form.Value, 'operator_id', _('Operator ID'));
		o.rmempty = true;
		o.validate = asciiLength(1, 4);

		o = s.taboption('compatibility', form.ListValue, 'omcc_version', _('OMCC version'));
		o.value('0xb0', '0xb0');
		o.value('0x86', '0x86');
		o.default = '0xb0';
		o.rmempty = false;
		o.description = _('If the ONU cannot register with a Huawei OLT, try OMCC version 0x86.');

		o = s.taboption('compatibility', form.Flag, 'disable_enhanced_security', _('Disable enhanced security (Class 332)'));
		o.default = '0';
		o.rmempty = true;

		o = s.taboption('compatibility', form.Value, 'alloc_id_timeout', _('Alloc-ID wait timeout'));
		o.datatype = 'uinteger';
		o.placeholder = '30';
		o.rmempty = true;
		o.description = _('Seconds a data path may wait for PLOAM to assign its Alloc-ID before it is reported as failed. The ONU keeps waiting afterwards; 0 disables the timeout.');

		s = m.section(form.TypedSection, 'oam', _('EPON OAM settings'));
		s.anonymous = false;
		s.addremove = false;
		s.filter = function(sectionId) {
			return sectionUsesEpon(sectionId);
		};
		s.tab('authentication', _('Authentication'));
		s.tab('identity', _('ONU identity'));

		o = s.taboption('authentication', form.DummyValue, 'line', _('EPON configuration'));
		o.default = '-';

		o = s.taboption('authentication', form.DummyValue, 'device', _('OAM interface'));
		o.default = '-';

		o = s.taboption('authentication', form.ListValue, 'operator', _('OAM profile'));
		o.default = 'ctc';
		o.value('ieee', _('IEEE 802.3ah'));
		o.value('ctc', _('IEEE 802.3ah + CTC'));

		o = s.taboption('authentication', form.Value, 'loid', _('LOID'));
		o.rmempty = true;
		o.depends('operator', 'ctc');
		o.validate = asciiLength(1, 24);

		o = s.taboption('authentication', form.Value, 'loid_password', _('LOID password'));
		o.password = true;
		o.rmempty = true;
		o.depends('operator', 'ctc');
		o.validate = asciiLength(1, 12);

		o = s.taboption('identity', form.Value, 'vendor_id', _('Vendor ID'));
		o.rmempty = true;
		o.validate = asciiLength(4, 4);

		o = s.taboption('identity', form.Value, 'model', _('ONU short model'));
		o.rmempty = true;
		o.validate = asciiLength(4, 4);

		o = s.taboption('identity', form.Value, 'equipment_id', _('Equipment ID'));
		o.rmempty = true;
		o.validate = asciiLength(1, 16);

		o = s.taboption('identity', form.Value, 'hardware_version', _('Hardware version'));
		o.rmempty = true;
		o.validate = asciiLength(1, 8);

		o = s.taboption('identity', form.Value, 'software_version', _('Software version'));
		o.rmempty = true;
		o.validate = asciiLength(1, 16);

		o = s.taboption('identity', form.Value, 'firmware_version', _('Firmware version'));
		o.rmempty = true;
		o.validate = asciiLength(1, 127);

		o = s.taboption('identity', form.Value, 'chipset_id', _('Chipset ID'));
		o.rmempty = true;
		o.validate = validateChipsetId;

		o = s.taboption('identity', form.Value, 'ge_ports', _('Ethernet port count'));
		o.default = '1';
		o.datatype = 'range(1,64)';

		return m.render().then(L.bind(function(map) {
			if (!storages.length)
				return map;

			selector = E('select', { 'class': 'cbi-input-select' },
				storages.map(function(storage, index) {
					return E('option', { 'value': String(index) }, storage.label);
				}));

			return E([], [ map,
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('PON board data')),
					E('div', { 'class': 'cbi-value' }, [
						E('label', { 'class': 'cbi-value-title' }, _('Target storage')),
						E('div', { 'class': 'cbi-value-field' }, [
							selector,
							E('button', {
								'class': 'cbi-button cbi-button-action',
								'disabled': m.readonly || null,
								'click': ui.createHandlerFn(this, 'handleBoardDataUpload', storages, selector)
							}, [ _('Upload and write') ])
						])
					])
				])
			]);
		}, this));
	}
});
