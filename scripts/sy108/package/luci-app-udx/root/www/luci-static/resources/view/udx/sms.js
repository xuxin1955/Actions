'use strict';
'require view';
'require form';
'require udx';

return view.extend({
    load : function() {
        return Promise.all(
            [ L.resolveDefault(udx.getHistory(), {}), L.resolveDefault(udx.getSMS(), {}) ]);
    },
    render : function(initial) {
        var settings = initial[0].settings || {}, pending = null, receipt = E('p'),
            storageKey = 'udx-sms-pending-request';
        try {
            pending = sessionStorage.getItem(storageKey);
            if (!/^[0-9a-f]{32}$/.test(pending || ''))
                pending = null;
        } catch (e) {
        }
        function remember(id) {
            pending = id;
            try {
                if (id)
                    sessionStorage.setItem(storageKey, id);
                else
                    sessionStorage.removeItem(storageKey);
            } catch (e) {
            }
        }
        var m = new form.JSONMap({
            main : {
                number : '',
                text : '',
                max_count : String(settings.max_count || 50),
                max_sent_count : String(settings.max_sent_count || 10),
                direct_receive : settings.direct_receive ? '1' : '0'
            }
        },
                                 _('SMS management'));
        var s = m.section(form.NamedSection, 'main', 'main');
        s.anonymous = true;
        s.tab('inbox', _('Inbox'));
        s.tab('sent', _('Sent'));
        s.tab('storage', _('Modem storage'));
        s.tab('compose', _('Send message'));
        s.tab('settings', _('Settings'));
        function embed(tab, key, node) {
            var o = s.taboption(tab, form.DummyValue, key, '');
            o.renderWidget = function() {
                return node;
            };
        }
        function action(tab, key, label, fn) {
            var o = s.taboption(tab, form.Button, key, label);
            o.inputtitle = label;
            o.inputstyle = 'action';
            o.onclick = function() {
                return udx.notify(fn);
            };
            return o;
        }
        function timestamp(r) {
            var t = r.timestamp || r.received_at;
            return typeof t === 'number' ? new Date(t * 1000).toLocaleString() : t;
        }
        var columns = [
            [
                _('Number'),
                function(r) {
                    return r.peer || r.number;
                }
            ],
            [ _('Time'), timestamp ],
            [
                _('Message'),
                function(r) {
                    return r.text == null ? r.binary_hex || r.decode_error : r.text;
                }
            ],
            [
                _('Status'),
                function(r) {
                    return r.state || (r.read ? _('Read') : _('Unread'));
                }
            ]
        ];
        var inbox = udx.list(columns, function(r) {
            return r.id;
        }, true);
        var sent = udx.list(columns, function(r) {
            return r.id;
        }, true);
        var stored = udx.list(columns, function(r) {
            return r.index + ':' + r.pdu_sha256;
        }, true);
        function reload() {
            return Promise.all([ udx.getHistory(), udx.getSMS() ]).then(update);
        }
        function remove(list, folder, all) {
            var items = list.selected(all).map(function(r) {
                return folder ? {folder : folder, id : r.id}
                              : {index : r.index, pdu_sha256 : r.pdu_sha256};
            });
            if (!items.length)
                throw new Error(_('No messages selected'));
            function batch() {
                if (!items.length)
                    return reload();
                return udx.execute('sms_delete_batch', {items : items.splice(0, 50)})
                    .then(function(r) {
                        if (!r.complete) {
                            var e = new Error((r.items || [])
                                                  .filter(function(x) {
                                                      return !x.deleted;
                                                  })
                                                  .map(function(x) {
                                                      return x.error;
                                                  })
                                                  .join('; ') ||
                                              _('Deletion incomplete'));
                            throw e;
                        }
                        return batch();
                    });
            }
            return batch();
        }
        [[ 'inbox', inbox, 'inbox' ], [ 'sent', sent, 'sent' ], [ 'storage', stored, null ]]
            .forEach(function(spec) {
                embed(spec[0], '_' + spec[0], spec[1].node);
                action(spec[0], '_delete_' + spec[0], _('Delete selected'), function() {
                    return remove(spec[1], spec[2], false);
                });
                action(spec[0], '_clear_' + spec[0], _('Clear folder'), function() {
                    return remove(spec[1], spec[2], true);
                });
            });
        action('inbox', '_refresh', _('Read messages'), function() {
            return udx.execute('sms_refresh', {}).then(reload);
        });
        var number = s.taboption('compose', form.Value, 'number', _('Recipient number'));
        number.placeholder = '+8613800000000';
        var text = s.taboption('compose', form.TextValue, 'text', _('Message'));
        text.rows = 5;
        embed('compose', '_receipt', receipt);
        action('compose', '_send', _('Send'), function() {
            if (pending)
                throw new Error(
                    _('Check the previous send result before starting another message'));
            var args = {
                number : number.formvalue('main') || '',
                text : text.formvalue('main') || '',
                request_id : udx.randomId()
            };
            remember(args.request_id);
            receipt.textContent = _('Sending');
            return udx.execute('sms_send', args)
                .then(function(r) {
                    remember(null);
                    receipt.textContent = _('Submitted to modem');
                    return reload().then(function() {
                        return r;
                    });
                })
                .catch(function(e) {
                    receipt.textContent = e.message;
                    throw e;
                });
        });
        action('compose', '_check_send', _('Check send result'), function() {
            if (!pending) {
                receipt.textContent = _('No pending message');
                return {};
            }
            return udx.smsReceipt(pending).then(function(r) {
                if (!r.available)
                    throw new Error(r.error || _('Send result unavailable'));
                receipt.textContent =
                    r.state === 'complete' ? _('Submitted to modem') : r.error || r.state;
                if (r.state === 'complete' || r.state === 'failed')
                    remember(null);
                return r;
            });
        });
        action('compose', '_new_message', _('Start another message'), function() {
            remember(null);
            receipt.textContent = '';
            return {};
        });
        if (pending)
            receipt.textContent = _('A previous send result needs checking');
        var maxInbox = s.taboption('settings', form.Value, 'max_count', _('Inbox limit'));
        maxInbox.datatype = 'range(10,150)';
        var maxSent = s.taboption('settings', form.Value, 'max_sent_count', _('Sent limit'));
        maxSent.datatype = 'range(1,50)';
        var direct = s.taboption('settings', form.Flag, 'direct_receive', _('Direct reception'));
        action('settings', '_save_settings', _('Apply settings'), function() {
            return udx.execute('sms_settings', {
                max_count : Number(maxInbox.formvalue('main')),
                max_sent_count : Number(maxSent.formvalue('main')),
                direct_receive : direct.formvalue('main') === '1'
            });
        });
        function update(rows) {
            inbox.update(rows[0].inbox || []);
            sent.update(rows[0].sent || []);
            stored.update(rows[1].messages || []);
        }
        var error = udx.watch(function() {
            return Promise.all([ udx.getHistory(), udx.getSMS() ]);
        }, update, initial);
        return m.render().then(function(node) {
            return E('div', {}, [ node, error ]);
        });
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});
