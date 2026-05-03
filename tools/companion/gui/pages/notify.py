"""Notify 页：手动通知输入 → notify_provider。

定位：本页是**原生 NotifyProvider 的 PC 调试入口**（手动构造一条通知推到设备原生
通知页）。它属于 `providers/native/` 这条线的配套 GUI，**不要**和 `plugins/notif/`
混淆——后者是动态 app 端 notif_pkg 的代理（监听 Win 系统通知 → BLE 推送），
两者目标和数据流都不同。

通过 bus.emit("notify:manual", (title, body, category)) 触发。
保留 ble_time_sync 独有的"手动推送"功能。
"""

from __future__ import annotations

import customtkinter as ctk

from ...constants import (
    NOTIFY_CAT_ALERT, NOTIFY_CAT_CALENDAR, NOTIFY_CAT_CALL, NOTIFY_CAT_EMAIL,
    NOTIFY_CAT_GENERIC, NOTIFY_CAT_MESSAGE, NOTIFY_CAT_NEWS, NOTIFY_CAT_SOCIAL,
)
from ..theme import (
    COLOR_ACCENT, COLOR_ERR, COLOR_MUTED, COLOR_OK, COLOR_PANEL_HI,
    COLOR_TEXT, COLOR_WARN,
)
from ..widgets import Card

CATEGORIES = [
    ("通用",     NOTIFY_CAT_GENERIC),
    ("消息",     NOTIFY_CAT_MESSAGE),
    ("邮件",     NOTIFY_CAT_EMAIL),
    ("电话",     NOTIFY_CAT_CALL),
    ("日历",     NOTIFY_CAT_CALENDAR),
    ("社交",     NOTIFY_CAT_SOCIAL),
    ("新闻",     NOTIFY_CAT_NEWS),
    ("警告",     NOTIFY_CAT_ALERT),
]


class NotifyPage(ctk.CTkFrame):
    def __init__(self, master, app) -> None:
        super().__init__(master, fg_color="transparent")
        self._app = app
        self._build()

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(2, weight=1)

        form = Card(self)
        form.grid(row=0, column=0, sticky="ew", padx=20, pady=(20, 8))
        form.grid_columnconfigure(1, weight=1)

        ctk.CTkLabel(form, text="标题", text_color=COLOR_MUTED,
                      width=80, anchor="w") \
            .grid(row=0, column=0, padx=(14, 4), pady=10)
        self._title = ctk.CTkEntry(form, fg_color=COLOR_PANEL_HI,
                                    text_color=COLOR_TEXT, border_width=0)
        self._title.grid(row=0, column=1, columnspan=2, sticky="ew",
                          padx=(4, 14), pady=10)

        ctk.CTkLabel(form, text="正文", text_color=COLOR_MUTED,
                      width=80, anchor="nw") \
            .grid(row=1, column=0, padx=(14, 4), pady=(10, 4), sticky="nw")
        self._body = ctk.CTkTextbox(form, height=100, fg_color=COLOR_PANEL_HI,
                                     text_color=COLOR_TEXT, border_width=0)
        self._body.grid(row=1, column=1, columnspan=2, sticky="ew",
                         padx=(4, 14), pady=10)

        ctk.CTkLabel(form, text="分类", text_color=COLOR_MUTED,
                      width=80, anchor="w") \
            .grid(row=2, column=0, padx=(14, 4), pady=10)
        self._cat = ctk.CTkOptionMenu(form, values=[name for name, _ in CATEGORIES],
                                        fg_color=COLOR_PANEL_HI,
                                        button_color=COLOR_PANEL_HI,
                                        button_hover_color=COLOR_ACCENT)
        self._cat.set("消息")
        self._cat.grid(row=2, column=1, sticky="w", padx=4, pady=10)
        self._send_btn = ctk.CTkButton(form, text="发 送",
                                        fg_color=COLOR_ACCENT,
                                        command=self._on_send, width=120)
        self._send_btn.grid(row=2, column=2, padx=(4, 14), pady=10, sticky="e")

        # 历史
        hist = Card(self)
        hist.grid(row=2, column=0, sticky="nsew", padx=20, pady=(8, 20))
        hist.grid_columnconfigure(0, weight=1)
        hist.grid_rowconfigure(1, weight=1)
        ctk.CTkLabel(hist, text="最近发送",
                      text_color=COLOR_MUTED, anchor="w") \
            .grid(row=0, column=0, sticky="w", padx=14, pady=(12, 4))
        self._hist = ctk.CTkTextbox(hist, fg_color="transparent",
                                      text_color=COLOR_TEXT, border_width=0)
        self._hist.grid(row=1, column=0, sticky="nsew", padx=14, pady=(0, 12))

        # status
        self._status = ctk.CTkLabel(self, text="",
                                     text_color=COLOR_MUTED, anchor="w")
        self._status.grid(row=1, column=0, sticky="ew", padx=20)

    def _on_send(self) -> None:
        title = self._title.get().strip()
        body  = self._body.get("1.0", "end").strip()
        if not title and not body:
            self._status.configure(text="标题或正文不能都为空", text_color=COLOR_ERR)
            return
        cat_name = self._cat.get()
        cat_value = next((v for n, v in CATEGORIES if n == cat_name),
                          NOTIFY_CAT_GENERIC)
        self._app.bus.emit_threadsafe("notify:manual", (title, body, cat_value))
        preview = (body or "").replace("\n", " ")[:40]
        self._hist.insert("1.0", f"[{cat_name}] {title}  {preview}\n")
        self._status.configure(text="已发送", text_color=COLOR_OK)
        self._title.delete(0, "end")
        self._body.delete("1.0", "end")
