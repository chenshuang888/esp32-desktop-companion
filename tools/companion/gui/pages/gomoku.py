"""Gomoku 页：五子棋联机对战（强在线）。

设备执黑先手 / PC 执白后手。**双方必须同时在五子棋页才能玩**。
  - 双方互发 'present' 心跳（每 1500ms）
  - 4 秒未收到对方心跳 → 判对方离场 = 我方赢
  - 主动离场（PC 切页/关 GUI/断 BLE）发 'leave'，对方立即判赢
  - PC 进入本页时启动心跳；离开（切页 / Unmap）时停心跳并发 leave

棋谱单一事实源在设备端。本页是瘦客户端：
  收到 ble bus "gomoku:rx" → (type, body)
        present                            → 标记设备在场
        sync   { m, turn, over, win }     → 整盘重建
        move   { r, c }                   → 设备落了一子
        reset                             → 设备清盘
        resign                            → 设备认输（PC 赢）
        leave                             → 设备退出 = PC 赢
  本页 → 设备 emit "gomoku:tx" → (type, body)
        ('present', None) 心跳
        ('move',    {r, c})
        ('reset',   None)
        ('resign',  None)
        ('leave',   None)
        ('sync_req',None)
"""

from __future__ import annotations

import logging
import time
import tkinter as tk
from typing import Any, Optional

import customtkinter as ctk

from ..theme import (
    COLOR_ACCENT, COLOR_ACCENT2, COLOR_BG, COLOR_ERR, COLOR_MUTED,
    COLOR_OK, COLOR_PANEL, COLOR_PANEL_HI, COLOR_TEXT, COLOR_WARN,
)
from ..widgets import Card

logger = logging.getLogger(__name__)


BD_SIZE  = 13
PRESENT_INTERVAL_MS = 1500
PRESENT_TIMEOUT_MS  = 4000
WATCHDOG_TICK_MS    = 1000


def _dec_moves(s: Any) -> list[list[int]]:
    if not isinstance(s, str) or len(s) % 2 != 0:
        return []
    out: list[list[int]] = []
    for i in range(0, len(s), 2):
        try:
            n = int(s[i:i + 2], 16)
        except ValueError:
            return []
        out.append([n // BD_SIZE, n % BD_SIZE])
    return out


CELL     = 28
MARGIN   = 18
BOARD_PX = MARGIN * 2 + (BD_SIZE - 1) * CELL   # 372

BD_BG    = "#D4A867"
GRID_C   = "#3A2A12"
BLACK_C  = "#101010"
WHITE_C  = "#F8F8F8"
WIN_C    = "#E74C3C"
HOVER_C  = "#06B6D4"


class GomokuPage(ctk.CTkFrame):
    def __init__(self, master, app) -> None:
        super().__init__(master, fg_color="transparent")
        self._app = app

        self._moves: list[list[int]] = []
        self._over: Any = 0                        # 0 / 'B' / 'W'
        self._win_line: list[list[int]] | None = None
        self._turn: str = 'B'
        self._connected: bool = False              # BLE 是否连着
        self._on_page: bool = False                # 本页是否处于显示状态
        self._dev_present: bool = False
        self._dev_last_seen_ms: float = 0.0
        self._heartbeat_after: str | None = None
        self._watchdog_after: str | None = None
        self._hover: tuple[int, int] | None = None
        self._stone_ids: list[int] = []
        self._win_id: int | None = None
        self._hover_id: int | None = None

        self._build()
        self._bind_bus()
        # 关键：本 frame 的 <Map>/<Unmap> 触发进/离场逻辑
        self.bind("<Map>",   self._on_page_enter)
        self.bind("<Unmap>", self._on_page_leave)
        self._redraw()
        self._refresh_hud()

    # ------------------------------------------------------------------
    # build
    # ------------------------------------------------------------------

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # 左：棋盘
        left = Card(self)
        left.grid(row=0, column=0, sticky="nsew", padx=(20, 8), pady=20)
        left.grid_columnconfigure(0, weight=1)
        left.grid_rowconfigure(1, weight=1)

        ctk.CTkLabel(left, text="五子棋（你执白）",
                      text_color=COLOR_ACCENT,
                      font=ctk.CTkFont(size=16, weight="bold")) \
            .grid(row=0, column=0, padx=14, pady=(12, 6), sticky="w")

        self._canvas = tk.Canvas(left, width=BOARD_PX, height=BOARD_PX,
                                  bg=BD_BG, highlightthickness=0,
                                  borderwidth=0)
        self._canvas.grid(row=1, column=0, padx=14, pady=(0, 14))
        self._canvas.bind("<Button-1>", self._on_click)
        self._canvas.bind("<Motion>",   self._on_hover)
        self._canvas.bind("<Leave>",    self._on_leave_canvas)

        # 右：信息 + 操作
        right = Card(self)
        right.grid(row=0, column=1, sticky="nsew", padx=(8, 20), pady=20)
        right.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(right, text="对局信息",
                      text_color=COLOR_MUTED,
                      font=ctk.CTkFont(size=12)) \
            .grid(row=0, column=0, padx=14, pady=(14, 4), sticky="w")

        self._hud = ctk.CTkLabel(right, text="未在页面",
                                  text_color=COLOR_TEXT,
                                  font=ctk.CTkFont(size=18, weight="bold"),
                                  anchor="w")
        self._hud.grid(row=1, column=0, padx=14, pady=(0, 4), sticky="ew")

        self._sub = ctk.CTkLabel(right, text="—",
                                  text_color=COLOR_MUTED, anchor="w")
        self._sub.grid(row=2, column=0, padx=14, pady=(0, 4), sticky="ew")

        self._presence = ctk.CTkLabel(right, text="● 设备 离线",
                                        text_color=COLOR_MUTED, anchor="w")
        self._presence.grid(row=3, column=0, padx=14, pady=(0, 14), sticky="ew")

        # 按钮组
        btn_row = ctk.CTkFrame(right, fg_color="transparent")
        btn_row.grid(row=4, column=0, padx=14, pady=4, sticky="ew")
        btn_row.grid_columnconfigure((0, 1), weight=1)

        ctk.CTkButton(btn_row, text="重开",
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       text_color=COLOR_TEXT,
                       command=self._on_reset) \
            .grid(row=0, column=0, padx=(0, 4), sticky="ew")
        ctk.CTkButton(btn_row, text="认输",
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ERR,
                       text_color=COLOR_ERR,
                       command=self._on_resign) \
            .grid(row=0, column=1, padx=(4, 0), sticky="ew")

        # 棋谱
        ctk.CTkLabel(right, text="棋谱",
                      text_color=COLOR_MUTED,
                      font=ctk.CTkFont(size=12)) \
            .grid(row=5, column=0, padx=14, pady=(20, 4), sticky="w")
        self._log = ctk.CTkTextbox(right, fg_color=COLOR_PANEL_HI,
                                    text_color=COLOR_TEXT,
                                    border_width=0, height=180)
        self._log.grid(row=6, column=0, padx=14, pady=(0, 14), sticky="nsew")
        right.grid_rowconfigure(6, weight=1)

    # ------------------------------------------------------------------
    # bus
    # ------------------------------------------------------------------

    def _bind_bus(self) -> None:
        self._app.bus.on("gomoku:rx", lambda p: self._app.root.after(
            0, self._on_rx, p))
        self._app.bus.on("connect",    lambda _: self._app.root.after(
            0, self._on_conn, True))
        self._app.bus.on("disconnect", lambda _: self._app.root.after(
            0, self._on_conn, False))

    def _on_conn(self, ok: bool) -> None:
        self._connected = ok
        if not ok:
            # BLE 断开等价于设备离场
            self._mark_dev_left("BLE 已断开")
        self._refresh_hud()

    def _on_rx(self, payload: Any) -> None:
        if not self._on_page:
            # 不在本页就忽略所有消息（设备会因为没收到 PC present 而判它赢）
            return
        if not isinstance(payload, tuple) or len(payload) != 2:
            return
        mtype, body = payload
        body = body if isinstance(body, dict) else {}
        # 任何业务消息都隐含"我在场"
        self._mark_dev_seen()
        if mtype == "present":
            return
        if mtype == "sync":
            self._moves = _dec_moves(body.get("m"))
            self._over = body.get("over") or 0
            wraw = body.get("win")
            self._win_line = _dec_moves(wraw) if isinstance(wraw, str) else None
            self._turn = str(body.get("turn") or 'B')
            self._redraw()
            self._refresh_hud()
            self._refresh_log()
        elif mtype == "move":
            r = body.get("r"); c = body.get("c")
            if isinstance(r, int) and isinstance(c, int):
                self._apply_local_move(r, c)
        elif mtype == "reset":
            self._moves = []
            self._over = 0
            self._win_line = None
            self._turn = 'B'
            self._redraw()
            self._refresh_hud()
            self._refresh_log()
        elif mtype == "resign":
            # 设备认输 = PC 赢
            self._over = 'W'
            self._win_line = None
            self._turn = 'X'
            self._refresh_hud()
            self._refresh_log()
        elif mtype == "leave":
            self._mark_dev_left("设备退出五子棋页")

    # ------------------------------------------------------------------
    # 进/离 page
    # ------------------------------------------------------------------

    def _on_page_enter(self, _ev: tk.Event) -> None:
        if self._on_page:
            return
        self._on_page = True
        # 重置局内 in-memory 状态（棋谱由设备 push）
        self._dev_present = False
        self._dev_last_seen_ms = 0.0
        self._moves = []; self._over = 0; self._win_line = None
        self._turn = 'B'
        self._redraw(); self._refresh_log(); self._refresh_hud()
        # 进场即心跳 + 请求 sync
        if self._connected:
            self._tx("present", None)
            self._tx("sync_req", None)
        self._start_heartbeat()
        self._start_watchdog()

    def _on_page_leave(self, _ev: tk.Event) -> None:
        if not self._on_page:
            return
        self._on_page = False
        self._stop_heartbeat()
        self._stop_watchdog()
        if self._connected:
            self._tx("leave", None)
        self._dev_present = False
        self._refresh_hud()

    # ------------------------------------------------------------------
    # 心跳 / watchdog
    # ------------------------------------------------------------------

    def _start_heartbeat(self) -> None:
        self._stop_heartbeat()
        def _tick() -> None:
            if not self._on_page:
                return
            if self._connected:
                self._tx("present", None)
            self._heartbeat_after = self.after(PRESENT_INTERVAL_MS, _tick)
        # 首发已在 _on_page_enter 发过一次；这里间隔启动
        self._heartbeat_after = self.after(PRESENT_INTERVAL_MS, _tick)

    def _stop_heartbeat(self) -> None:
        if self._heartbeat_after is not None:
            try: self.after_cancel(self._heartbeat_after)
            except Exception: pass
            self._heartbeat_after = None

    def _start_watchdog(self) -> None:
        self._stop_watchdog()
        def _tick() -> None:
            if not self._on_page:
                return
            if not self._over and self._dev_present and \
                    (time.monotonic() * 1000 -
                     self._dev_last_seen_ms) > PRESENT_TIMEOUT_MS:
                self._mark_dev_left("设备已离线（{}s 无心跳）".format(
                    PRESENT_TIMEOUT_MS // 1000))
            self._watchdog_after = self.after(WATCHDOG_TICK_MS, _tick)
        self._watchdog_after = self.after(WATCHDOG_TICK_MS, _tick)

    def _stop_watchdog(self) -> None:
        if self._watchdog_after is not None:
            try: self.after_cancel(self._watchdog_after)
            except Exception: pass
            self._watchdog_after = None

    def _mark_dev_seen(self) -> None:
        was = self._dev_present
        self._dev_present = True
        self._dev_last_seen_ms = time.monotonic() * 1000
        if not was:
            self._refresh_hud()

    def _mark_dev_left(self, reason: str) -> None:
        if not self._dev_present and not self._over:
            # 从来没见过设备：只更新文案
            self._refresh_hud()
            return
        self._dev_present = False
        if self._over:
            self._refresh_hud()
            return
        # 还在对局中 → PC（白）赢
        self._over = 'W'
        self._win_line = None
        self._turn = 'X'
        self._refresh_hud()
        self._sub.configure(text=reason, text_color=COLOR_OK)

    # ------------------------------------------------------------------
    # 用户交互
    # ------------------------------------------------------------------

    def _on_click(self, ev: tk.Event) -> None:
        if not self._connected:
            self._flash_sub("未连接 ESP32", COLOR_ERR); return
        if not self._dev_present:
            self._flash_sub("设备未在五子棋页", COLOR_ERR); return
        if self._over:
            self._flash_sub("对局已结束，请先重开", COLOR_WARN); return
        if self._next_color() != 'W':
            self._flash_sub("等待设备落子…", COLOR_MUTED); return
        rc = self._xy_to_rc(ev.x, ev.y)
        if rc is None:
            return
        r, c = rc
        if self._cell_used(r, c):
            self._flash_sub("该处已有子", COLOR_WARN); return
        # 乐观更新本地
        self._apply_local_move(r, c)
        self._tx("move", {"r": r, "c": c})

    def _on_hover(self, ev: tk.Event) -> None:
        if self._over or self._next_color() != 'W' or \
                not self._connected or not self._dev_present:
            self._clear_hover(); return
        rc = self._xy_to_rc(ev.x, ev.y)
        if rc is None or self._cell_used(*rc):
            self._clear_hover(); return
        if self._hover == rc:
            return
        self._hover = rc
        self._draw_hover()

    def _on_leave_canvas(self, _ev: tk.Event) -> None:
        self._clear_hover()

    def _on_reset(self) -> None:
        if not self._can_act(): return
        self._tx("reset", None)
        self._moves = []; self._over = 0; self._win_line = None
        self._turn = 'B'
        self._redraw(); self._refresh_hud(); self._refresh_log()

    def _on_resign(self) -> None:
        if not self._can_act(): return
        if self._over:
            self._flash_sub("对局已结束", COLOR_WARN); return
        self._over = 'B'
        self._win_line = None
        self._turn = 'X'
        self._tx("resign", None)
        self._refresh_hud(); self._refresh_log()

    def _can_act(self) -> bool:
        if not self._connected:
            self._flash_sub("未连接", COLOR_ERR); return False
        if not self._dev_present:
            self._flash_sub("设备未在五子棋页", COLOR_ERR); return False
        return True

    # ------------------------------------------------------------------
    # 模型
    # ------------------------------------------------------------------

    def _next_color(self) -> str:
        return 'B' if (len(self._moves) % 2 == 0) else 'W'

    def _cell_used(self, r: int, c: int) -> bool:
        for m in self._moves:
            if m[0] == r and m[1] == c:
                return True
        return False

    def _xy_to_rc(self, x: int, y: int) -> Optional[tuple[int, int]]:
        c = round((x - MARGIN) / CELL)
        r = round((y - MARGIN) / CELL)
        if r < 0 or r >= BD_SIZE or c < 0 or c >= BD_SIZE:
            return None
        cx = MARGIN + c * CELL; cy = MARGIN + r * CELL
        if abs(x - cx) > CELL // 2 or abs(y - cy) > CELL // 2:
            return None
        return (r, c)

    def _apply_local_move(self, r: int, c: int) -> None:
        if self._cell_used(r, c) or self._over:
            return
        self._moves.append([r, c])
        line = self._check_win(r, c)
        if line:
            color = 'B' if (len(self._moves) - 1) % 2 == 0 else 'W'
            self._over = color
            self._win_line = line
        self._redraw(); self._refresh_hud(); self._refresh_log()

    def _check_win(self, r: int, c: int) -> list[list[int]] | None:
        board = [[0] * BD_SIZE for _ in range(BD_SIZE)]
        for i, m in enumerate(self._moves):
            board[m[0]][m[1]] = 1 if (i % 2 == 0) else 2
        color = board[r][c]
        if not color:
            return None
        for dr, dc in ((0, 1), (1, 0), (1, 1), (1, -1)):
            line = [[r, c]]
            nr, nc = r + dr, c + dc
            while 0 <= nr < BD_SIZE and 0 <= nc < BD_SIZE \
                    and board[nr][nc] == color:
                line.append([nr, nc]); nr += dr; nc += dc
            nr, nc = r - dr, c - dc
            while 0 <= nr < BD_SIZE and 0 <= nc < BD_SIZE \
                    and board[nr][nc] == color:
                line.insert(0, [nr, nc]); nr -= dr; nc -= dc
            if len(line) >= 5:
                return line
        return None

    # ------------------------------------------------------------------
    # 绘图
    # ------------------------------------------------------------------

    def _redraw(self) -> None:
        self._canvas.delete("all")
        self._stone_ids = []
        self._win_id = None
        self._hover_id = None
        self._hover = None
        L = MARGIN; R = MARGIN + (BD_SIZE - 1) * CELL
        for r in range(BD_SIZE):
            y = MARGIN + r * CELL
            self._canvas.create_line(L, y, R, y, fill=GRID_C, width=1)
        for c in range(BD_SIZE):
            x = MARGIN + c * CELL
            self._canvas.create_line(x, MARGIN, x, R, fill=GRID_C, width=1)
        for sr, sc in [(3, 3), (3, 9), (9, 3), (9, 9), (6, 6)]:
            sx = MARGIN + sc * CELL; sy = MARGIN + sr * CELL
            self._canvas.create_oval(sx - 3, sy - 3, sx + 3, sy + 3,
                                       fill=GRID_C, outline=GRID_C)
        radius = CELL // 2 - 2
        for i, m in enumerate(self._moves):
            cx = MARGIN + m[1] * CELL; cy = MARGIN + m[0] * CELL
            color = BLACK_C if (i % 2 == 0) else WHITE_C
            outline = "#000000" if (i % 2 == 1) else BLACK_C
            sid = self._canvas.create_oval(
                cx - radius, cy - radius, cx + radius, cy + radius,
                fill=color, outline=outline, width=1)
            self._stone_ids.append(sid)
            if i == len(self._moves) - 1 and not self._over:
                self._canvas.create_rectangle(
                    cx - radius - 2, cy - radius - 2,
                    cx + radius + 2, cy + radius + 2,
                    outline=COLOR_ACCENT, width=2)
        if self._win_line and len(self._win_line) >= 2:
            s = self._win_line[0]; e = self._win_line[-1]
            x0 = MARGIN + s[1] * CELL; y0 = MARGIN + s[0] * CELL
            x1 = MARGIN + e[1] * CELL; y1 = MARGIN + e[0] * CELL
            self._win_id = self._canvas.create_line(
                x0, y0, x1, y1, fill=WIN_C, width=3)

    def _draw_hover(self) -> None:
        self._clear_hover()
        if self._hover is None:
            return
        r, c = self._hover
        cx = MARGIN + c * CELL; cy = MARGIN + r * CELL
        radius = CELL // 2 - 4
        self._hover_id = self._canvas.create_oval(
            cx - radius, cy - radius, cx + radius, cy + radius,
            outline=HOVER_C, width=2)

    def _clear_hover(self) -> None:
        if self._hover_id is not None:
            self._canvas.delete(self._hover_id)
            self._hover_id = None
        self._hover = None

    # ------------------------------------------------------------------
    # HUD
    # ------------------------------------------------------------------

    def _refresh_hud(self) -> None:
        # 主标题：胜负 / 我方回合 / 对手回合 / 等待加入
        if not self._on_page:
            self._hud.configure(text="未在页面", text_color=COLOR_MUTED)
        elif not self._connected:
            self._hud.configure(text="未连接 ESP32", text_color=COLOR_ERR)
        elif self._over == 'B':
            self._hud.configure(text="黑方胜（设备）", text_color=COLOR_ERR)
        elif self._over == 'W':
            self._hud.configure(text="白方胜（你）", text_color=COLOR_OK)
        elif not self._dev_present:
            self._hud.configure(text="等待设备加入…", text_color=COLOR_WARN)
        elif self._next_color() == 'W':
            self._hud.configure(text="● 你的回合（白）", text_color=COLOR_ACCENT)
        else:
            self._hud.configure(text="○ 等待设备（黑）", text_color=COLOR_ACCENT2)

        # 副标题：手数 / 提示
        if self._over:
            self._sub.configure(
                text=f"共 {len(self._moves)} 手 · 点重开开始下一局",
                text_color=COLOR_MUTED)
        elif not self._connected:
            self._sub.configure(text="先在首页连接设备", text_color=COLOR_MUTED)
        elif not self._dev_present:
            self._sub.configure(text="让设备打开五子棋 app", text_color=COLOR_MUTED)
        else:
            self._sub.configure(text=f"已下 {len(self._moves)} 手",
                                 text_color=COLOR_MUTED)

        # 在场指示
        if self._dev_present:
            self._presence.configure(text="● 设备 在场",
                                       text_color=COLOR_OK)
        else:
            self._presence.configure(text="● 设备 离场",
                                       text_color=COLOR_MUTED)

    def _flash_sub(self, text: str, color: str) -> None:
        self._sub.configure(text=text, text_color=color)

    def _refresh_log(self) -> None:
        self._log.delete("1.0", "end")
        if not self._moves:
            self._log.insert("1.0", "（空）")
            return
        cols = "ABCDEFGHJKLMN"
        lines = []
        for i, m in enumerate(self._moves):
            r, c = m[0], m[1]
            tag = "黑" if (i % 2 == 0) else "白"
            label = f"{cols[c]}{BD_SIZE - r}"
            lines.append(f"{i+1:>3}. {tag} {label}")
        self._log.insert("1.0", "\n".join(lines))

    # ------------------------------------------------------------------
    # tx
    # ------------------------------------------------------------------

    def _tx(self, mtype: str, body: Any) -> None:
        self._app.bus.emit_threadsafe("gomoku:tx", (mtype, body))

