"""Upload 页：app 包目录上传 + 进度条 + app 列表。

通过 bus.emit("upload:request", {"kind": ..., "args": ..., "future": fut}) 触发。
kind 取值：pack | list | delete。
"""

from __future__ import annotations

import os
from concurrent.futures import Future as ConcFuture
from tkinter import filedialog

import customtkinter as ctk

from ..theme import (
    COLOR_ACCENT, COLOR_ERR, COLOR_MUTED, COLOR_OK, COLOR_PANEL, COLOR_PANEL_HI,
    COLOR_TEXT, COLOR_WARN,
)
from ..widgets import Card


class UploadPage(ctk.CTkFrame):
    def __init__(self, master, app) -> None:
        super().__init__(master, fg_color="transparent")
        self._app = app
        self._folder_path: str | None = None
        self._app_id_var = ctk.StringVar()
        self._build()

        # 进度回调
        app.bus.on("upload:progress", lambda p: self.after(0, self._on_progress, p))
        app.bus.on("upload:step",     lambda p: self.after(0, self._on_step, p))
        app.bus.on("upload:end",      lambda _: self.after(0, self._on_end))
        app.bus.on("upload:begin",    lambda _: self.after(0, self._on_begin))

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(2, weight=1)

        # 选择行（仅目录）
        row = Card(self)
        row.grid(row=0, column=0, sticky="ew", padx=20, pady=(20, 8))
        row.grid_columnconfigure(1, weight=1)
        ctk.CTkLabel(row, text="App 包目录",
                      text_color=COLOR_MUTED, width=80, anchor="w") \
            .grid(row=0, column=0, padx=(14, 4), pady=10)
        self._sel_lbl = ctk.CTkLabel(row, text="(未选择)",
                                      text_color=COLOR_TEXT, anchor="w")
        self._sel_lbl.grid(row=0, column=1, sticky="ew", padx=4, pady=10)
        ctk.CTkButton(row, text="选目录", width=90,
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       command=self._pick_dir) \
            .grid(row=0, column=2, padx=(4, 14), pady=10)

        # app_id + 上传 / 列表 / 删除
        ctrl = Card(self)
        ctrl.grid(row=1, column=0, sticky="ew", padx=20, pady=8)
        ctrl.grid_columnconfigure(1, weight=1)
        ctk.CTkLabel(ctrl, text="App ID", text_color=COLOR_MUTED,
                      width=80, anchor="w") \
            .grid(row=0, column=0, padx=(14, 4), pady=10)
        self._aid = ctk.CTkEntry(ctrl, fg_color=COLOR_PANEL_HI,
                                  text_color=COLOR_TEXT, border_width=0,
                                  textvariable=self._app_id_var)
        self._aid.grid(row=0, column=1, sticky="ew", padx=4, pady=10)
        self._upload_btn = ctk.CTkButton(ctrl, text="上 传", width=90,
                                          fg_color=COLOR_ACCENT,
                                          command=self._do_upload)
        self._upload_btn.grid(row=0, column=2, padx=4, pady=10)
        self._list_btn = ctk.CTkButton(ctrl, text="刷新列表", width=90,
                                        fg_color=COLOR_PANEL_HI,
                                        command=self._do_list)
        self._list_btn.grid(row=0, column=3, padx=4, pady=10)
        self._del_btn = ctk.CTkButton(ctrl, text="删除", width=90,
                                       fg_color=COLOR_PANEL_HI,
                                       hover_color=COLOR_ERR,
                                       command=self._do_delete)
        self._del_btn.grid(row=0, column=4, padx=(4, 14), pady=10)

        # 进度区
        prog = Card(self)
        prog.grid(row=2, column=0, sticky="nsew", padx=20, pady=(8, 20))
        prog.grid_columnconfigure(0, weight=1)
        prog.grid_rowconfigure(2, weight=1)
        ctk.CTkLabel(prog, text="进度", text_color=COLOR_MUTED, anchor="w") \
            .grid(row=0, column=0, sticky="w", padx=14, pady=(12, 4))
        self._step_lbl = ctk.CTkLabel(prog, text="--",
                                       text_color=COLOR_TEXT, anchor="w")
        self._step_lbl.grid(row=1, column=0, sticky="ew", padx=14, pady=(0, 6))
        self._bar = ctk.CTkProgressBar(prog, progress_color=COLOR_ACCENT)
        self._bar.set(0.0)
        self._bar.grid(row=2, column=0, sticky="new", padx=14, pady=(0, 8))
        self._apps_lbl = ctk.CTkLabel(prog, text="apps: --",
                                       text_color=COLOR_MUTED, anchor="w",
                                       justify="left")
        self._apps_lbl.grid(row=3, column=0, sticky="ew", padx=14, pady=(0, 12))

    # actions

    def _pick_dir(self) -> None:
        p = filedialog.askdirectory()
        if not p:
            return
        self._folder_path = p
        self._sel_lbl.configure(text=p)
        if not self._app_id_var.get():
            self._app_id_var.set(os.path.basename(p.rstrip("/\\")))

    def _do_upload(self) -> None:
        app_id = self._app_id_var.get().strip()
        if not app_id:
            self._step_lbl.configure(text="App ID 必填", text_color=COLOR_ERR)
            return
        if not self._folder_path:
            self._step_lbl.configure(text="先选择 app 包目录", text_color=COLOR_ERR)
            return
        self._submit("pack", {"app_id": app_id, "pack_dir": self._folder_path})

    def _do_list(self) -> None:
        self._submit("list", {})

    def _do_delete(self) -> None:
        app_id = self._app_id_var.get().strip()
        if not app_id:
            self._step_lbl.configure(text="App ID 必填", text_color=COLOR_ERR)
            return
        self._submit("delete", {"app_id": app_id})

    def _submit(self, kind: str, args: dict) -> None:
        fut: ConcFuture = ConcFuture()
        self._step_lbl.configure(text=f"{kind}...", text_color=COLOR_WARN)
        self._bar.set(0.0)
        self._app.bus.emit_threadsafe("upload:request", {
            "kind": kind, "args": args, "future": fut,
        })
        fut.add_done_callback(lambda f: self.after(0, self._on_done, kind, f))

    def _on_done(self, kind: str, fut: ConcFuture) -> None:
        try:
            ret = fut.result()
        except Exception as e:
            self._step_lbl.configure(text=f"失败: {e}", text_color=COLOR_ERR)
            return
        if kind == "list":
            names = ret or []
            self._apps_lbl.configure(
                text="apps: " + (", ".join(names) if names else "(空)"))
            self._step_lbl.configure(text="刷新完成", text_color=COLOR_OK)
        else:
            self._step_lbl.configure(text=f"{kind} 完成", text_color=COLOR_OK)
            self._bar.set(1.0)

    def _on_progress(self, payload: object) -> None:
        try:
            sent, total = payload  # type: ignore[misc]
        except Exception:
            return
        if total > 0:
            self._bar.set(sent / total)

    def _on_step(self, payload: object) -> None:
        try:
            name, idx, total = payload  # type: ignore[misc]
        except Exception:
            return
        self._step_lbl.configure(text=f"[{idx}/{total}] {name}",
                                   text_color=COLOR_WARN)

    def _on_begin(self) -> None:
        self._upload_btn.configure(state="disabled")
        self._list_btn.configure(state="disabled")
        self._del_btn.configure(state="disabled")

    def _on_end(self) -> None:
        self._upload_btn.configure(state="normal")
        self._list_btn.configure(state="normal")
        self._del_btn.configure(state="normal")
