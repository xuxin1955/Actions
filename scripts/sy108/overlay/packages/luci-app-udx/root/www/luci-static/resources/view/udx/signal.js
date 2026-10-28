'use strict';
'require view';
'require udx';

return view.extend({
    load : function() {
        return udx.getStatus();
    },
    render : function(initial) {
        var frame = E('iframe', {
            'src' : '/luci-static/udx/signal.html',
            'style' : 'width:100%;height:calc(100vh - 190px);min-height:560px;'
                    + 'border:0;border-radius:8px;background:#0b1220;display:block',
            'title' : _('UDX710 signal dashboard')
        });

        return E('div', { 'class' : 'cbi-section' }, [
            E('div', { 'class' : 'cbi-map-descr' }, [
                E('p', {}, _('实时蜂窝状态。数据直接来自设备 ubus，每 10 秒刷新一次。'))
            ]),
            frame
        ]);
    },
    handleSaveApply : null,
    handleSave : null,
    handleReset : null
});
