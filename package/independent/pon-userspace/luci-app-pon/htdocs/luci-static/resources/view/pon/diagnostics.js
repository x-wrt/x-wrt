// SPDX-License-Identifier: Apache-2.0

'use strict';
'require fs';
'require form';
'require ui';
'require uci';
'require view';

var HELPER = '/usr/libexec/airoha-pon-debug';
var BUNDLE = '/tmp/pon-debug.tar.gz';

function downloadBundle() {
	return fs.read_direct(BUNDLE, 'blob').then(function(blob) {
		var url = window.URL.createObjectURL(blob);
		var link = document.createElement('a');
		var now = new Date();

		link.style.display = 'none';
		link.href = url;
		link.download = 'pon-debug-' + now.toISOString().replace(/[:.]/g, '-') + '.tar.gz';
		document.body.appendChild(link);
		link.click();
		link.remove();
		window.URL.revokeObjectURL(url);
	});
}

return view.extend({
	handleDownload: function() {
		var button = document.getElementById('pon-debug-download');

		button.disabled = true;
		button.classList.add('spinning');
		return fs.exec_direct(HELPER, [ 'collect' ]).then(downloadBundle).catch(function(error) {
			ui.addNotification(null, E('p', {},
				_('Unable to create diagnostic package: %s').format(error.message)));
		}).finally(function() {
			button.disabled = false;
			button.classList.remove('spinning');
		});
	},

	handleRestart: function() {
		var button = document.getElementById('pon-debug-restart');

		if (!window.confirm(_('The PON line and OMCI/OAM interfaces will be stopped and restarted. Continue?')))
			return;

		button.disabled = true;
		button.classList.add('spinning');
		return fs.exec_direct(HELPER, [ 'restart' ]).then(function(output) {
			var result = JSON.parse(output);

			if (!result.ok)
				throw new Error(_('PON restart failed.'));
			ui.addNotification(null, E('p', {}, _('PON restart completed.')));
		}).catch(function(error) {
			ui.addNotification(null, E('p', {},
				_('Unable to restart PON: %s').format(error.message)));
		}).finally(function() {
			button.disabled = false;
			button.classList.remove('spinning');
		});
	},

	render: function() {
		var m, s, o;

		m = new form.Map('airoha-pon-debug', _('PON diagnostics'),
			_('Debug mode continuously keeps recent raw PON, OMCI and OAM packets in RAM. The setting remains enabled after reboot.'));
		m.chain('pon');
		m.readonly = !L.hasViewPermission();

		s = m.section(form.NamedSection, 'capture', 'capture', _('PON line'));

		o = s.option(form.DummyValue, '_devices', _('PON interface'));
		o.cfgvalue = function() {
			return uci.sections('pon', 'xpon').map(function(line) {
				return line.device || line['.name'];
			}).join(', ') || '-';
		};

		o = s.option(form.Flag, 'enabled', _('Debug mode'));
		o.default = o.disabled;
		o.rmempty = false;
		o.description = _('Enabling starts packet capture before a full PON restart. Disabling stops capture.');

		return m.render().then(L.bind(function(map) {
			return E([], [ map,
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('Diagnostic operations')),
					E('p', {}, _('The package contains raw packet captures, PON state, system logs and network configuration.')),
					E('div', { 'class': 'cbi-page-actions' }, [
						E('button', {
							'class': 'cbi-button cbi-button-action',
							'id': 'pon-debug-download',
							'disabled': m.readonly || null,
							'click': ui.createHandlerFn(this, 'handleDownload')
						}, _('Download diagnostic package')),
						E('button', {
							'class': 'cbi-button cbi-button-negative',
							'id': 'pon-debug-restart',
							'disabled': m.readonly || null,
							'click': ui.createHandlerFn(this, 'handleRestart')
						}, _('Restart PON line'))
					])
				])
			]);
		}, this));
	}
});
