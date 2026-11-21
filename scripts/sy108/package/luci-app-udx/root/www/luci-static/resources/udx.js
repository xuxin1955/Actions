'use strict';
'require baseclass';
'require rpc';
'require poll';
'require ui';

var statusRPC = rpc.declare({object : 'udx', method : 'status', expect : {'' : {}}, reject : true});
var boardRPC =
    rpc.declare({object : 'system', method : 'board', expect : {'' : {}}, reject : true});
var systemRPC =
    rpc.declare({object : 'system', method : 'info', expect : {'' : {}}, reject : true});
function call(operation, args) {
    args = args || {};
    var fields = Object.keys(args);
    return rpc.declare({ object: 'udx.cellular', method: operation,
        params: fields, expect: { '': {} }, reject: true })
        .apply(null, fields.map(function(key) { return args[key]; }));
}
var cellStatusRPC = rpc.declare({object: 'udx.cellular', method: 'status', expect: {'': {}}, reject: true});
var historyRPC = rpc.declare({object: 'udx.cellular', method: 'sms_history_status', expect: {'': {}}, reject: true});
var smsRPC = rpc.declare({object: 'udx.cellular', method: 'sms_status', expect: {'': {}}, reject: true});
var apnRPC = rpc.declare({object: 'udx.cellular', method: 'apn_status', expect: {'': {}}, reject: true});
var wanRPC = rpc.declare({object: 'network.interface.wan', method: 'status', expect: {'': {}}, reject: true});
var wan6RPC = rpc.declare({object: 'network.interface.wan6', method: 'status', expect: {'': {}}, reject: true});
var usbRPC = rpc.declare({
    object : 'udx',
    method : 'usb',
    params : [ 'profile', 'acm' ],
    expect : {'' : {}},
    reject : true
});
var receiptRPC = rpc.declare({
    object : 'udx.cellular',
    method : 'sms_receipt',
    params : [ 'request_id' ],
    expect : {'' : {}},
    reject : true
});
var frequencyRPC =
    rpc.declare({object : 'udx', method : 'frequency_status', expect : {'' : {}}, reject : true});
var frequencyApplyRPC = rpc.declare({
    object : 'udx',
    method : 'frequency_apply',
    params : [ 'cpu_governor', 'cpu_min_khz', 'cpu_max_khz' ],
    expect : {'' : {}},
    reject : true
});
var queue = Promise.resolve();
function delay(ms) {
    return new Promise(function(resolve) {
        window.setTimeout(resolve, ms);
    });
}
function value(v, suffix) {
    if (v === null || v === undefined || v === '')
        return '—';
    if (typeof v === 'boolean')
        return v ? _('Yes') : _('No');
    if (Array.isArray(v))
        return v.map(function(x) {
                    return value(x);
                }).join(' / ') ||
               '—';
    return String(v) + (suffix || '');
}
function failure(row) {
    var e = new Error(row.error || _('Operation failed'));
    e.result = row.result;
    e.uncertain = row.state === 'uncertain' || row.uncertain;
    return e;
}
function result(row) {
    if (row.state !== 'complete')
        throw failure(row);
    return row.result || {};
}
function execute(operation, args) {
    function task() {
        var deadline = Date.now() + 360000;
        function submit() {
            return call(operation, args || {}).then(function(reply) {
                if (reply.busy && Date.now() < deadline)
                    return delay(600).then(submit);
                if (!reply.accepted)
                    throw failure(reply);
                if (reply.duplicate) {
                    if (reply.state === 'complete')
                        return result(reply);
                    throw failure(reply);
                }
                var ref = {
                    id : reply.operation_id,
                    boot_id : reply.boot_id,
                    broker_id : reply.broker_id
                };
                function wait() {
                    if (Date.now() >= deadline)
                        throw new Error(_('Operation timed out; it was not submitted again'));
                    return call('operation_status', ref).then(function(row) {
                        if (!row.accepted)
                            throw failure(row);
                        if (row.state === 'queued' || row.state === 'running')
                            return delay(500).then(wait);
                        return result(row);
                    });
                }
                return wait();
            });
        }
        return submit();
    }
    var pending = queue.then(task, task);
    queue = pending.catch(function() {});
    return pending;
}
function getStatus() {
    return Promise
        .all([ statusRPC(), L.resolveDefault(systemRPC(), {}), L.resolveDefault(boardRPC(), {}),
               L.resolveDefault(cellStatusRPC(), {available: false}), L.resolveDefault(wanRPC(), {}),
               L.resolveDefault(wan6RPC(), {}) ])
        .then(function(rows) {
            if (!rows[0].boot)
                throw failure(rows[0]);
            rows[0].memory = rows[1].memory || {};
            rows[0].swap = rows[1].swap || {};
            rows[0].system_info = rows[1];
            rows[0].board = rows[2];
            rows[0].cellular = rows[3];
            var addresses = [];
            [rows[4], rows[5]].forEach(function(network) {
                (network['ipv6-address'] || []).forEach(function(row) {
                    var ip = row.address || '';
                    if (ip && !/^fe[89ab]/i.test(ip) && ip !== '::' && ip !== '::1' && addresses.indexOf(ip) < 0)
                        addresses.push(ip);
                });
            });
            rows[0].cellular_kernel = {ipv6_global_addresses: addresses};
            rows[0].cellular_netifd = {wan: rows[4], wan6: rows[5]};
            return rows[0];
        });
}
function notify(action, message) {
    return Promise.resolve()
        .then(action)
        .then(function(r) {
            if (r && r.complete === false) {
                var errors = (r.items || [])
                                 .filter(function(x) {
                                     return !x.deleted;
                                 })
                                 .map(function(x) {
                                     return x.error;
                                 });
                throw new Error(errors.join('; ') || _('Some messages were not deleted'));
            }
            ui.addNotification(null,
                               E('p', {},
                                 r && r.waiting_for ? _('Waiting for') + ': ' + r.waiting_for
                                                    : message || _('Completed')),
                               'info');
            return r;
        })
        .catch(function(e) {
            ui.addNotification(null, E('p', {}, e.message), 'error');
            throw e;
        });
}
function button(label, action, style) {
    var b = E('button', {
        type : 'button',
        class : 'cbi-button ' + (style || 'cbi-button-action'),
        click : function() {
            if (b.disabled)
                return;
            b.disabled = true;
            b.classList.add('spinning');
            return notify(action).catch(function() {}).finally(function() {
                b.disabled = false;
                b.classList.remove('spinning');
            });
        }
    },
              label);
    return b;
}
function table(title, rows) {
    var cells = rows.map(function() {
        return E('td', {class : 'td'}, '—');
    });
    var node = E('section', {class : 'cbi-section'}, [
        E('h3', {}, title), E('table', {class : 'table'}, rows.map(function(r, i) {
            return E('tr', {class : 'tr'}, [ E('td', {class : 'td'}, r[0]), cells[i] ]);
        }))
    ]);
    return {
        node : node,
        update : function(data) {
            rows.forEach(function(r, i) {
                cells[i].textContent = value(r[1](data), r[2]);
            });
        }
    };
}
/* Only changed rows are inserted or removed. Input nodes in retained rows survive polling. */
function list(columns, key, selectable, action) {
    var entries = new Map(), rows = [], body = E('tbody'),
        header = E('tr', {class : 'tr table-titles'});
    if (selectable)
        header.appendChild(E('th', {class : 'th'}, _('Select')));
    columns.forEach(function(c) {
        header.appendChild(E('th', {class : 'th'}, c[0]));
    });
    if (action)
        header.appendChild(E('th', {class : 'th'}, _('Action')));
    var node = E('table', {class : 'table'}, [ E('thead', {}, header), body ]);
    return {
        node : node,
        update : function(next) {
            rows = next || [];
            var seen = new Set();
            rows.forEach(function(row, index) {
                var id = String(key(row)), entry = entries.get(id);
                seen.add(id);
                if (!entry) {
                    entry = {row : row, node : E('tr', {class : 'tr'}), cells : []};
                    if (selectable) {
                        entry.check =
                            E('input', {type : 'checkbox', 'aria-label' : _('Select message')});
                        entry.node.appendChild(E('td', {class : 'td'}, entry.check));
                    }
                    columns.forEach(function() {
                        var cell = E('td', {class : 'td'});
                        entry.cells.push(cell);
                        entry.node.appendChild(cell);
                    });
                    if (action)
                        entry.node.appendChild(E('td', {class : 'td'}, action(function() {
                                                     return entry.row;
                                                 })));
                    entries.set(id, entry);
                }
                entry.row = row;
                columns.forEach(function(c, i) {
                    entry.cells[i].textContent = value(c[1](row));
                });
                if (body.children[index] !== entry.node)
                    body.insertBefore(entry.node, body.children[index] || null);
            });
            entries.forEach(function(entry, id) {
                if (!seen.has(id)) {
                    entry.node.remove();
                    entries.delete(id);
                }
            });
        },
        selected : function(all) {
            return rows.filter(function(row) {
                var entry = entries.get(String(key(row)));
                return all || (entry.check && entry.check.checked);
            });
        }
    };
}
function watch(load, update, initial) {
    var error = E('p', {class : 'alert-message warning', hidden : true});
    if (initial)
        update(initial);
    poll.add(function() {
        return load()
            .then(function(data) {
                update(data);
                error.hidden = true;
            })
            .catch(function(e) {
                error.textContent = e.message;
                error.hidden = false;
            });
    }, 5);
    return error;
}
function usb(profile, acm) {
    return usbRPC(profile, acm).then(function(r) {
        if (!r.accepted)
            throw failure(r);
        var end = Date.now() + 300000;
        function wait() {
            return getStatus().then(
                function(d) {
                    var u = d.usb || {};
                    if (!u.pending && u.profile === profile && u.acm === (acm ? '1' : '0') &&
                        (u.result === '0' || u.result === 0))
                        return u;
                    if (!u.pending && u.result != null && String(u.result) !== '0')
                        throw new Error(_('USB configuration failed') + ': ' + u.result);
                    if (Date.now() >= end)
                        throw new Error(_('USB reconnection timed out'));
                    return delay(1500).then(wait);
                },
                function(e) {
                    if (Date.now() >= end)
                        throw e;
                    return delay(1500).then(wait);
                });
        }
        return delay(2000).then(wait);
    });
}
/* Refresh only this select after an explicit action; retain all other inputs. */
function setChoices(option, rows, selected) {
    var widget = option.getUIElement('main');
    option.keylist = [];
    option.vallist = [];
    rows.forEach(function(row) {
        option.value(String(row[0]), row[1]);
    });
    return Promise.resolve(option.renderWidget('main', 0, String(selected))).then(function(node) {
        widget.node.replaceWith(node);
    });
}
return baseclass.extend({
    setChoices : setChoices,
    frequencyStatus : frequencyRPC,
    frequencyApply : function(cpu, minimum, maximum) {
        return frequencyApplyRPC(cpu, +minimum, +maximum).then(function(r) {
            if (!r.accepted)
                throw failure(r);
            return r;
        });
    },
    value : value,
    execute : execute,
    notify : notify,
    button : button,
    table : table,
    list : list,
    watch : watch,
    getStatus : getStatus,
    getHistory : historyRPC,
    getSMS : smsRPC,
    getAPN : apnRPC,
    smsReceipt : receiptRPC,
    usb : usb,
    delay : delay,
    randomId : function() {
        var a = new Uint8Array(16);
        crypto.getRandomValues(a);
        return Array
            .from(a,
                  function(x) {
                      return x.toString(16).padStart(2, '0');
                  })
            .join('');
    }
});
