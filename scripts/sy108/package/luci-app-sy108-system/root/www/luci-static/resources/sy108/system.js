'use strict';
'require baseclass';
'require fs';
'require ui';
'require poll';

// Observe the reboot through the standard file RPC. No receipt is persisted.
// CP/NV shutdown may outlast LuCI's normal five-second reconnect delay.
function watchReboot(boot, hosts) {
    var started = Date.now();
    function check() {
        if (Date.now() - started > 420000) {
            poll.remove(check);
            ui.hideModal();
            ui.addNotification(
                null, E('p', {}, _('Reboot completion has not been confirmed. Check the device.')),
                'warning');
            return;
        }
        return fs.trimmed('/proc/sys/kernel/random/boot_id')
            .then(function(now) {
                if (now !== boot) {
                    poll.remove(check);
                    ui.awaitReconnect.apply(ui, hosts);
                }
            })
            .catch(function() {
                poll.remove(check);
                ui.awaitReconnect.apply(ui, hosts);
            });
    }
    poll.add(check, 3);
    return function() {
        poll.remove(check);
    };
}
function execute(path, args, hosts) {
    return fs.trimmed('/proc/sys/kernel/random/boot_id').then(function(boot) {
        ui.showModal(_('Preparing system operation'), [
            E('p', {class : 'spinning'},
              _('Stopping services and waiting for the device. This may take several minutes.'))
        ]);
        var cancel = watchReboot(boot, hosts || [ window.location.host ]);
        var timeout = L.env.rpctimeout;
        L.env.rpctimeout = Math.max(timeout || 20, 360);
        return fs.exec(path, args)
            .then(function(res) {
                if (res.code !== 0) {
                    cancel();
                    ui.hideModal();
                    ui.addNotification(
                        null, E('p', {}, res.stderr || _('System operation failed.')), 'warning');
                }
            })
            .catch(function(e) {
                ui.addNotification(null, E('p', {}, e.message), 'warning');
            })
            .finally(function() {
                L.env.rpctimeout = timeout;
            });
    });
}
return baseclass.extend({
    isRecovery : function() {
        return L.resolveDefault(fs.stat('/etc/udx-recovery-root'), {}).then(function(s) {
            return s.type === 'file';
        });
    },
    reboot : function(target) {
        return execute('/usr/libexec/sy108-system', [ 'reboot', target ]);
    },
    handleFirstboot : function() {
        if (!confirm(_(
                'Clear the OpenWrt configuration and reboot? Device SSH keys and factory NV are retained. The login password will be reset to admin.')))
            return;
        return execute('/usr/libexec/sy108-system', [ 'reset' ],
                       [ window.location.host, '192.168.66.1', 'sy108.lan' ]);
    },
    handleRestoreConfirm : function() {
        return fs.exec('/sbin/sysupgrade', [ '--restore-backup', '/tmp/backup.tar.gz' ])
            .then(function(res) {
                if (res.code !== 0)
                    throw new Error(res.stderr || _('Configuration restore failed'));
                return execute('/usr/libexec/sy108-system', [ 'reboot', 'openwrt' ],
                               [ window.location.host, '192.168.66.1', 'sy108.lan' ]);
            })
            .catch(function(e) {
                ui.addNotification(null, E('p', {}, e.message), 'warning');
            });
    },
    handleSysupgradeConfirm : function(btn, opts) {
        var args = [];
        for (var key in opts)
            if (opts[key][0].checked === opts[key][1])
                args.push(opts[key][2]);
        args.push('/tmp/firmware.bin');
        return execute('/sbin/sysupgrade', args,
                       opts.keep[0].checked ? [ window.location.host ]
                                            : [ '192.168.66.1', 'sy108.lan' ]);
    }
});
