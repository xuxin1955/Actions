'use strict';
'require view';
'require form';
'require udx';

return view.extend({
    load : function() {
        return udx.getStatus();
    },
    render : function(initial) {
        var current = initial.cellular || {}, choices = {
            auto : [ 'nr_lte', 'sa_nsa' ],
            sa : [ 'nr_only', 'sa_only' ],
            nsa : [ 'nr_lte', 'nsa_only' ],
            lte : [ 'lte_only', 'sa_nsa' ],
            all : [ 'nr_lte_wcdma_gsm', 'sa_nsa' ]
        };
        function selectedMode(c) {
            return c.mode === 'lte_only'                         ? 'lte'
                   : c.mode === 'nr_only'                        ? 'sa'
                   : c.mode === 'nr_lte_wcdma_gsm'               ? 'all'
                   : (c.nr_capability || {}).mode === 'nsa_only' ? 'nsa'
                                                                 : 'auto';
        }
        var m = new form.JSONMap({
            main : {
                airplane : current.radio_enabled === false ? '1' : '0',
                data : current.automatic_connect ? '1' : '0',
                roaming : current.roaming_allowed ? '1' : '0',
                slot : String(
                    (current.physical_sim || {}).slot == null ? '' : current.physical_sim.slot),
                mode : selectedMode(current)
            }
        },
                                 _('Overview and network control'));
        var s = m.section(form.NamedSection, 'main', 'main', _('Network control'));
        s.anonymous = true;
        var airplane = s.option(form.Flag, 'airplane', _('Airplane mode'));
        var data = s.option(form.Flag, 'data', _('Mobile data'));
        var roaming = s.option(form.Flag, 'roaming', _('Data roaming'));
        var slot = s.option(form.ListValue, 'slot', _('SIM slot'));
        var physical = current.physical_sim || {}, slots = physical.slots || [];
        var activeSlot = slots.find(function(x) {
            return x.slot === physical.slot;
        });
        slot.readonly = !activeSlot || !activeSlot.switch_supported;
        slots
            .filter(function(x) {
                return x.switch_supported || x.slot === physical.slot;
            })
            .forEach(function(x) {
                slot.value(String(x.slot), 'SIM ' + (x.slot + 1));
            });
        if (physical.slot == null)
            slot.value('', '—');
        var mode = s.option(form.ListValue, 'mode', _('Network mode'));
        mode.value('auto', _('5G / 4G automatic (SA + NSA)'));
        mode.value('sa', _('5G SA only'));
        mode.value('nsa', _('5G NSA'));
        mode.value('lte', _('4G LTE only'));
        mode.value('all', _('5G / 4G / 3G / 2G automatic'));
        var apply = s.option(form.Button, '_apply', _('Apply network settings'));
        apply.inputstyle = 'apply';
        apply.inputtitle = _('Apply');
        apply.onclick = function() {
            return udx.notify(function() {
                return m.parse().then(function() {
                    var enabled = airplane.formvalue('main') !== '1',
                        wanted = data.formvalue('main') === '1',
                        roam = roaming.formvalue('main') === '1';
                    var target = choices[mode.formvalue('main')],
                        targetSlot = slot.readonly ? null : Number(slot.formvalue('main'));
                    if (targetSlot != null && !(current.physical_sim.slots || []).some(function(x) {
                            return x.slot === targetSlot && x.switch_supported;
                        }))
                        throw new Error(_('SIM slot'));
                    return udx.execute('policy', {automatic_connect : wanted, roaming : roam})
                        .then(function() {
                            return udx.execute('radio', {enabled : enabled});
                        })
                        .then(function() {
                            if (targetSlot != null &&
                                (current.physical_sim || {}).slot !== targetSlot)
                                return udx.execute('physical_slot', {slot : targetSlot});
                        })
                        .then(function() {
                            if (current.mode !== target[0] ||
                                (current.nr_capability || {}).mode !== target[1])
                                return udx.execute('network_configure',
                                                   {mode : target[0], nr_capability : target[1]});
                        })
                        .then(function() {
                            return udx.execute(wanted && enabled ? 'connect' : 'disconnect',
                                               wanted && enabled ? {automatic : false} : {});
                        });
                });
            });
        };
        var reset = s.option(form.Button, '_restart', _('Restart modem'));
        reset.inputtitle = _('Restart modem');
        reset.inputstyle = 'reset';
        reset.readonly = true;
        reset.description = _('Modem reset is not supported in this version.');
        var refresh = s.option(form.Button, '_refresh', _('Refresh network information'));
        refresh.inputtitle = _('Refresh');
        refresh.onclick = function() {
            return udx.notify(function() {
                return udx.execute('modem_info', {})
                    .then(function() {
                        return udx.execute('radio_options', {});
                    })
                    .then(function() {
                        return udx.execute('cells_refresh', {});
                    });
            });
        };
        function cell(d) {
            return ((d.cellular || {}).cells || [])
                       .filter(function(x) {
                           return x.serving;
                       })
                       .sort(function(a) {
                           return a.rat === 'nr' ? -1 : 1;
                       })[0] ||
                   {};
        }
        function c(d) {
            return d.cellular || {};
        }
        function qos(d, field) {
            var n = (c(d).qos || {})[field];
            return n == null ? null : n / 1000;
        }
        var brands = {
            china_mobile : _('China Mobile'),
            china_unicom : _('China Unicom'),
            china_telecom : _('China Telecom'),
            china_broadnet : _('China Broadnet')
        };
        var states = {
            pdp_active : _('Connected'),
            idle : _('Disconnected'),
            uncertain : _('Status uncertain'),
            waiting_sim : _('Waiting for SIM'),
            waiting_radio : _('Waiting for radio'),
            waiting_registration : _('Searching for network'),
            waiting_attachment : _('Waiting for packet attachment'),
            connecting : _('Connecting')
        };
        var network = udx.table(_('Current network'), [
            [
                _('Operator'),
                function(d) {
                    var op = c(d).operator || {};
                    var name = brands[op.brand] || brands[op.home_brand] || op.name;
                    if (op.plmn)
                        name += ' (' + op.plmn + ')';
                    return name;
                }
            ],
            [
                _('SIM status'),
                function(d) {
                    return (c(d).sim || {}).state;
                }
            ],
            [
                _('Active SIM'),
                function(d) {
                    var n = (c(d).physical_sim || {}).slot;
                    return n == null ? null : 'SIM ' + (n + 1);
                }
            ],
            [
                _('Radio'),
                function(d) {
                    return c(d).cfun === 1                    ? _('On')
                           : [ 0, 4 ].indexOf(c(d).cfun) >= 0 ? _('Off')
                                                              : null;
                }
            ],
            [
                _('Data connection'),
                function(d) {
                    return states[c(d).state] || c(d).state;
                }
            ],
            [
                _('Network mode'),
                function(d) {
                    return ((c(d).mode_choices || {})[c(d).mode] || {}).label || c(d).mode;
                }
            ],
            [
                _('Band'),
                function(d) {
                    var x = cell(d);
                    return x.band || x.band_candidates;
                }
            ],
            [
                'ARFCN',
                function(d) {
                    return cell(d).arfcn;
                }
            ],
            [
                'PCI',
                function(d) {
                    return cell(d).pci;
                }
            ],
            [
                'RSRP',
                function(d) {
                    return cell(d).rsrp;
                },
                ' dBm'
            ],
            [
                'RSRQ',
                function(d) {
                    return cell(d).rsrq;
                },
                ' dB'
            ],
            [
                'SINR',
                function(d) {
                    return cell(d).sinr;
                },
                ' dB'
            ],
            [
                'TAC',
                function(d) {
                    return cell(d).tac;
                }
            ],
            [
                'Cell ID',
                function(d) {
                    return cell(d).cell_id;
                }
            ],
            [
                _('Bandwidth'),
                function(d) {
                    return cell(d).bandwidth_mhz;
                },
                ' MHz'
            ],
            [
                _('AMBR downlink'),
                function(d) {
                    return qos(d, 'ambr_downlink_kbps');
                },
                ' Mbps'
            ],
            [
                _('AMBR uplink'),
                function(d) {
                    return qos(d, 'ambr_uplink_kbps');
                },
                ' Mbps'
            ],
            [
                _('MBR downlink'),
                function(d) {
                    return qos(d, 'mbr_downlink_kbps');
                },
                ' Mbps'
            ],
            [
                _('MBR uplink'),
                function(d) {
                    return qos(d, 'mbr_uplink_kbps');
                },
                ' Mbps'
            ],
            [
                _('GBR downlink'),
                function(d) {
                    return qos(d, 'gbr_downlink_kbps');
                },
                ' Mbps'
            ],
            [
                _('GBR uplink'),
                function(d) {
                    return qos(d, 'gbr_uplink_kbps');
                },
                ' Mbps'
            ],
            [
                'IPv4',
                function(d) {
                    return (c(d).data_session || {}).ipv4;
                }
            ],
            [
                'IPv6',
                function(d) {
                    var a = (d.cellular_kernel || {}).ipv6_global_addresses;
                    return a && a.map(function(x) {
                        return x.address || x;
                    });
                }
            ],
            [
                _('Last error'),
                function(d) {
                    return c(d).last_error || _('None');
                }
            ]
        ]);
        var error = udx.watch(udx.getStatus, function(d) {
            current = d.cellular || {};
            network.update(d);
        }, initial);
        return m.render().then(function(node) {
            return E('div', {}, [ node, error, network.node ]);
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});
