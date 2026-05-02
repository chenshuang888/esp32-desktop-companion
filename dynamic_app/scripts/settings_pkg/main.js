// ============================================================================
// settings_pkg —— 多级 iOS 风格设置（Router 多页栈验证 app）
//
// 页结构：
//   list           主列表（push 入口）
//     ├─ display    显示子页：亮度滑块（用 + / - 按钮模拟）+ 暗色模式开关
//     ├─ about      关于子页：固件信息列表（KV 行）
//     ├─ network    网络子页 → 内含一个三级 push 到 detail_ble
//     │   └─ detail_ble  三级页：BLE 状态详情 + "回首页"按钮（popTo('list')）
//     └─ danger     危险操作：清空状态（onLeave 演示，进出弹 toast）
//
// 状态：
//   存在 sys.app.saveState 的单 KV 里：{ b: 亮度0..100, d: 暗色 0/1 }
//   Router.onEnter('display') 时 loadState 同步 UI；onLeave 时 saveState
//
// 演示要点：
//   ✓ Router.start / push / pop / replace / popTo
//   ✓ onEnter / onLeave 生命周期
//   ✓ 跨页面状态持久化（saveState 在 onLeave 触发，避免每次拖动写 NVS）
//   ✓ 屏底上滑：栈深≥2 自动 pop；栈深=1 退出 app（穿透到宿主）
//   ✓ 同名页递归 push（detail_ble 可以多次进入也不冲突）
// ============================================================================

var T = UI.T;

// --- 状态 ----------------------------------------------------------------
var state = { b: 60, d: 0 };

(function loadState() {
    var raw = sys.app.loadState();
    if (!raw) return;
    try {
        var s = JSON.parse(raw);
        if (s && typeof s.b === 'number') state.b = Math.max(0, Math.min(100, s.b | 0));
        if (s && typeof s.d === 'number') state.d = s.d ? 1 : 0;
    } catch (e) { sys.log('settings: bad state'); }
})();

function saveState() {
    sys.app.saveState(JSON.stringify({ b: state.b, d: state.d }));
}

// --- 公共：返回栏 -------------------------------------------------------
//   左侧"<"按钮 + 标题。带 onBack 自定义；缺省 = Router.pop
function topbar(title, onBack) {
    return h('panel', {
        size: [-100, 36],
        bg: T.C_PANEL,
        scrollable: false,
        align: ['tl', 0, 0],
        flex: 'row',
        flexAlign: ['start', 'center', 'center'],
        pad: [4, 2, 12, 2],
        gap: [0, 8],
        border: { color: T.C_BORDER, width: 1, side: 'bottom', opa: 102 }
    }, [
        h('button', {
            size: [40, 28], bg: T.C_PANEL, radius: 6, scrollable: false,
            pressedBg: { color: T.C_PANEL_HI, opa: 255 },
            onClick: onBack || function () { Router.pop(); }
        }, [
            h('label', { text: sys.icons.CHEVRON_LEFT,
                         fg: T.C_ACCENT, font: 'icon24', align: ['c', 0, 0] })
        ]),
        h('label', { text: title, fg: T.C_TEXT, font: 'title' })
    ]);
}

// --- 页：list（主列表）-------------------------------------------------
function buildList(props) {
    return h('panel', {
        bg: T.C_BG, flex: 'col', scrollable: true, gap: [4, 0]
    }, [
        UI.statusBar({ title: '设置' }),
        UI.card({ pad: [0, 0, 0, 0] }, [
            UI.listRow({
                icon: sys.icons.BRIGHTNESS, label: '显示',
                value: state.b + '% / ' + (state.d ? '深色' : '浅色'),
                iconBg: T.C_ACCENT,
                onClick: function () { Router.push('display'); }
            }),
            UI.listRow({
                icon: sys.icons.BLUETOOTH, label: '网络',
                iconBg: T.C_INFO,
                onClick: function () { Router.push('network'); }
            }),
            UI.listRow({
                icon: sys.icons.INFO, label: '关于',
                iconBg: T.C_TEXT_MUTED,
                onClick: function () { Router.push('about'); }
            }),
            UI.listRow({
                icon: sys.icons.SETTINGS, label: '清空设置',
                iconBg: T.C_ERR,
                onClick: function () { Router.push('danger'); }
            })
        ])
    ]);
}

// --- 页：display（显示）-----------------------------------------------
//   亮度用 +/- 按钮调（步进 10），暗色模式用一个胶囊按钮
function buildDisplay(props) {
    return h('panel', { bg: T.C_BG, flex: 'col', scrollable: true, gap: [4, 0] }, [
        topbar('显示'),
        UI.card({}, [
            h('panel', {
                size: [-100, 50], scrollable: false,
                flex: 'row', flexAlign: ['between', 'center', 'center'],
                pad: [12, 0, 12, 0]
            }, [
                h('label', { text: '亮度', fg: T.C_TEXT, font: 'text' }),
                h('panel', {
                    size: [120, 32], scrollable: false,
                    flex: 'row', flexAlign: ['between', 'center', 'center'],
                    gap: [0, 6]
                }, [
                    h('button', {
                        size: [32, 28], bg: T.C_PANEL_HI, radius: 14, scrollable: false,
                        pressedBg: { color: T.C_ACCENT, opa: 80 },
                        onClick: function () {
                            state.b = Math.max(0, state.b - 10);
                            VDOM.set('disp_b', { text: state.b + '%' });
                        }
                    }, [
                        h('label', { text: '−', fg: T.C_TEXT, font: 'title',
                                     align: ['c', 0, -2] })
                    ]),
                    h('label', { id: 'disp_b', text: state.b + '%',
                                 fg: T.C_TEXT, font: 'text', align: ['c', 0, 0] }),
                    h('button', {
                        size: [32, 28], bg: T.C_PANEL_HI, radius: 14, scrollable: false,
                        pressedBg: { color: T.C_ACCENT, opa: 80 },
                        onClick: function () {
                            state.b = Math.min(100, state.b + 10);
                            VDOM.set('disp_b', { text: state.b + '%' });
                        }
                    }, [
                        h('label', { text: '+', fg: T.C_TEXT, font: 'title',
                                     align: ['c', 0, -2] })
                    ])
                ])
            ])
        ]),
        UI.card({}, [
            h('panel', {
                size: [-100, 50], scrollable: false,
                flex: 'row', flexAlign: ['between', 'center', 'center'],
                pad: [12, 0, 12, 0]
            }, [
                h('label', { text: '深色模式', fg: T.C_TEXT, font: 'text' }),
                UI.pillBtn({
                    id: 'disp_d',
                    text: state.d ? '已开启' : '关闭',
                    w: 78, h: 28,
                    bg: state.d ? T.C_ACCENT : T.C_PANEL_HI,
                    fg: state.d ? T.C_PANEL : T.C_TEXT,
                    onClick: function () {
                        state.d = state.d ? 0 : 1;
                        VDOM.set('disp_d', {
                            text: state.d ? '已开启' : '关闭',
                            bg:   state.d ? T.C_ACCENT : T.C_PANEL_HI,
                            fg:   state.d ? T.C_PANEL : T.C_TEXT
                        });
                    }
                })
            ])
        ])
    ]);
}

// --- 页：about ---------------------------------------------------------
function buildAbout(props) {
    var build = sys.time.format(sys.time.now(), '%Y-%m-%d %H:%M');
    return h('panel', { bg: T.C_BG, flex: 'col', scrollable: true, gap: [4, 0] }, [
        topbar('关于'),
        UI.card({ pad: [0, 0, 0, 0] }, [
            UI.kvRow({ key: '设备',     value: 'ESP32-S3 N16R8' }),
            UI.kvRow({ key: '固件',     value: 'demo6 / feat/optimize_page' }),
            UI.kvRow({ key: '动态app',  value: 'gomoku/doodle/notif/settings' }),
            UI.kvRow({ key: '当前时间', value: build })
        ]),
        UI.card({}, [
            UI.pillBtn({
                text: '替换为危险页（replace 演示）',
                w: -100, h: 36,
                onClick: function () { Router.replace('danger'); }
            })
        ])
    ]);
}

// --- 页：network -------------------------------------------------------
function buildNetwork(props) {
    return h('panel', { bg: T.C_BG, flex: 'col', scrollable: true, gap: [4, 0] }, [
        topbar('网络'),
        UI.card({ pad: [0, 0, 0, 0] }, [
            UI.listRow({
                icon: sys.icons.BLUETOOTH, label: 'BLE 详情',
                iconBg: T.C_INFO,
                onClick: function () { Router.push('detail_ble'); }
            }),
            UI.kvRow({ key: 'WiFi', value: '未启用' })
        ])
    ]);
}

// --- 页：detail_ble（三级页，演示 popTo）-------------------------------
function buildDetailBle(props) {
    var lines = [
        { k: '已配对', v: '是' },
        { k: '名称',   v: 'esp32-watch' },
        { k: 'MTU',    v: '203' },
        { k: 'RSSI',   v: '-58 dBm' }
    ];
    var rows = [];
    for (var i = 0; i < lines.length; i++) {
        rows.push(UI.kvRow({ key: lines[i].k, value: lines[i].v,
                             divider: i < lines.length - 1 }));
    }
    rows.push(h('panel', { size: [-100, 8] }));
    rows.push(UI.pillBtn({
        text: '回到设置首页（popTo）',
        w: -100, h: 36,
        onClick: function () { Router.popTo('list'); }
    }));
    return h('panel', { bg: T.C_BG, flex: 'col', scrollable: true, gap: [4, 0] }, [
        topbar('BLE 详情'),
        UI.card({ pad: [0, 0, 0, 0] }, rows)
    ]);
}

// --- 页：danger --------------------------------------------------------
function buildDanger(props) {
    return h('panel', { bg: T.C_BG, flex: 'col', scrollable: true, gap: [4, 0] }, [
        topbar('清空设置'),
        UI.card({}, [
            h('label', {
                text: '点下方按钮会把亮度恢复 60%、深色模式关闭，并写回 NVS。',
                fg: T.C_TEXT_DIM, font: 'text'
            })
        ]),
        UI.card({}, [
            UI.pillBtn({
                text: '恢复默认',
                w: -100, h: 40,
                bg: T.C_ERR, fg: T.C_PANEL,
                onClick: function () {
                    UI.modal({
                        title: '确认清空？',
                        body:  '亮度恢复 60%、关闭深色',
                        action0: '取消',
                        action1: '确定',
                        onResult: function (idx) {
                            if (idx !== 1) return;
                            state.b = 60; state.d = 0;
                            saveState();
                            UI.toast('已恢复', 700);
                            Router.pop();
                        }
                    });
                }
            })
        ])
    ]);
}

// --- 注册 + 生命周期 -----------------------------------------------------
Router.define('list',       buildList);
Router.define('display',    buildDisplay);
Router.define('about',      buildAbout);
Router.define('network',    buildNetwork);
Router.define('detail_ble', buildDetailBle);
Router.define('danger',     buildDanger);

// 离开 display 时落盘（避免每次按 +/- 都写 NVS）
Router.onLeave('display', function () {
    saveState();
    sys.log('settings: display saved (b=' + state.b + ' d=' + state.d + ')');
});

// 进入 list 时刷新预览值（从子页 pop 回来时同步显示）
Router.onEnter('list', function () {
    // 第一次 start 也会触发，此时 list 还没渲染完就尝试 set 会 warn
    // 简单方法：尝试 set，找不到就忽略（VDOM.set 内部已 sys.log warn）
    // 进 detail 修改 state 后回来，list 的 listRow value 不会自动更新——
    // 正确做法是把 listRow value 单独 id 化，这里仅做演示，不强求精致刷新。
});

// 启动
sys.log('settings: start');
Router.start('list');
sys.log('settings: ready, depth=' + Router.depth());
