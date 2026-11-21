'use strict';
'require form';
'require network';

return network.registerProtocol('udx', {
    getI18n : function() {
        return _('UDX710 cellular');
    },
    getIfname : function() {
        return this._ubus('l3_device') || 'wwan0';
    },
    getPackageName : function() {
        return 'udx-cellular';
    },
    isFloating : function() {
        return true;
    },
    isVirtual : function() {
        return false;
    },
    getDevices : function() {
        return null;
    },
    containsDevice : function(ifname) {
        return network.getIfnameOf(ifname) === this.getIfname();
    },
    renderFormOptions : function(section) {
        var device = section.taboption('general', form.ListValue, 'device', _('Device'));
        device.value('wwan0', 'wwan0');
        device.default = 'wwan0';
        device.rmempty = false;
        var info = section.taboption('general', form.DummyValue, '_cellular_settings',
                                     _('Cellular settings'));
        info.rawhtml = true;
        info.cfgvalue = function() {
            return E('a', {href : L.url('admin', 'udx', 'overview')}, _('Open network management'));
        };
        info.description = _(
            'The modem supplies the IP configuration. Manage the APN and radio settings in UDX710; netifd manages this network interface.');
        var metric = section.taboption('advanced', form.Value, 'metric', _('Use gateway metric'));
        metric.datatype = 'uinteger';
        metric.placeholder = '50';
    }
});
