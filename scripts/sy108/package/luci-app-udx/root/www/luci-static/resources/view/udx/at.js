'use strict';
'require view';
'require form';
'require udx';

return view.extend({
    load : function() {
        return Promise.resolve();
    },
    render : function() {
        var m = new form.JSONMap(
            {main : {identity : '0', imei : '', command : 'AT', timeout : '10', shortcut : 'AT'}},
            _('AT terminal and IMEI'));
        var s = m.section(form.NamedSection, 'main', 'main');
        s.anonymous = true;
        s.tab('at', _('AT terminal'));
        s.tab('imei', _('IMEI'));
        function action(tab, key, label, fn) {
            var o = s.taboption(tab, form.Button, key, label);
            o.inputtitle = label;
            o.inputstyle = 'action';
            o.onclick = function() {
                return udx.notify(fn);
            };
            return o;
        }
        var command = s.taboption('at', form.Value, 'command', _('AT command'));
        command.rmempty = false;
        var timeout = s.taboption('at', form.Value, 'timeout', _('Timeout'));
        timeout.datatype = 'range(1,120)';
        var output = E('pre', {'aria-live' : 'polite'});
        var response = s.taboption('at', form.DummyValue, '_response', _('Response'));
        response.renderWidget = function() {
            return output;
        };
        function run(wire) {
            output.textContent = wire + '\n';
            return udx
                .execute('at_debug', {command : wire, timeout : Number(timeout.formvalue('main'))})
                .then(function(r) {
                    output.textContent = wire + '\n' + (r.response || '');
                    return r;
                })
                .catch(function(e) {
                    output.textContent =
                        wire + '\n' + ((e.result || {}).response || '') + '\n' + e.message;
                    throw e;
                });
        }
        action('at', '_send', _('Execute AT command'), function() {
            return run(command.formvalue('main') || '');
        });
        var shortcut = s.taboption('at', form.ListValue, 'shortcut', _('Quick command'));
        ['AT', 'AT+CFUN?', 'AT+CPIN?', 'AT+COPS?', 'AT+CSQ', 'AT+SPIMEI?', 'AT+CCID', 'AT+CIMI',
         'AT+SPLBAND=0', 'AT+SPLBAND=3', 'AT+SPFORCEFRQ=12,3', 'AT+SPFORCEFRQ=16,3', 'AT+CGDCONT?',
         'AT+CGEQOSRDP']
            .forEach(function(x) {
                shortcut.value(x);
            });
        action('at', '_shortcut', _('Execute quick command'), function() {
            return run(shortcut.formvalue('main'));
        });
        var identity = s.taboption('imei', form.ListValue, 'identity', _('IMEI index'));
        identity.value('', '—');
        identity.readonly = true;
        var availableIdentities = [];
        function updateIdentities(r) {
            availableIdentities = r.identities || [];
            identities.update(availableIdentities);
            var selected = availableIdentities.find(function(x) {
                return String(x.slot) === identity.formvalue('main');
            }) || availableIdentities[0];
            identity.readonly = availableIdentities.length < 2;
            var choices = availableIdentities.map(function(x) {
                return [ String(x.slot), 'IMEI ' + x.slot ];
            });
            if (!choices.length)
                choices = [ [ '', '—' ] ];
            return udx.setChoices(identity, choices, selected ? selected.slot : '')
                .then(function() {
                    if (selected)
                        imei.getUIElement('main').setValue(selected.imei);
                    return r;
                });
        }
        var imei = s.taboption('imei', form.Value, 'imei', 'IMEI');
        imei.validate = function(section, value) {
            if (!/^\d{15}$/.test(value || ''))
                return _('Enter a 15-digit IMEI');
            var sum = 0;
            for (var i = 0; i < 15; i++) {
                var n = Number(value[i]) * (i % 2 ? 2 : 1);
                sum += Math.floor(n / 10) + n % 10;
            }
            return sum % 10 ? _('IMEI Luhn check failed') : true;
        };
        var identities = udx.list(
            [
                [
                    _('Identity'),
                    function(r) {
                        return 'IMEI ' + r.slot;
                    }
                ],
                [
                    'IMEI',
                    function(r) {
                        return r.imei;
                    }
                ]
            ],
            function(r) {
                return r.slot;
            },
            false);
        var identityTable = s.taboption('imei', form.DummyValue, '_identities', _('Readback'));
        identityTable.renderWidget = function() {
            return identities.node;
        };
        action('imei', '_read_imei', _('Read IMEI'), function() {
            return udx.execute('imei_read', {}).then(updateIdentities);
        });
        action('imei', '_write_imei', _('Write IMEI'), function() {
            if (!availableIdentities.some(function(x) {
                    return String(x.slot) === identity.formvalue('main');
                }))
                throw new Error(_('Read IMEI'));
            return udx
                .execute('imei_write', {
                    slot : Number(identity.formvalue('main')),
                    imei : imei.formvalue('main') || ''
                })
                .then(updateIdentities);
        });
        return m.render();
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});
