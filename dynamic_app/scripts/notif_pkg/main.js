// ============================================================================
// notif —— 动态 app 复刻原生"通知"app
//
// 验证目标：
//   用 prelude.js 提供的 UI 库 + sys.time + sys.ui.modal/toast/fadeIn 这套
//   动态 app 能力，**不依赖 notify_manager**，独立做出与原生通知 app 几乎
//   一致的视觉与交互。
//
// 协议（自定义，与 notify_manager 无关）：
//   ESP → PC   {to:"notif", type:"req"}                请求历史（可选）
//   ESP ← PC   {to:"notif", type:"add",
//                body:{title, body, ts, cat}}          PC 推一条新通知
//   ESP ← PC   {to:"notif", type:"clear"}              PC 要求清空
//
//   cat 取值：'msg' | 'mail' | 'call' | 'cal' | 'social' | 'news' | 'alert'
//
// 数据：
//   首次启动注入 5 条假数据（演示）；之后 BLE 进来的 add 追加到顶部，最多 30 条
//   退出时通过 sys.app.saveState 保存（JSON），重进恢复
//
// UI 布局：
//   [统一标题栏] "通知" + 右上圆角徽章（长按弹"清空所有"）
//   [列表] 220×218 可滚动；每项 64px = icon 36 + 标题(LONG_DOT) + 时间 + 单行 body
//   [屏底 30px hitZone] 上滑退出
//
// 详情：点列表项 → UI.modal({ title, body, action0:'删除', action1:'关闭' })
// ============================================================================

var APP_NAME = "notif_pkg";
var ble = makeBle(APP_NAME);

var T = UI.T;
var I = UI.I;

// --- 状态 ----------------------------------------------------------------
var state = {
    items: []   // 每项：{title, body, ts, cat}
};

var MAX_ITEMS = 30;

// 类别 → { icon, color }（仿原生 cat_style_for）
function catStyle(cat) {
    if (cat === 'msg')    return { icon: I.NOTIFICATIONS, color: T.C_ACCENT };       // 蓝
    if (cat === 'mail')   return { icon: I.NOTIFICATIONS, color: T.C_WARN };         // 橙
    if (cat === 'call')   return { icon: I.NOTIFICATIONS, color: T.C_OK };           // 绿
    if (cat === 'cal')    return { icon: I.SCHEDULE,      color: T.C_ACCENT_2 };     // 紫
    if (cat === 'social') return { icon: I.NOTIFICATIONS, color: T.C_INFO };         // 浅蓝
    if (cat === 'news')   return { icon: I.NOTIFICATIONS, color: T.C_TEXT_MUTED };   // 灰
    if (cat === 'alert')  return { icon: I.NOTIFICATIONS, color: T.C_ERR };          // 红
    return { icon: I.NOTIFICATIONS, color: T.C_TEXT_MUTED };
}

// --- 持久化 --------------------------------------------------------------
function loadState() {
    try {
        var s = sys.app.loadState();
        if (s) {
            var d = JSON.parse(s);
            if (d && d.items) state.items = d.items;
        }
    } catch (e) { sys.log("loadState err: " + e); }
}

function saveState() {
    try { sys.app.saveState(JSON.stringify(state)); }
    catch (e) { sys.log("saveState err: " + e); }
}

// --- 假数据（首启时） ----------------------------------------------------
function ensureSeedData() {
    if (state.items.length > 0) return;
    var now = sys.time.now();
    state.items = [
        { title: "微信", body: "妈妈：晚上记得打电话回家",
          ts: now - 60,        cat: 'msg' },
        { title: "公司日历", body: "10:30 周会 · 视频会议链接已发送",
          ts: now - 60 * 30,   cat: 'cal' },
        { title: "邮件", body: "采购单已审批通过",
          ts: now - 60 * 90,   cat: 'mail' },
        { title: "未接来电", body: "+86 138-XXXX-1234 · 2 次",
          ts: now - 60 * 180,  cat: 'call' },
        { title: "系统提醒", body: "电池电量低于 20%，请尽快充电",
          ts: now - 3600 * 5,  cat: 'alert' }
    ];
}

// --- 时间格式化 -----------------------------------------------------------
function formatShort(ts) {
    return sys.time.format(ts, "%H:%M");
}
function formatFull(ts) {
    return sys.time.format(ts, "%Y-%m-%d %H:%M");
}

// --- BLE 接收 -------------------------------------------------------------
ble.on("add", function (msg) {
    var b = msg.body || {};
    sys.log("notif: ble.add fired title=" + (b.title || '') + " items=" + state.items.length);
    if (!b.title && !b.body) return;
    var item = {
        title: ('' + (b.title || '')).slice(0, 31),
        body:  ('' + (b.body  || '')).slice(0, 95),
        ts:    (b.ts | 0) || sys.time.now(),
        cat:   b.cat || 'msg'
    };
    state.items.unshift(item);
    if (state.items.length > MAX_ITEMS) state.items.length = MAX_ITEMS;
    saveState();
    rebuildList();
    UI.toast("新通知", 1000);
});

ble.on("clear", function () {
    state.items = [];
    saveState();
    rebuildList();
});

// --- UI ------------------------------------------------------------------

// 单项卡片（id 用 'it_' + index，rebuild 时整体重建）
function renderItem(idx, n) {
    var cs = catStyle(n.cat);
    return h('button', {
        id: 'it_' + idx,
        size: [-100, 64],
        bg: T.C_PANEL,
        radius: T.R_LG,
        border: { color: T.C_BORDER, width: 1, side: 'full', opa: 128 },
        pad: [10, 10, 10, 10],
        scrollable: false,
        pressedBg: { color: T.C_PANEL_HI, opa: 255 },
        onClick: function () { showDetail(idx); }
    }, [
        // icon 块（36×36 圆角彩底）
        h('panel', {
            id: 'ic_' + idx,
            size: [36, 36], bg: cs.color, radius: T.R_MD,
            align: ['lm', 0, 0],
            scrollable: false
        }, [
            h('label', { id: 'ic_lb_' + idx, text: cs.icon,
                          fg: T.C_PANEL, font: 'icon24',
                          align: ['c', 0, 0] })
        ]),

        // 标题（单行 LONG_DOT）
        h('label', {
            id: 'ti_' + idx,
            text: n.title || '(无标题)',
            fg: T.C_TEXT, font: 'title',
            longMode: 'dot',
            size: [116, 20],
            align: ['tl', 44, 2]
        }),

        // 时间（右上 40×20）
        h('label', {
            id: 'tm_' + idx,
            text: formatShort(n.ts),
            fg: T.C_TEXT_MUTED, font: 'text',
            textAlign: 'right',
            size: [40, 20],
            align: ['tr', 0, 2]
        }),

        // 正文（单行 LONG_DOT，下方占满）
        h('label', {
            id: 'bd_' + idx,
            text: n.body || '',
            fg: T.C_TEXT_DIM, font: 'text',
            longMode: 'dot',
            size: [160, 18],
            align: ['tl', 44, 28]
        })
    ]);
}

function renderEmpty() {
    return h('panel', {
        id: 'empty', size: [-100, 120],
        align: ['c', 0, 0],
        scrollable: false,
        flex: 'column',
        flexAlign: ['center', 'center', 'center'],
        gap: [10, 0]
    }, [
        h('label', { id: 'em_ic', text: I.NOTIFICATIONS,
                     fg: T.C_TEXT_MUTED, font: 'icon36' }),
        h('label', { id: 'em_lb', text: '暂无通知',
                     fg: T.C_TEXT_MUTED, font: 'title' })
    ]);
}

function rebuildList() {
    // 销毁旧的列表内容
    var listNode = VDOM.find('list');
    if (listNode) {
        var kids = listNode.children.slice();
        for (var i = 0; i < kids.length; i++) {
            VDOM.destroy(kids[i].props.id);
        }
    }

    // 计数徽章
    var n = state.items.length;
    var badgeText = n > 99 ? '99+' : ('' + n);
    VDOM.set('badge_lb', { text: badgeText });
    VDOM.set('badge', { bg: n === 0 ? T.C_BORDER : T.C_ACCENT });

    // 列表内容
    if (n === 0) {
        var em = renderEmpty();
        em._parent = listNode;
        VDOM.mount(em, 'list');
        listNode.children.push(em);
    } else {
        for (var i2 = 0; i2 < n; i2++) {
            var it = renderItem(i2, state.items[i2]);
            it._parent = listNode;
            VDOM.mount(it, 'list');
            listNode.children.push(it);
        }
    }
}

// 详情模态
function showDetail(idx) {
    var n = state.items[idx];
    if (!n) return;
    UI.modal({
        title: n.title || '(无标题)',
        body:  formatFull(n.ts) + "\n\n" + (n.body || ''),
        action0: '删除',
        action1: '关闭',
        onResult: function (r) {
            if (r === 0) {
                // 删除
                state.items.splice(idx, 1);
                saveState();
                rebuildList();
                UI.toast('已删除', 800);
            }
            // r=1 关闭 / r=-1 取消：什么都不做
        }
    });
}

// 长按徽章 → 清空所有
function showClearAll() {
    if (state.items.length === 0) return;
    UI.modal({
        title: '清空所有通知？',
        body:  '此操作将删除全部已收到的通知，且无法恢复。',
        action0: '取消',
        action1: '清空',
        onResult: function (r) {
            if (r === 1) {
                state.items = [];
                saveState();
                rebuildList();
                UI.toast('已清空', 800);
            }
        }
    });
}

// --- 主页面构建 ---------------------------------------------------------
// 注意：宿主层（page_dynapp_host）已经挂了顶部 24px 状态栏 + 屏底 28px 上滑
// 退出区。动态 app 的可用区是 [y=0, h=268]（root 容器已对齐到状态栏下）。
// 所以这里 root size 用 -100/-100（撑满 list_root），不需要再处理 statusbar
// 或自己的 hitZone。
function buildUI() {
    // 顶栏（标题 + 徽章）—— 直接顶到 list_root 顶部 0
    var header = h('panel', {
        id: 'hdr',
        size: [-100, 36],
        align: ['tl', 0, 0],
        flex: 'row',
        flexAlign: ['between', 'center', 'center'],
        pad: [T.SP_MD, 6, T.SP_MD, 6],
        scrollable: false
    }, [
        h('label', { id: 'title', text: '通知',
                     fg: T.C_TEXT, font: 'title' }),
        // 徽章（长按清空）
        h('panel', {
            id: 'badge',
            size: [36, 20],
            bg: T.C_ACCENT, radius: T.R_PILL,
            scrollable: false,
            onLongPress: showClearAll
        }, [
            h('label', { id: 'badge_lb', text: '0',
                         fg: T.C_PANEL, font: 'text', align: ['c', 0, 0] })
        ])
    ]);

    // 列表容器：list_root 高 268，header 占 36，留 232 给列表
    var list = h('panel', {
        id: 'list',
        size: [-100, 232],
        align: ['tl', 0, 36],
        pad: [T.SP_MD, 0, T.SP_MD, 0],
        flex: 'column',
        flexAlign: ['start', 'center', 'start'],
        gap: [8, 0],
        scrollable: true
    }, []);

    var page = h('panel', {
        id: 'root',
        size: [-100, -100],
        bg: T.C_BG,
        scrollable: false,
        pad: [0, 0, 0, 0]
    }, [header, list]);

    VDOM.mount(page, null);
}

// --- 入口 ----------------------------------------------------------------
sys.log('notif: build start');

loadState();
ensureSeedData();
buildUI();
sys.ui.attachRootListener('root');
rebuildList();

// 入场动画
UI.fadeIn('hdr',   0);
UI.fadeIn('list',  100);

// 1Hz 刷新时间显示（只更新可见项的 tm_x label）
setInterval(function () {
    for (var i = 0; i < state.items.length; i++) {
        var nd = VDOM.find('tm_' + i);
        if (nd) VDOM.set('tm_' + i, { text: formatShort(state.items[i].ts) });
    }
}, 30000);   // 30s 刷一次足够（HH:MM 不会每秒变）

sys.log('notif: build done, items=' + state.items.length);
