'use strict';
'require view';
'require form';
'require uci';
'require ui';
'require udx';

return view.extend({
    load : function() {
        return Promise.all([ udx.getStatus(), uci.load('udx-usb'), udx.frequencyStatus() ]);
    },
    render : function(initial) {
        var clocks = new form.JSONMap({main : {}});
        var f = clocks.section(form.NamedSection, 'main', 'frequency');
        f.anonymous = true;
        var panels = {system : [], sensors : [], storage : [], usb : []};
        function embed(tab, node) {
            panels[tab].push(node);
        }
        function mib(v) {
            return v == null ? null : (v / 1048576).toFixed(1) + ' MiB';
        }
        function mhz(v) {
            return v == null ? null : (v / 1000000).toFixed(0) + ' MHz';
        }
        var board = udx.table(_('Board information'), [
            [
                _('Model'),
                function(d) {
                    return (d.board || {}).model;
                }
            ],
            [
                _('Board name'),
                function(d) {
                    return (d.board || {}).board_name;
                }
            ],
            [
                _('System'),
                function(d) {
                    return (d.board || {}).system;
                }
            ],
            [
                _('Hostname'),
                function(d) {
                    return (d.board || {}).hostname;
                }
            ],
            [
                _('Firmware version'),
                function(d) {
                    return ((d.board || {}).release || {}).description;
                }
            ],
            [
                _('Kernel'),
                function(d) {
                    return (d.board || {}).kernel;
                }
            ],
            [
                _('Target'),
                function(d) {
                    return ((d.board || {}).release || {}).target;
                }
            ],
            [
                _('Uptime'),
                function(d) {
                    return Math.floor((d.system_info || {}).uptime || d.sample_uptime || 0);
                },
                ' s'
            ],
            [
                _('Load average'),
                function(d) {
                    return ((d.system_info || {}).load || []).map(function(x) {
                        return (x / 65536).toFixed(2);
                    });
                }
            ]
        ]);
        embed('system', board.node);
        var memory = udx.table(_('Memory'), [
            [
                _('Total'),
                function(d) {
                    return mib(d.memory.total);
                }
            ],
            [
                _('Available memory'),
                function(d) {
                    return mib(d.memory.available);
                }
            ],
            [
                _('Free'),
                function(d) {
                    return mib(d.memory.free);
                }
            ],
            [
                _('Buffered'),
                function(d) {
                    return mib(d.memory.buffered);
                }
            ],
            [
                _('Page cache'),
                function(d) {
                    return mib(d.memory.cached);
                }
            ],
            [
                _('Swap total'),
                function(d) {
                    return mib((d.swap || {}).total);
                }
            ],
            [
                _('Swap free'),
                function(d) {
                    return mib((d.swap || {}).free);
                }
            ]
        ]);
        embed('system', memory.node);
        var runtime = udx.table(_('Platform services'), [
            [
                _('Core service'),
                function(d) {
                    return (d.runtime || {}).core;
                }
            ],
            [
                _('Modem service'),
                function(d) {
                    return (d.runtime || {}).baseband;
                }
            ],
            [
                _('CP/NV error'),
                function(d) {
                    return (d.runtime || {}).cpnv_error;
                }
            ],
            [
                _('Runtime error'),
                function(d) {
                    return (d.runtime || {}).error || _('None');
                }
            ]
        ]);
        embed('system', runtime.node);
        var temperatures = udx.list(
            [
                [
                    _('Sensor'),
                    function(r) {
                        return r.name;
                    }
                ],
                [
                    _('Temperature'),
                    function(r) {
                        return udx.value(r.temperature_c, ' °C');
                    }
                ]
            ],
            function(r) {
                return r.id || r.name;
            },
            false);
        embed('sensors', E('section', {class : 'cbi-section'},
                           [ E('h3', {}, _('Temperatures')), temperatures.node ]));
        var cpus = udx.list(
            [
                [
                    _('CPU policy'),
                    function(r) {
                        return r.policy;
                    }
                ],
                [
                    _('Current frequency'),
                    function(r) {
                        return r.current_khz == null ? null : r.current_khz / 1000 + ' MHz';
                    }
                ],
                [
                    _('Governor'),
                    function(r) {
                        return r.governor;
                    }
                ],
                [
                    _('Minimum frequency'),
                    function(r) {
                        return r.min_khz == null ? null : r.min_khz / 1000 + ' MHz';
                    }
                ],
                [
                    _('Maximum frequency'),
                    function(r) {
                        return r.max_khz == null ? null : r.max_khz / 1000 + ' MHz';
                    }
                ]
            ],
            function(r) {
                return r.policy;
            },
            false);
        embed('sensors',
              E('section', {class : 'cbi-section'}, [ E('h3', {}, _('CPU clocks')), cpus.node ]));
        var ddr = udx.list(
            [
                [
                    _('Device'),
                    function(r) {
                        return r.name;
                    }
                ],
                [
                    _('Current frequency'),
                    function(r) {
                        return mhz(r.current_hz);
                    }
                ],
                [
                    _('Available frequencies'),
                    function(r) {
                        return (r.available_frequencies || '').trim().split(/\s+/)
                            .filter(Boolean).map(function(hz) { return mhz(+hz); }).join(', ');
                    }
                ]
            ],
            function(r) {
                return r.name;
            },
            false);
        embed('sensors',
              E('section', {class : 'cbi-section'}, [ E('h3', {}, _('DDR clocks')),
                  E('p', {}, _('DDR is managed automatically by firmware. Monitoring only.')), ddr.node ]));
        var frequency = initial[2] || {}, cpuPolicy = frequency.cpu || {};
        function setting(key, title, options, selected, numeric) {
            var o = f.option(form.ListValue, key, title);
            options.forEach(function(v) {
                o.value(String(v), numeric ? (v / numeric) + ' MHz' : v);
            });
            o.cfgvalue = function() {
                return selected == null ? null : String(selected);
            };
            o.rmempty = false;
            return o;
        }
        var cpuGovernor = setting('_cpu_governor', _('CPU governor'), cpuPolicy.governors || [],
                                  cpuPolicy.governor);
        var cpuMinimum = setting('_cpu_minimum', _('CPU minimum frequency'),
                                 cpuPolicy.frequencies_khz || [], cpuPolicy.min_khz, 1000);
        var cpuMaximum = setting('_cpu_maximum', _('CPU maximum frequency'),
                                 cpuPolicy.frequencies_khz || [], cpuPolicy.max_khz, 1000);
        cpuMaximum.description = _('Set the same minimum and maximum for a fixed CPU frequency.');
        var frequencyApply =
            f.option(form.Button, '_frequency_apply', _('Apply CPU frequency settings'));
        frequencyApply.inputtitle = _('Apply');
        frequencyApply.inputstyle = 'apply';
        frequencyApply.description = _('Settings are saved and restored at startup.');
        frequencyApply.onclick = function() {
            return udx.notify(function() {
                if (+cpuMinimum.formvalue('main') > +cpuMaximum.formvalue('main'))
                    throw new Error(_('Minimum frequency must not exceed maximum frequency.'));
                return udx.frequencyApply(
                    cpuGovernor.formvalue('main'), cpuMinimum.formvalue('main'),
                    cpuMaximum.formvalue('main'));
            });
        };
        var boot = udx.table(_('Boot information'), [
            [
                _('Running system'),
                function(d) {
                    return (d.boot || {}).target;
                }
            ],
            [
                _('Boot policy'),
                function(d) {
                    return (d.boot || {}).policy;
                }
            ],
            [
                _('Root filesystem'),
                function(d) {
                    return (d.board || {}).rootfs_type;
                }
            ],
            [
                _('Root marker'),
                function(d) {
                    return (d.boot || {}).root_marker;
                }
            ]
        ]);
        embed('storage', boot.node);
        var filesystems = udx.list(
            [
                [
                    _('Mount point'),
                    function(r) {
                        return r.mount;
                    }
                ],
                [
                    _('Filesystem'),
                    function(r) {
                        return r.type;
                    }
                ],
                [
                    _('Total'),
                    function(r) {
                        return mib(r.total_bytes);
                    }
                ],
                [
                    _('Available'),
                    function(r) {
                        return mib(r.free_bytes);
                    }
                ],
                [
                    _('Read only'),
                    function(r) {
                        return r.readonly;
                    }
                ]
            ],
            function(r) {
                return r.mount;
            },
            false);
        embed('storage', E('section', {class : 'cbi-section'},
                           [ E('h3', {}, _('Filesystems')), filesystems.node ]));
        var volumes = udx.list(
            [
                [
                    _('Volume'),
                    function(r) {
                        return r.name;
                    }
                ],
                [
                    _('Device'),
                    function(r) {
                        return r.device;
                    }
                ],
                [
                    _('Type'),
                    function(r) {
                        return r.type;
                    }
                ],
                [
                    _('Data size'),
                    function(r) {
                        return mib(r.data_bytes);
                    }
                ],
                [
                    _('Reserved eraseblocks'),
                    function(r) {
                        return r.reserved_ebs;
                    }
                ]
            ],
            function(r) {
                return r.device;
            },
            false);
        embed('storage', E('section', {class : 'cbi-section'},
                           [ E('h3', {}, _('UBI volumes')), volumes.node ]));
        var usbModes = [
            [ 'ncm', _('NCM (hardware accelerated)'), _('Works on Windows, Linux and macOS. Fastest.') ],
            [ 'ncm-software', _('NCM (software mode)'), _('Use when the host has trouble with accelerated NCM.') ],
            [ 'ecm', _('CDC-ECM'), _('Native support on Linux and macOS.') ],
            [ 'rndis', _('RNDIS'), _('Compatibility mode for Windows, especially older releases.') ]
        ];
        var usbCfg = initial[1] || {};
        var usbMain = usbCfg['main'] || {};
        var usbSelect = E('select', { 'class' : 'cbi-input-select' },
            usbModes.map(function(m) {
                return E('option', {
                    'value' : m[0],
                    'selected' : (m[0] === (usbMain['profile'] || 'rndis')) ? 'selected' : null
                }, [ m[1] ]);
            }));
        var usbHint = E('div', { 'class' : 'cbi-value-description' }, [ '' ]);
        var usbAcm = E('input', {
            'type' : 'checkbox',
            'class' : 'cbi-input-checkbox',
            'checked' : (usbMain['acm'] === '1') ? 'checked' : null
        });
        function usbRefreshHint() {
            var sel = usbModes.filter(function(m) { return m[0] === usbSelect.value; })[0];
            usbHint.textContent = sel ? sel[2] : '';
        }
        usbSelect.addEventListener('change', usbRefreshHint);
        usbRefreshHint();
        var usbApply = E('button', {
            'class' : 'cbi-button cbi-button-action important',
            'click' : ui.createHandlerFn(this, function() {
                if (!confirm(_('Switch the USB network mode to %s? The device reboots and the USB link drops for about a minute.').format(usbSelect.value)))
                    return;
                return udx.notify(function() {
                    return udx.usb(usbSelect.value, !!usbAcm.checked);
                });
            })
        }, [ _('Apply and reboot') ]);
        embed('usb', E('section', {class : 'cbi-section'}, [
            E('h3', {}, _('USB network mode')),
            E('div', {class : 'cbi-value'}, [
                E('label', {class : 'cbi-value-title'}, [ _('USB mode') ]),
                E('div', {class : 'cbi-value-field'}, [ usbSelect, ' ', usbApply ]),
                usbHint
            ]),
            E('div', {class : 'cbi-value'}, [
                E('label', {class : 'cbi-value-title'}, [ _('Authenticated ACM console') ]),
                E('div', {class : 'cbi-value-field'}, [
                    usbAcm,
                    E('span', {style : 'margin-left:8px'}, [ _('Adds a second serial channel for debug console access.') ])
                ])
            ])
        ]));
        var usb = udx.table(_('USB status'), [
            [
                _('Mode'),
                function(d) {
                    return (d.usb || {}).profile;
                }
            ],
            [
                _('ACM console'),
                function(d) {
                    var r = (d.usb || {}).acm;
                    return r == null ? null : r === '1';
                }
            ],
            [
                'UDC',
                function(d) {
                    return (d.usb || {}).udc;
                }
            ],
            [
                _('Connection'),
                function(d) {
                    return (d.usb || {}).state;
                }
            ],
            [
                _('Speed'),
                function(d) {
                    return (d.usb || {}).speed;
                }
            ],
            [
                _('Hardware acceleration'),
                function(d) {
                    var r = (d.usb || {}).offload_state;
                    return r == null ? null : /\brunning=1\b/.test(r);
                }
            ],
            [
                _('Device MAC'),
                function(d) {
                    return (d.usb || {}).dev_mac;
                }
            ],
            [
                _('Host MAC'),
                function(d) {
                    return (d.usb || {}).host_mac;
                }
            ],
            [
                _('Serial number'),
                function(d) {
                    return (d.usb || {}).serial;
                }
            ],
            [
                _('Apply result'),
                function(d) {
                    return (d.usb || {}).result;
                }
            ]
        ]);
        embed('usb', usb.node);
        var error = udx.watch(udx.getStatus, function(d) {
            board.update(d);
            memory.update(d);
            runtime.update(d);
            boot.update(d);
            usb.update(d);
            temperatures.update(d.thermal || []);
            cpus.update(d.cpu || []);
            ddr.update(d.ddr || []);
            filesystems.update(d.storage || []);
            volumes.update(d.ubi || []);
        }, initial[0]);
        return clocks.render().then(function(form) {
            panels.sensors.push(form);
            var titles = {
                system : _('Board and system'),
                sensors : _('Sensors and clocks'),
                storage : _('Boot and storage'),
                usb : _('USB network mode')
            };
            var tabs = E('div', {}, Object.keys(panels).map(function(name) {
                return E('div', {'data-tab' : name, 'data-tab-title' : titles[name]}, panels[name]);
            }));
            var page = E('div', {}, [ E('h2', {}, _('Hardware and platform')), tabs, error ]);
            ui.tabs.initTabGroup(tabs.childNodes);
            return page;
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});
