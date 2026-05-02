// ============================================================================
// gomoku_pkg —— BLE 联机五子棋（设备执黑先手 · PC 执白）
//
// 在线对战 only：双方必须同时在"五子棋页"才能玩。
//   - 双方互发 'present' 心跳（每 1500ms）
//   - 4 秒未收到对方心跳 → 判对方离场 = 我方赢
//   - 主动离场（PC 关页 / 设备退 app）发 'leave' 立即判对方赢
//   - 对手不在场期间禁止落子，HUD 提示"等待对手加入…"
//
// 协议（JSON over a3a30002/3）：
//   设备 → PC:  ble.send('present')                  // 心跳
//              ble.send('move',     { r, c })       // 落子
//              ble.send('reset')                    // 重开
//              ble.send('leave')                    // 主动离场
//              ble.send('sync',     { m, turn, over, win })
//   PC → 设备:  { type: 'present' }
//              { type: 'move',  body: { r, c } }
//              { type: 'reset' }
//              { type: 'leave' }
//              { type: 'sync_req' }                  // 上线后请求设备 push sync
// ============================================================================

var APP_NAME = 'gomoku_pkg';
var T = UI.T;

var BD_SIZE   = 13;
var CELL      = 16;
var MARGIN    = 12;
var BOARD_PX  = MARGIN * 2 + (BD_SIZE - 1) * CELL;   // 12+12+192 = 216
var BG_COLOR  = 0xE6C988;
var GRID_C    = 0x3A2A12;
var BLACK     = 0x101010;
var WHITE     = 0xF8F8F8;
var WIN_C     = 0xE74C3C;

var ble = makeBle(APP_NAME);

// --- 状态 ---------------------------------------------------------------
var board    = null;     // [BD_SIZE][BD_SIZE], 0=empty 1=B 2=W
var moves    = null;     // [[r,c], ...]
var gameOver = false;    // false | 'B' | 'W'
var winLine  = null;     // [[r,c], ...] 5 子连线

// 在线对战
var PRESENT_INTERVAL_MS = 1500;
var PRESENT_TIMEOUT_MS  = 4000;
var pcPresent       = false;        // PC 是否在线
var pcLastSeenMs    = 0;            // 最近一次收到 PC present 的 uptime
var heartbeatT      = 0;            // setInterval id
var watchdogT       = 0;            // setInterval id

function emptyBoard() {
    var b = [];
    for (var r = 0; r < BD_SIZE; r++) {
        var row = [];
        for (var c = 0; c < BD_SIZE; c++) row.push(0);
        b.push(row);
    }
    return b;
}

function nextTurn() {
    return (moves.length % 2 === 0) ? 'B' : 'W';
}

function rebuildBoardFromMoves() {
    board = emptyBoard();
    for (var i = 0; i < moves.length; i++) {
        var m = moves[i];
        if (m[0] < 0 || m[0] >= BD_SIZE || m[1] < 0 || m[1] >= BD_SIZE) continue;
        board[m[0]][m[1]] = (i % 2 === 0) ? 1 : 2;
    }
}

// --- 绘图 ---------------------------------------------------------------
function drawGrid() {
    sys.canvas.fill('bd', BG_COLOR);
    var L = MARGIN, R = MARGIN + (BD_SIZE - 1) * CELL;
    for (var r = 0; r < BD_SIZE; r++) {
        var y = MARGIN + r * CELL;
        sys.canvas.line('bd', L, y, R, y, GRID_C, 1);
    }
    for (var c = 0; c < BD_SIZE; c++) {
        var x = MARGIN + c * CELL;
        sys.canvas.line('bd', x, MARGIN, x, R, GRID_C, 1);
    }
    var stars = [[3,3],[3,9],[9,3],[9,9],[6,6]];
    for (var s = 0; s < stars.length; s++) {
        var sx = MARGIN + stars[s][1] * CELL;
        var sy = MARGIN + stars[s][0] * CELL;
        sys.canvas.line('bd', sx, sy, sx, sy, GRID_C, 3);
    }
}

function drawStone(r, c, isBlack) {
    var cx = MARGIN + c * CELL;
    var cy = MARGIN + r * CELL;
    var col = isBlack ? BLACK : WHITE;
    sys.canvas.line('bd', cx, cy, cx, cy, col, 11);
    if (!isBlack) {
        // 白子加描边四点，避免和木色背景糊在一起
        sys.canvas.setPixel('bd', cx - 5, cy,     BLACK);
        sys.canvas.setPixel('bd', cx + 5, cy,     BLACK);
        sys.canvas.setPixel('bd', cx,     cy - 5, BLACK);
        sys.canvas.setPixel('bd', cx,     cy + 5, BLACK);
    }
}

function redrawAllStones() {
    for (var i = 0; i < moves.length; i++) {
        drawStone(moves[i][0], moves[i][1], (i % 2 === 0));
    }
}

function drawWinLineGfx(line) {
    if (!line || line.length < 2) return;
    var s = line[0], e = line[line.length - 1];
    var x0 = MARGIN + s[1] * CELL;
    var y0 = MARGIN + s[0] * CELL;
    var x1 = MARGIN + e[1] * CELL;
    var y1 = MARGIN + e[0] * CELL;
    sys.canvas.line('bd', x0, y0, x1, y1, WIN_C, 2);
}

// --- 判胜 ---------------------------------------------------------------
function checkWin(r, c) {
    var color = board[r][c];
    if (!color) return null;
    var dirs = [[0,1],[1,0],[1,1],[1,-1]];
    for (var d = 0; d < 4; d++) {
        var dr = dirs[d][0], dc = dirs[d][1];
        var line = [[r, c]];
        var nr = r + dr, nc = c + dc;
        while (nr >= 0 && nr < BD_SIZE && nc >= 0 && nc < BD_SIZE &&
               board[nr][nc] === color) {
            line.push([nr, nc]); nr += dr; nc += dc;
        }
        nr = r - dr; nc = c - dc;
        while (nr >= 0 && nr < BD_SIZE && nc >= 0 && nc < BD_SIZE &&
               board[nr][nc] === color) {
            line.unshift([nr, nc]); nr -= dr; nc -= dc;
        }
        if (line.length >= 5) return line;
    }
    return null;
}

// --- 落子 ---------------------------------------------------------------
function applyMove(r, c, fromPC) {
    if (gameOver) return false;
    if (r < 0 || r >= BD_SIZE || c < 0 || c >= BD_SIZE) return false;
    if (board[r][c] !== 0) return false;
    var idx = moves.length;
    var isBlack = (idx % 2 === 0);
    moves.push([r, c]);
    board[r][c] = isBlack ? 1 : 2;
    drawStone(r, c, isBlack);
    var line = checkWin(r, c);
    if (line) {
        gameOver = isBlack ? 'B' : 'W';
        winLine = line;
        drawWinLineGfx(line);
        bumpRecord(gameOver === 'B');
    }
    refreshTurnLabel();
    if (gameOver) announceWin();
    if (fromPC !== true) {
        // 设备落子 → 推 PC
        ble.send('move', { r: r, c: c });
    }
    return true;
}

function resetGame(fromPC) {
    moves = [];
    board = emptyBoard();
    gameOver = false;
    winLine = null;
    drawGrid();
    refreshTurnLabel();
    if (fromPC !== true) {
        ble.send('reset');
    }
}

function announceWin() {
    var who = (gameOver === 'B') ? '黑方（你）' : '白方（PC）';
    UI.modal({
        title: who + '胜！',
        body:  '战绩 ' + record.wins + ' 胜 ' + record.losses + ' 负',
        action1: '好的'
    });
}

// 任一方离场（自己退/对方退/超时）= 还在场的人赢
function settleByLeave(reasonText) {
    if (gameOver) return;
    gameOver = 'B';            // 设备这边永远是黑，自己还在 = 黑赢
    winLine = null;
    bumpRecord(true);
    refreshTurnLabel();
    UI.modal({
        title: '你赢了',
        body:  reasonText + '\n战绩 ' + record.wins + ' 胜 ' + record.losses + ' 负',
        action1: '好的'
    });
}

// --- 持久化（仅战绩，不存棋谱）------------------------------------------
var record = { wins: 0, losses: 0 };

function loadRecord() {
    var raw = sys.app.loadState();
    if (!raw) return;
    try {
        var s = JSON.parse(raw);
        if (s && typeof s.w === 'number') record.wins = s.w | 0;
        if (s && typeof s.l === 'number') record.losses = s.l | 0;
    } catch (eLoad) {
        sys.log('gomoku: bad record, reset');
    }
}

function bumpRecord(iWon) {
    if (iWon) record.wins   += 1;
    else      record.losses += 1;
    sys.app.saveState(JSON.stringify({ w: record.wins, l: record.losses }));
}

// --- UI -----------------------------------------------------------------
function refreshTurnLabel() {
    var txt;
    if (gameOver) {
        txt = (gameOver === 'B') ? '黑方胜（你）' : '白方胜（PC）';
    } else if (!pcPresent) {
        txt = '等待 PC 加入…';
    } else {
        txt = (nextTurn() === 'B') ? '黑方回合（你）' : '等待 PC 落子…';
    }
    VDOM.set('hud_l', { text: txt });
    var sub = '战绩 ' + record.wins + '胜 ' + record.losses + '负 · ' +
              (pcPresent ? 'PC 在线' : 'PC 离线');
    VDOM.set('hud_sub', { text: sub, fg: pcPresent ? T.C_OK : T.C_TEXT_MUTED });
}

function buildUI() {
    var hudL = h('panel', {
        id: 'hud_lc', flex: 'col', scrollable: false,
        flexAlign: ['center', 'start', 'center'],
        gap: [0, 0], grow: 1
    }, [
        h('label', { id: 'hud_l',   text: '加载中…',
                     fg: T.C_TEXT,       font: 'text' }),
        h('label', { id: 'hud_sub', text: '',
                     fg: T.C_TEXT_MUTED, font: 'text' })
    ]);

    var hud = h('panel', {
        id: 'hud',
        size: [-100, 36],
        bg: T.C_PANEL,
        scrollable: false,
        align: ['tl', 0, 0],
        flex: 'row',
        flexAlign: ['between', 'center', 'center'],
        pad: [10, 2, 6, 2],
        border: { color: T.C_BORDER, width: 1, side: 'bottom', opa: 102 }
    }, [
        hudL,
        h('button', {
            id: 'hud_resign', size: [44, 22], bg: T.C_PANEL_HI, radius: 11,
            scrollable: false,
            onClick: askResign
        }, [
            h('label', { id: 'hud_rg_l', text: '认输',
                         fg: T.C_ERR, font: 'text', align: ['c', 0, 0] })
        ]),
        h('button', {
            id: 'hud_reset', size: [44, 22], bg: T.C_PANEL_HI, radius: 11,
            scrollable: false,
            onClick: askReset
        }, [
            h('label', { id: 'hud_rs_l', text: '重开',
                         fg: T.C_ACCENT, font: 'text', align: ['c', 0, 0] })
        ])
    ]);

    var canvas = h('canvas', {
        id: 'bd',
        w: BOARD_PX,
        h: BOARD_PX,
        align: ['tm', 0, 36],
        onPress: function (ev) {
            if (gameOver) return;
            if (!pcPresent) {
                UI.toast('PC 未在五子棋页', 600);
                return;
            }
            if (nextTurn() !== 'B') {
                UI.toast('等待 PC 落子', 500);
                return;
            }
            var c = Math.round((ev.dx - MARGIN) / CELL);
            var r = Math.round((ev.dy - MARGIN) / CELL);
            if (r < 0 || r >= BD_SIZE || c < 0 || c >= BD_SIZE) return;
            if (board[r][c] !== 0) {
                UI.toast('该处已有子', 500);
                return;
            }
            applyMove(r, c, false);
        }
    });

    var page = h('panel', {
        id: 'root', size: [-100, -100], bg: T.C_BG,
        scrollable: false, pad: [0, 0, 0, 0]
    }, [hud, canvas]);

    VDOM.mount(page, null);
}

// --- 交互 ---------------------------------------------------------------
function askReset() {
    UI.modal({
        title: '重新开始？',
        body:  '当前棋局将被清空',
        action0: '取消',
        action1: '重开',
        onResult: function (idx) {
            if (idx === 1) resetGame(false);
        }
    });
}

function askResign() {
    if (gameOver) { UI.toast('已结束', 500); return; }
    UI.modal({
        title: '认输？',
        body:  '认输后白方获胜',
        action0: '取消',
        action1: '认输',
        onResult: function (idx) {
            if (idx !== 1) return;
            gameOver = 'W';
            winLine = null;
            bumpRecord(false);
            refreshTurnLabel();
            ble.send('resign');
            announceWin();
        }
    });
}

// --- BLE ----------------------------------------------------------------
function markPCSeen() {
    var first = !pcPresent;
    pcPresent = true;
    pcLastSeenMs = sys.time.uptimeMs();
    if (first) {
        refreshTurnLabel();
        // PC 第一次出现 → push 当前棋局让其同步
        pushSync();
        UI.toast('PC 已加入', 600);
    }
}

ble.on('present', function () { markPCSeen(); });

ble.on('move', function (msg) {
    markPCSeen();
    var b = msg.body;
    if (!b || typeof b.r !== 'number' || typeof b.c !== 'number') return;
    if (gameOver) {
        sys.log('gomoku: drop PC move (game over)');
        return;
    }
    if (nextTurn() !== 'W') {
        sys.log('gomoku: drop PC move (not W turn)');
        return;
    }
    applyMove(b.r, b.c, true);
});

ble.on('reset', function () {
    markPCSeen();
    resetGame(true);
    UI.toast('PC 重开对局', 800);
});

ble.on('resign', function () {
    markPCSeen();
    if (gameOver) return;
    gameOver = 'B';
    winLine = null;
    bumpRecord(true);
    refreshTurnLabel();
    UI.toast('PC 认输', 800);
    announceWin();
});

ble.on('leave', function () {
    if (gameOver) {
        pcPresent = false;
        refreshTurnLabel();
        return;
    }
    pcPresent = false;
    settleByLeave('PC 退出五子棋页');
});

ble.on('sync_req', function () { markPCSeen(); pushSync(); });

function pushSync() {
    ble.send('sync', {
        m:    encMoves(moves),
        turn: gameOver ? 'X' : nextTurn(),
        over: gameOver ? gameOver : 0,
        win:  winLine ? encMoves(winLine) : 0
    });
}

// moves 紧凑编码：每步 (r*BD_SIZE+c) 转 2 hex 字符。
// BD_SIZE=13 → r*c<169 < 256，1 字节够。
// 200B 单帧扣掉 JSON 头 ~60B，剩 ~140 字符 → 70 步上限，够实战。
var HEX = '0123456789abcdef';
function encMoves(arr) {
    var s = '';
    for (var i = 0; i < arr.length; i++) {
        var n = arr[i][0] * BD_SIZE + arr[i][1];
        s += HEX.charAt((n >> 4) & 0xF) + HEX.charAt(n & 0xF);
    }
    return s;
}

// --- 入口 ---------------------------------------------------------------
sys.log('gomoku: build start');
loadRecord();
moves = []; board = emptyBoard(); gameOver = false; winLine = null;
buildUI();
sys.ui.attachRootListener('root');
drawGrid();
refreshTurnLabel();

// 心跳（每 1500ms 通告自己在场）
heartbeatT = setInterval(function () {
    ble.send('present');
}, PRESENT_INTERVAL_MS);

// watchdog（每 1000ms 看 PC 心跳是否超时）
watchdogT = setInterval(function () {
    if (gameOver || !pcPresent) return;
    if (sys.time.uptimeMs() - pcLastSeenMs > PRESENT_TIMEOUT_MS) {
        pcPresent = false;
        settleByLeave('PC 已离线（' + (PRESENT_TIMEOUT_MS / 1000) + 's 无心跳）');
    }
}, 1000);

// 立即先发一次 present + sync_req 邀请 PC 应答
ble.send('present');
ble.send('sync_req');

sys.log('gomoku: build done · record ' + record.wins + 'W ' + record.losses + 'L');
