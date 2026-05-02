// Dynamic App —— 闹钟页面（华为风格复刻，240×320 适配）
// 框架：VDOM / h / makeBle 由 prelude.js 提供。约束 ES5。

// ============================================================================
// §2. 闹钟业务（声明式 view + rerender）
// ============================================================================
//
// 数据流：state -> view(state) -> VDOM.render(树) -> diff -> 调原语
//
// 业务只写：
//   1) state（普通对象）
//   2) view(state)（纯函数，描述当前状态对应的 UI 长啥样）
//   3) 事件回调里改 state，最后调 rerender()
//
// 例外：手势拖动 (onCardDrag) 因为每帧都触发，保留命令式 VDOM.set 旁路，
//      避免每帧重 render 整棵树。释放/点击时再走 rerender 收尾。
//
// 屏幕 240×320。布局（自上而下）：
//   - header     36   "闹钟" + ☰
//   - clockArea  80   时段 + HH:MM:SS（huge）
//   - statusBar  20   "已开启 N 个闹钟" / "所有闹钟已关闭"
//   - list       140  闹钟卡片列表
//   - fab        44   底部居中圆形 +
//
// 卡片三层（swipe-to-reveal）：
//   row_<seq>   轨道
//     ├─ del_<seq>    底层红色删除按钮
//     └─ alarm_<seq>  上层卡片本体，align lm x 0；x 由 cardSwipe[seq] 决定

var COLOR_BG          = 0x1A1530;
var COLOR_HEADER      = 0x2D2640;
var COLOR_CARD        = 0x2D2640;
var COLOR_TEXT        = 0xF1ECFF;
var COLOR_TEXT_DIM    = 0x9088A8;
var COLOR_TEXT_OFF    = 0x6B6480;
var COLOR_ACCENT      = 0x007DFF;
var COLOR_SWITCH_OFF  = 0x4A4360;
var COLOR_KNOB        = 0xFFFFFF;
var COLOR_DANGER      = 0xEF4444;

var SWIPE_REVEAL_PX   = 80;    // 露出删除按钮宽度
var SWIPE_THRESHOLD   = 30;    // 松手时位移超过这个就吸附到 open

// ---- 全局 state ----
//
// 持久化策略：
//   - 启动时 sys.app.loadState() 拿到 JSON 字符串，没数据返回 null
//   - 只持久化"业务状态"：alarms / nextSeq
//   - UI 临时态（editSeq / clockPeriod / clockMain）每次启动都重置成默认
//   - 改 state 的事件回调末尾调一次 persist()，立即落盘

var DEFAULT_ALARMS = [
    { seq: 0, tag: "清晨", time: "6:55", sub: "闹钟，每天",   on: false },
    { seq: 1, tag: "早上", time: "8:20", sub: "早上好，每天", on: true  },
    { seq: 2, tag: "早上", time: "8:30", sub: "早上好，每天", on: false }
];

function loadState() {
    var raw = sys.app.loadState();
    if (!raw) return null;
    try { return JSON.parse(raw); }
    catch (e) { sys.log("loadState: bad JSON, ignored"); return null; }
}

var saved = loadState();
var state = {
    alarms:      saved && saved.alarms  ? saved.alarms  : DEFAULT_ALARMS,
    nextSeq:     saved && saved.nextSeq ? saved.nextSeq : 100,
    editSeq:     -1,
    clockPeriod: "上午",
    clockMain:   "00:00:00"
};

function persist() {
    sys.app.saveState(JSON.stringify({
        alarms:  state.alarms,
        nextSeq: state.nextSeq
    }));
}

// 卡片手势状态：seq -> { phase: 'rest'|'dragging'|'open', x: 当前位移 }
// 不放进 state —— 这是临时 UI 态，不该触发 rerender
var cardSwipe = {};

function alarmRowId(seq)     { return "row_"   + seq; }
function alarmCardId(seq)    { return "alarm_" + seq; }
function alarmDelId(seq)     { return "del_"   + seq; }
function alarmSwitchId(seq)  { return "sw_"    + seq; }
function alarmKnobId(seq)    { return "knob_"  + seq; }
function alarmTimeId(seq)    { return "tm_"    + seq; }

function findIndexBySeq(seq) {
    var i;
    for (i = 0; i < state.alarms.length; i++) {
        if (state.alarms[i].seq === seq) return i;
    }
    return -1;
}

function statusMsg() {
    var n = 0, i;
    for (i = 0; i < state.alarms.length; i++) if (state.alarms[i].on) n++;
    return (n === 0) ? "所有闹钟已关闭" : ("已开启 " + n + " 个闹钟");
}

// ---- 关闭其他露出卡片（命令式旁路：临时 UI 态收敛，不重 render）----
function closeAllExcept(exceptSeq) {
    var k;
    for (k in cardSwipe) {
        if (!cardSwipe.hasOwnProperty(k)) continue;
        if (k === String(exceptSeq)) continue;
        if (cardSwipe[k].phase === 'open') {
            cardSwipe[k].phase = 'rest';
            cardSwipe[k].x = 0;
            VDOM.set(alarmCardId(parseInt(k, 10)), { align: ['lm', 0, 0] });
        }
    }
}

// ---- 手势 hooks ----
function onCardPress(e) {
    var seq = parseInt(e.currentTarget.substring(6), 10);
    if (!cardSwipe[seq]) cardSwipe[seq] = { phase: 'rest', x: 0 };
    cardSwipe[seq].phase = 'dragging';
}

function onCardDrag(e) {
    // 每帧触发：走命令式旁路，不重 render
    var seq = parseInt(e.currentTarget.substring(6), 10);
    var s = cardSwipe[seq];
    if (!s || s.phase !== 'dragging') return;
    var nx = s.x + e.dx;
    if (nx > 0)               nx = 0;
    if (nx < -SWIPE_REVEAL_PX) nx = -SWIPE_REVEAL_PX;
    s.x = nx;
    VDOM.set(alarmCardId(seq), { align: ['lm', nx, 0] });
}

function onCardRelease(e) {
    var seq = parseInt(e.currentTarget.substring(6), 10);
    var s = cardSwipe[seq];
    if (!s || s.phase !== 'dragging') return;
    var dragged = (s.x !== 0);
    if (s.x <= -SWIPE_THRESHOLD) {
        s.phase = 'open';
        s.x = -SWIPE_REVEAL_PX;
        VDOM.set(alarmCardId(seq), { align: ['lm', -SWIPE_REVEAL_PX, 0] });
        closeAllExcept(seq);
    } else {
        s.phase = 'rest';
        s.x = 0;
        VDOM.set(alarmCardId(seq), { align: ['lm', 0, 0] });
    }
    // LVGL 在小于 gesture 阈值时 release 后还会发 CLICKED；
    // 我们这边业务已知"刚才拖过"，标记一下，紧跟着的 click 直接吞掉。
    if (dragged) s.suppressClick = true;
}

function onCardClick(e) {
    var seq = parseInt(e.currentTarget.substring(6), 10);
    var s = cardSwipe[seq];
    if (s && s.suppressClick) {
        s.suppressClick = false;
        return;
    }
    if (s && s.phase === 'open') {
        s.phase = 'rest';
        s.x = 0;
        VDOM.set(alarmCardId(seq), { align: ['lm', 0, 0] });
        return;
    }
    closeAllExcept(seq);
    state.editSeq = seq;
    rerender();
}

function onCardLongPress(e) {
    // 长按直接吸附到 open，露出删除按钮（提供滑动之外的另一条路径）
    var seq = parseInt(e.currentTarget.substring(6), 10);
    if (!cardSwipe[seq]) cardSwipe[seq] = { phase: 'rest', x: 0 };
    cardSwipe[seq].phase = 'open';
    cardSwipe[seq].x = -SWIPE_REVEAL_PX;
    VDOM.set(alarmCardId(seq), { align: ['lm', -SWIPE_REVEAL_PX, 0] });
    closeAllExcept(seq);
}

function onSwitchClick(e) {
    var seq = parseInt(e.currentTarget.substring(3), 10);
    var idx = findIndexBySeq(seq);
    if (idx < 0) return false;
    state.alarms[idx].on = !state.alarms[idx].on;
    persist();
    rerender();
    return false;
}

function onDelClick(e) {
    var seq = parseInt(e.currentTarget.substring(4), 10);
    var idx = findIndexBySeq(seq);
    if (idx < 0) return false;
    state.alarms.splice(idx, 1);
    delete cardSwipe[seq];
    persist();
    rerender();
    sys.log("alarm removed: seq=" + seq);
    return false;
}

function onAddClick() {
    var seq = state.nextSeq++;
    state.alarms.push({
        seq: seq, tag: "上午", time: "9:00",
        sub: "闹钟，仅一次", on: true
    });
    persist();
    rerender();
    sys.log("alarm added: seq=" + seq);
}

function closeEditView() {
    state.editSeq = -1;
    rerender();
}

// ============================================================================
// §3. view —— 纯函数，描述 state 对应的 UI 长什么样
// ============================================================================

function viewAlarmRow(a) {
    var seq = a.seq;
    return h('panel', {
        id: alarmRowId(seq),
        size: [-100, 56],
        bg: COLOR_BG
    }, [
        h('button', {
            id: alarmDelId(seq),
            size: [SWIPE_REVEAL_PX, 56],
            bg: COLOR_DANGER,
            radius: 14,
            align: ['rm', 0, 0],
            onClick: onDelClick
        }, [
            h('label', { id: "delL_" + seq, text: "删除", fg: COLOR_TEXT,
                         font: 'text', align: ['c', 0, 0] })
        ]),
        h('button', {
            id: alarmCardId(seq),
            size: [-100, 56],
            bg: COLOR_CARD,
            radius: 14,
            shadow: [0x000000, 10, 3],
            // 跟随当前手势位移：rerender 时不会把已露出的卡片瞬移回 0
            align: ['lm', (cardSwipe[seq] && cardSwipe[seq].x) || 0, 0],
            onPress:     onCardPress,
            onDrag:      onCardDrag,
            onRelease:   onCardRelease,
            onClick:     onCardClick,
            onLongPress: onCardLongPress
        }, [
            h('label', { id: "tag_" + seq, text: a.tag, fg: COLOR_TEXT_DIM,
                         font: 'text', align: ['lm', 14, -10] }),
            h('label', { id: alarmTimeId(seq), text: a.time,
                         fg: a.on ? COLOR_TEXT : COLOR_TEXT_OFF,
                         font: 'title', align: ['lm', 42, -10] }),
            h('label', { id: "sub_" + seq, text: a.sub, fg: COLOR_TEXT_DIM,
                         font: 'text', align: ['lm', 14, 14] }),
            h('button', {
                id: alarmSwitchId(seq),
                size: [36, 22],
                radius: 11,
                bg: a.on ? COLOR_ACCENT : COLOR_SWITCH_OFF,
                align: ['rm', -12, 0],
                onClick: onSwitchClick
            }, [
                h('label', { id: alarmKnobId(seq), text: " ", bg: COLOR_KNOB,
                             size: [16, 16], radius: 8,
                             align: ['lm', a.on ? 18 : 2, 0] })
            ])
        ])
    ]);
}

function viewEditView() {
    var idx = findIndexBySeq(state.editSeq);
    if (idx < 0) return null;
    var a = state.alarms[idx];
    return h('panel', { id: "editView", size: [-100, -100], bg: COLOR_BG,
                         align: ['tl', 0, 0], scrollable: false }, [
        h('panel', { id: "editHdr", size: [-100, 36], bg: COLOR_HEADER,
                     align: ['tm', 0, 0] }, [
            h('button', { id: "editBack", size: [60, 36], bg: COLOR_HEADER,
                          align: ['lm', 0, 0],
                          onClick: closeEditView }, [
                h('label', { id: "editBackL", text: sys.symbols.LEFT,
                             fg: COLOR_TEXT, font: 'text',
                             align: ['c', 0, 0] })
            ]),
            h('label', { id: "editTitle", text: "编辑闹钟", fg: COLOR_TEXT,
                         font: 'title', align: ['c', 0, 0] }),
            h('button', { id: "editDone", size: [60, 36], bg: COLOR_HEADER,
                          align: ['rm', 0, 0],
                          onClick: closeEditView }, [
                h('label', { id: "editDoneL", text: "完成",
                             fg: COLOR_ACCENT, font: 'text',
                             align: ['c', 0, 0] })
            ])
        ]),
        h('label', { id: "editLine1", text: a.tag + " " + a.time,
                     fg: COLOR_TEXT, font: 'huge',
                     align: ['tm', 0, 60] }),
        h('label', { id: "editLine2", text: a.sub,
                     fg: COLOR_TEXT_DIM, font: 'text',
                     align: ['tm', 0, 130] }),
        h('label', { id: "editHint", text: "（编辑功能待实现）",
                     fg: COLOR_TEXT_OFF, font: 'text',
                     align: ['tm', 0, 170] })
    ]);
}

function view() {
    var rowKids = [];
    var i;
    for (i = 0; i < state.alarms.length; i++) {
        rowKids.push(viewAlarmRow(state.alarms[i]));
    }

    var rootKids = [
        h('panel', { id: "header", size: [-100, 36], bg: COLOR_HEADER,
                     align: ['tm', 0, 0] }, [
            h('label', { id: "headerTitle", text: "闹钟", fg: COLOR_TEXT,
                         font: 'title', align: ['lm', 14, 0] }),
            h('label', { id: "headerMore", text: sys.symbols.BARS,
                         fg: COLOR_TEXT_DIM,
                         font: 'text', align: ['rm', -14, 0] })
        ]),
        h('panel', { id: "clockArea", size: [-100, 80], bg: COLOR_BG,
                     align: ['tm', 0, 36] }, [
            h('label', { id: "clockPeriod", text: state.clockPeriod,
                         fg: COLOR_TEXT_DIM,
                         font: 'text', align: ['c', -82, -2] }),
            h('label', { id: "clockMain", text: state.clockMain, fg: COLOR_TEXT,
                         font: 'huge', align: ['c', 14, 0] })
        ]),
        h('panel', { id: "statusBar", size: [-100, 20], bg: COLOR_BG,
                     align: ['tm', 0, 116] }, [
            h('label', { id: "statusLine", text: statusMsg(),
                         fg: COLOR_TEXT_DIM,
                         font: 'text', align: ['c', 0, 0] })
        ]),
        h('panel', { id: "list", size: [-100, 132], bg: COLOR_BG, flex: 'col',
                     align: ['tm', 0, 136],
                     pad: [8, 4, 8, 4],
                     gap: [6, 0],
                     scrollable: true }, rowKids),
        h('button', { id: "fab", size: [44, 44], radius: 22, bg: COLOR_ACCENT,
                      align: ['bm', 0, -8], onClick: onAddClick }, [
            h('label', { id: "fabL", text: "+", fg: COLOR_TEXT,
                         font: 'huge', align: ['c', 0, -4] })
        ])
    ];

    if (state.editSeq >= 0) {
        var ev = viewEditView();
        if (ev) rootKids.push(ev);
    }

    return h('panel', { id: "appRoot", size: [-100, -100], bg: COLOR_BG,
                         scrollable: false },
             rootKids);
}

function rerender() { VDOM.render(view(), null); }

// ---- 时钟 tick ----
function pad2(n) { n = n | 0; return n < 10 ? "0" + n : "" + n; }
function tickClock() {
    var totalSec = (sys.time.uptimeMs() / 1000) | 0;
    var s = totalSec % 60;
    var m = (totalSec / 60 | 0) % 60;
    var hh = (totalSec / 3600 | 0) % 24;
    state.clockPeriod = (hh < 6) ? "凌晨"
                      : (hh < 12) ? "上午"
                      : (hh < 13) ? "中午"
                      : (hh < 18) ? "下午"
                      : "晚上";
    state.clockMain = pad2(hh) + ":" + pad2(m) + ":" + pad2(s);
    rerender();
}

// ============================================================================
// §4. 启动
// ============================================================================

sys.log("alarm: build start");

rerender();
sys.ui.attachRootListener("appRoot");

tickClock();
setInterval(tickClock, 1000);

sys.log("alarm: build done");
