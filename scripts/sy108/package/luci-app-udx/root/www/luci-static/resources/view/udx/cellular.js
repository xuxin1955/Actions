'use strict';
'require view';
'require form';
'require udx';

return view.extend({
    load : function() {
        return Promise.all([ udx.getStatus(), L.resolveDefault(udx.getAPN(), {}) ]);
    },
    render : function(initial) {
        var current = initial[0].cellular || {}, apnState = initial[1],
            templates = apnState.templates || [], catalog = [];
        var limits = current.band_limits || {}, profile = current.profile || {},
            auth = current.auth || {};
        var m = new form.JSONMap({
            main : {
                lte : (limits.lte || {}).unrestricted ? [] : (limits.lte || {}).bands || [],
                nr : (limits.nr || {}).unrestricted ? [] : (limits.nr || {}).bands || [],
                apn_mode : current.apn_mode || 'auto',
                apn : profile.apn || '',
                pdp_type : profile.pdp_type || 'IPV4V6',
                username : auth.username || '',
                password : '',
                auth_method : auth.auth_method || 'none',
                template : '0',
                template_name : '',
                provider : '',
                plmn : '',
                act : '7'
            }
        },
                                 _('Bands, cells and APN'));
        var s = m.section(form.NamedSection, 'main', 'main');
        s.anonymous = true;
        s.tab('bands', _('Band locking'));
        s.tab('cells', _('Serving and neighboring cells'));
        s.tab('operators', _('Operators'));
        s.tab('apn', _('APN settings'));
        function field(tab, kind, name, label) {
            return s.taboption(tab, kind, name, label);
        }
        function action(tab, name, label, fn) {
            var o = field(tab, form.Button, name, label);
            o.inputtitle = label;
            o.inputstyle = 'action';
            o.onclick = function() {
                return udx.notify(fn);
            };
            return o;
        }
        function embed(tab, name, node) {
            var o = field(tab, form.DummyValue, name, '');
            o.renderWidget = function() {
                return node;
            };
            return o;
        }
        var lte = field('bands', form.MultiValue, 'lte', _('LTE bands'));
        ['B1', 'B3', 'B5', 'B8', 'B34', 'B38', 'B39', 'B40', 'B41'].forEach(function(b) {
            lte.value(b);
        });
        var nr = field('bands', form.MultiValue, 'nr', _('NR bands'));
        ['n1', 'n3', 'n5', 'n8', 'n28', 'n41', 'n77', 'n78', 'n79'].forEach(function(b) {
            nr.value(b);
        });
        lte.description = nr.description = _('Leave empty for all bands.');
        action('bands', '_lock', _('Apply bands'), function() {
            return udx.execute(
                'band_lock', {lte : lte.formvalue('main') || [], nr : nr.formvalue('main') || []});
        });
        action('bands', '_unlock', _('Unlock all bands'), function() {
            return udx.execute('unlock', {scope : 'bands'});
        });
        var bandTable = udx.table(_('Applied bands'), [
            [
                'LTE',
                function(c) {
                    var r = (c.band_limits || {}).lte || {};
                    return r.unrestricted ? _('Unrestricted') : r.bands;
                }
            ],
            [
                'NR',
                function(c) {
                    var r = (c.band_limits || {}).nr || {};
                    return r.unrestricted ? _('Unrestricted') : r.bands;
                }
            ]
        ]);
        embed('bands', '_band_status', bandTable.node);
        action('bands', '_read_bands', _('Read band and cell locks'), function() {
            return udx.execute('advanced_refresh', {});
        });
        var cellList = udx.list(
            [
                [
                    _('Type'),
                    function(r) {
                        return r.rat.toUpperCase() + ' ' +
                               (r.serving ? _('Serving') : _('Neighbor'));
                    }
                ],
                [
                    _('Band'),
                    function(r) {
                        return r.band || r.band_candidates;
                    }
                ],
                [
                    'ARFCN',
                    function(r) {
                        return r.arfcn;
                    }
                ],
                [
                    'PCI',
                    function(r) {
                        return r.pci;
                    }
                ],
                [
                    'RSRP',
                    function(r) {
                        return udx.value(r.rsrp, ' dBm');
                    }
                ],
                [
                    'RSRQ',
                    function(r) {
                        return udx.value(r.rsrq, ' dB');
                    }
                ],
                [
                    'SINR',
                    function(r) {
                        return udx.value(r.sinr, ' dB');
                    }
                ],
                [
                    'TAC',
                    function(r) {
                        return r.tac;
                    }
                ],
                [
                    'Cell ID',
                    function(r) {
                        return r.cell_id;
                    }
                ],
                [
                    _('Bandwidth'),
                    function(r) {
                        return udx.value(r.bandwidth_mhz, ' MHz');
                    }
                ]
            ],
            function(r) {
                return [ r.rat, r.serving, r.arfcn, r.pci ].join(':');
            },
            false,
            function(get) {
                return udx.button(_('Lock cell'), function() {
                    var r = get();
                    return udx.execute('cell_lock', {rat : r.rat, arfcn : r.arfcn, pci : r.pci});
                });
            });
        embed('cells', '_cells', cellList.node);
        var cellLocks = udx.table(_('Cell locks'), [ 'lte', 'nr' ].map(function(rat) {
            return [
                rat.toUpperCase(),
                function(c) {
                    var r = (c.cell_locks || {})[rat] || {};
                    return r.error || (r.locked === false ? _('Unlocked')
                                       : r.locked         ? r.arfcn + ' / PCI ' + r.pci
                                                          : null);
                }
            ];
        }));
        embed('cells', '_cell_locks', cellLocks.node);
        ['lte', 'nr'].forEach(function(rat) {
            action('cells', '_unlock_' + rat, _('Unlock') + ' ' + rat.toUpperCase(), function() {
                return udx.execute('unlock', {scope : 'cells', rat : rat});
            });
        });
        action('cells', '_unlock_cells', _('Unlock all cells'), function() {
            return udx.execute('unlock', {scope : 'cells', rat : 'lte'}).then(function() {
                return udx.execute('unlock', {scope : 'cells', rat : 'nr'});
            });
        });
        action('cells', '_read_cells', _('Refresh cells'), function() {
            return udx.execute('cells_refresh', {});
        });
        var operators = udx.list(
            [
                [
                    _('Operator'),
                    function(r) {
                        return r.name || r.short_name || r.plmn;
                    }
                ],
                [
                    'PLMN',
                    function(r) {
                        return r.plmn;
                    }
                ],
                [
                    _('Technology'),
                    function(r) {
                        return r.act;
                    }
                ],
                [
                    _('Status'),
                    function(r) {
                        return [ _('Unknown'), _('Available'), _('Current'),
                                 _('Forbidden') ][r.status];
                    }
                ]
            ],
            function(r) {
                return r.plmn + ':' + r.act;
            },
            false,
            function(get) {
                return udx.button(_('Register'), function() {
                    var r = get();
                    return udx.execute('operator_select', {plmn : r.plmn, act : r.act});
                });
            });
        action('operators', '_scan', _('Scan operators'), function() {
            return udx.execute('operators_scan', {}).then(function(r) {
                operators.update(r.operators);
                return r;
            });
        });
        action('operators', '_auto', _('Automatic operator selection'), function() {
            return udx.execute('operator_auto', {});
        });
        embed('operators', '_operators', operators.node);
        var plmn = field('operators', form.Value, 'plmn', 'MCC/MNC');
        plmn.datatype = 'and(uinteger,minlength(5),maxlength(6))';
        var act = field('operators', form.ListValue, 'act', _('Technology'));
        act.value('7', 'LTE');
        act.value('11', 'NR');
        act.value('13', 'NR + LTE');
        action('operators', '_register', _('Register manually'), function() {
            return udx.execute(
                'operator_select',
                {plmn : plmn.formvalue('main'), act : Number(act.formvalue('main'))});
        });
        var apnMode = field('apn', form.ListValue, 'apn_mode', _('APN mode'));
        apnMode.value('auto', _('Automatic'));
        apnMode.value('manual', _('Manual'));
        var apn = field('apn', form.Value, 'apn', 'APN');
        apn.depends('apn_mode', 'manual');
        var pdp = field('apn', form.ListValue, 'pdp_type', _('PDP type'));
        ['IPV4V6', 'IP', 'IPV6'].forEach(function(x) {
            pdp.value(x);
        });
        pdp.depends('apn_mode', 'manual');
        var username = field('apn', form.Value, 'username', _('Username'));
        username.depends('apn_mode', 'manual');
        var password = field('apn', form.Value, 'password', _('Password'));
        password.password = true;
        password.depends('apn_mode', 'manual');
        password.placeholder = auth.password_set ? _('Unchanged') : '';
        var method = field('apn', form.ListValue, 'auth_method', _('Authentication'));
        method.value('none', _('None'));
        method.value('pap', 'PAP');
        method.value('chap', 'CHAP');
        method.depends('apn_mode', 'manual');
        action('apn', '_save_apn', _('Apply APN'), function() {
            if (apnMode.formvalue('main') === 'auto')
                return udx.execute('apn_select', {mode : 'auto', template_id : 0});
            return udx.execute('apn', {
                apn : apn.formvalue('main') || '',
                pdp_type : pdp.formvalue('main'),
                cid : 1,
                username : username.formvalue('main') || '',
                password : password.formvalue('main') || null,
                auth_method : method.formvalue('main')
            });
        });
        var template = field('apn', form.ListValue, 'template', _('Saved template'));
        template.value('0', _('Select a template'));
        templates.forEach(function(t) {
            template.value(String(t.id), t.name + ' — ' + t.apn);
        });
        function updateTemplates(r) {
            templates = (r.apn || {}).templates || [];
            var selected = r.saved_id != null ? String(r.saved_id) : template.formvalue('main');
            if (!templates.some(function(t) {
                    return String(t.id) === selected;
                }))
                selected = '0';
            return udx
                .setChoices(template,
                            [ [ '0', _('Select a template') ] ].concat(templates.map(function(t) {
                                return [ String(t.id), t.name + ' — ' + t.apn ];
                            })),
                            selected)
                .then(function() {
                    return r;
                });
        }
        action('apn', '_use_template', _('Use template'), function() {
            return udx.execute('apn_select',
                               {mode : 'manual', template_id : Number(template.formvalue('main'))});
        });
        action('apn', '_delete_template', _('Delete template'), function() {
            return udx.execute('apn_template_delete', {id : Number(template.formvalue('main'))})
                .then(updateTemplates);
        });
        var name = field('apn', form.Value, 'template_name', _('New template name'));
        action('apn', '_save_template', _('Save as template'), function() {
            return udx
                .execute('apn_template_save', {
                    id : 0,
                    name : name.formvalue('main') || '',
                    apn : apn.formvalue('main') || '',
                    pdp_type : pdp.formvalue('main'),
                    username : username.formvalue('main') || '',
                    password : password.formvalue('main') || '',
                    auth_method : method.formvalue('main')
                })
                .then(updateTemplates);
        });
        var providerList = udx.list(
            [
                [
                    _('Operator'),
                    function(r) {
                        return r.name;
                    }
                ],
                [
                    'MCC/MNC',
                    function(r) {
                        return r.plmn;
                    }
                ],
                [
                    'APN',
                    function(r) {
                        return r.apn;
                    }
                ]
            ],
            function(r) {
                return r.plmn + ':' + r.apn;
            },
            false,
            function(get) {
                return udx.button(_('Use APN'), function() {
                    var r = get();
                    return udx.execute('apn', {
                        apn : r.apn,
                        pdp_type : r.pdp_type,
                        cid : 1,
                        username : r.username || '',
                        password : r.password || '',
                        auth_method : r.auth_method || 'none'
                    });
                });
            });
        action('apn', '_providers', _('Read SIM operator APNs'), function() {
            return udx.execute('operator_catalog', {}).then(function(r) {
                catalog = r.home_candidates || [];
                providerList.update(catalog);
                return r;
            });
        });
        embed('apn', '_provider_table', providerList.node);
        var errors = E('p');
        embed('cells', '_query_errors', errors);
        var error = udx.watch(udx.getStatus, function(d) {
            current = d.cellular || {};
            bandTable.update(current);
            cellLocks.update(current);
            cellList.update(current.cells || []);
            errors.textContent = Object.keys(current.cell_query_errors || {})
                                     .map(function(k) {
                                         return k + ': ' + current.cell_query_errors[k];
                                     })
                                     .join('; ');
        }, initial[0]);
        return m.render().then(function(node) {
            return E('div', {}, [ node, error ]);
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});
