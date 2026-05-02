"""Music 页：本地音乐文件夹管理 + 一键同步到手表。

功能：
  - 显示当前 music_folder 路径，可改
  - 添加文件（弹文件选择器，支持多选 mp3/flac/wav/ogg/m4a）
  - 在线下载：从 archive.org（CC0/公共领域）搜索并下载免费音乐
  - 列表展示当前文件夹的音乐文件，每行带"删除"
  - "推送同步"按钮 → emit("media:rescan", {future}) 让 MediaProvider 重新扫描+流式推送
  - 文件夹不存在时自动创建
"""

from __future__ import annotations

import os
import shutil
import threading
from concurrent.futures import Future as ConcFuture
from pathlib import Path
from tkinter import filedialog, messagebox

import customtkinter as ctk

from ..theme import (
    COLOR_ACCENT, COLOR_ERR, COLOR_MUTED, COLOR_OK, COLOR_PANEL, COLOR_PANEL_HI,
    COLOR_TEXT, COLOR_WARN,
)
from ..widgets import Card
from ...shared import archive_org

SUPPORTED_EXTS = {".mp3", ".flac", ".wav", ".ogg", ".m4a"}
MAX_TRACKS = 50


class MusicPage(ctk.CTkFrame):
    def __init__(self, master, app) -> None:
        super().__init__(master, fg_color="transparent")
        self._app = app
        self._cfg = app.cfg
        self._row_widgets: list[ctk.CTkFrame] = []
        self._build()
        self._refresh_list()

    # ------------------------------------------------------------------
    # build
    # ------------------------------------------------------------------

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(2, weight=1)

        # 路径行
        path_card = Card(self)
        path_card.grid(row=0, column=0, sticky="ew", padx=20, pady=(20, 8))
        path_card.grid_columnconfigure(1, weight=1)
        ctk.CTkLabel(path_card, text="文件夹",
                      text_color=COLOR_MUTED, width=80, anchor="w") \
            .grid(row=0, column=0, padx=(14, 4), pady=10)
        self._path_lbl = ctk.CTkLabel(path_card, text=self._folder_str(),
                                        text_color=COLOR_TEXT, anchor="w")
        self._path_lbl.grid(row=0, column=1, sticky="ew", padx=4, pady=10)
        ctk.CTkButton(path_card, text="选目录", width=90,
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       command=self._pick_folder) \
            .grid(row=0, column=2, padx=4, pady=10)
        ctk.CTkButton(path_card, text="打开", width=70,
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       command=self._open_folder) \
            .grid(row=0, column=3, padx=(4, 14), pady=10)

        # 操作栏
        op = Card(self)
        op.grid(row=1, column=0, sticky="ew", padx=20, pady=8)
        op.grid_columnconfigure(0, weight=1)
        self._info_lbl = ctk.CTkLabel(op, text="", text_color=COLOR_MUTED, anchor="w")
        self._info_lbl.grid(row=0, column=0, sticky="ew", padx=14, pady=10)
        ctk.CTkButton(op, text="添加音乐", width=100,
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       command=self._add_files) \
            .grid(row=0, column=1, padx=4, pady=10)
        ctk.CTkButton(op, text="在线下载", width=100,
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       command=self._open_download_dialog) \
            .grid(row=0, column=2, padx=4, pady=10)
        ctk.CTkButton(op, text="刷新", width=80,
                       fg_color=COLOR_PANEL_HI, hover_color=COLOR_ACCENT,
                       command=self._refresh_list) \
            .grid(row=0, column=3, padx=4, pady=10)
        self._sync_btn = ctk.CTkButton(op, text="推送同步", width=100,
                                        fg_color=COLOR_ACCENT,
                                        command=self._sync_to_watch)
        self._sync_btn.grid(row=0, column=4, padx=(4, 14), pady=10)

        # 列表（可滚动）
        list_wrap = Card(self)
        list_wrap.grid(row=2, column=0, sticky="nsew", padx=20, pady=(8, 20))
        list_wrap.grid_columnconfigure(0, weight=1)
        list_wrap.grid_rowconfigure(0, weight=1)

        self._scroll = ctk.CTkScrollableFrame(list_wrap, fg_color="transparent")
        self._scroll.grid(row=0, column=0, sticky="nsew", padx=8, pady=8)
        self._scroll.grid_columnconfigure(0, weight=1)

    # ------------------------------------------------------------------
    # folder helpers
    # ------------------------------------------------------------------

    def _folder_path(self) -> Path:
        p = self._cfg.get("music_folder")
        if not p:
            p = str(Path(os.path.expanduser("~")) / "Music" / "Watch")
            self._cfg["music_folder"] = p
        return Path(p)

    def _folder_str(self) -> str:
        return str(self._folder_path())

    def _ensure_folder(self) -> Path:
        p = self._folder_path()
        p.mkdir(parents=True, exist_ok=True)
        return p

    def _scan(self) -> list[Path]:
        try:
            p = self._ensure_folder()
        except Exception:
            return []
        return sorted([f for f in p.iterdir()
                        if f.is_file() and f.suffix.lower() in SUPPORTED_EXTS],
                       key=lambda x: x.name.lower())

    # ------------------------------------------------------------------
    # actions
    # ------------------------------------------------------------------

    def _pick_folder(self) -> None:
        p = filedialog.askdirectory(initialdir=self._folder_str(),
                                      title="选择音乐文件夹")
        if not p:
            return
        self._cfg["music_folder"] = p
        self._path_lbl.configure(text=p)
        self._refresh_list()

    def _open_folder(self) -> None:
        try:
            self._ensure_folder()
            os.startfile(self._folder_str())
        except Exception as e:
            self._info_lbl.configure(text=f"打开失败: {e}", text_color=COLOR_ERR)

    def _add_files(self) -> None:
        paths = filedialog.askopenfilenames(
            title="选择音乐文件",
            filetypes=[("Audio", "*.mp3 *.flac *.wav *.ogg *.m4a"), ("All", "*.*")])
        if not paths:
            return
        dst = self._ensure_folder()
        ok, skipped = 0, 0
        for src_str in paths:
            src = Path(src_str)
            if src.suffix.lower() not in SUPPORTED_EXTS:
                skipped += 1
                continue
            try:
                target = dst / src.name
                if target.exists():
                    skipped += 1
                    continue
                shutil.copy2(src, target)
                ok += 1
            except Exception as e:
                self._info_lbl.configure(text=f"复制失败: {e}", text_color=COLOR_ERR)
                skipped += 1
        self._info_lbl.configure(
            text=f"已添加 {ok} 首" + (f"，跳过 {skipped}" if skipped else ""),
            text_color=COLOR_OK if ok else COLOR_WARN)
        self._refresh_list()

    def _delete_file(self, path: Path) -> None:
        if not messagebox.askyesno("确认删除",
                                     f"删除文件？\n{path.name}\n（此操作不可恢复）"):
            return
        try:
            path.unlink()
            self._info_lbl.configure(text=f"已删除 {path.name}", text_color=COLOR_OK)
        except Exception as e:
            self._info_lbl.configure(text=f"删除失败: {e}", text_color=COLOR_ERR)
        self._refresh_list()

    def _sync_to_watch(self) -> None:
        files = self._scan()
        if not files:
            self._info_lbl.configure(text="文件夹是空的，先添加音乐",
                                       text_color=COLOR_WARN)
            return
        self._sync_btn.configure(state="disabled", text="推送中…")
        fut: ConcFuture = ConcFuture()
        # 通知 provider 更新 music_folder（如果改过路径），然后让它重扫并推送
        # provider 拿不到最新 cfg，所以这里用 emit("media:set_folder", path) 更新 + rescan
        self._app.bus.emit_threadsafe("media:set_folder", str(self._folder_path()))
        self._app.bus.emit_threadsafe("media:rescan", {"future": fut})
        fut.add_done_callback(lambda f: self.after(0, self._on_sync_done, f))

    def _on_sync_done(self, fut: ConcFuture) -> None:
        self._sync_btn.configure(state="normal", text="推送同步")
        try:
            n = fut.result()
            self._info_lbl.configure(
                text=f"已推送 {n} 首到手表" if n else "未推送（手表未连接？）",
                text_color=COLOR_OK if n else COLOR_WARN)
        except Exception as e:
            self._info_lbl.configure(text=f"推送失败: {e}", text_color=COLOR_ERR)

    # ------------------------------------------------------------------
    # list rendering
    # ------------------------------------------------------------------

    def _refresh_list(self) -> None:
        for w in self._row_widgets:
            w.destroy()
        self._row_widgets.clear()

        files = self._scan()
        n = len(files)
        over = max(0, n - MAX_TRACKS)
        if n == 0:
            tip = "文件夹是空的，点「添加音乐」选 mp3/flac/wav/ogg/m4a"
            self._info_lbl.configure(text=tip, text_color=COLOR_MUTED)
        else:
            txt = f"共 {n} 首"
            if over:
                txt += f"（手表上限 {MAX_TRACKS}，超出 {over} 首会被截断）"
            self._info_lbl.configure(text=txt, text_color=COLOR_MUTED)

        for i, p in enumerate(files):
            self._make_row(i, p, over_limit=(i >= MAX_TRACKS))

    def _make_row(self, idx: int, path: Path, over_limit: bool) -> None:
        row = ctk.CTkFrame(self._scroll, fg_color=COLOR_PANEL_HI, corner_radius=8)
        row.grid(row=idx, column=0, sticky="ew", padx=4, pady=2)
        row.grid_columnconfigure(2, weight=1)
        self._row_widgets.append(row)

        ctk.CTkLabel(row, text=str(idx + 1), width=28,
                      text_color=COLOR_MUTED if not over_limit else COLOR_WARN,
                      anchor="e") \
            .grid(row=0, column=0, padx=(8, 4), pady=8)

        # 文件名解析 → title / artist
        stem = path.stem
        if " - " in stem:
            artist, _, title = stem.partition(" - ")
            artist, title = artist.strip(), title.strip()
            disp = f"{title}"
            sub  = f"{artist}"
        else:
            disp = stem
            sub  = path.suffix.upper().lstrip(".")

        title_lbl = ctk.CTkLabel(row, text=disp, anchor="w",
                                  text_color=COLOR_TEXT if not over_limit else COLOR_WARN)
        title_lbl.grid(row=0, column=1, sticky="w", padx=(4, 8), pady=(8, 0))
        sub_lbl = ctk.CTkLabel(row, text=sub, anchor="w",
                                text_color=COLOR_MUTED,
                                font=ctk.CTkFont(size=11))
        sub_lbl.grid(row=1, column=1, sticky="w", padx=(4, 8), pady=(0, 8))

        ctk.CTkButton(row, text="删除", width=60, height=28,
                       fg_color=COLOR_PANEL, hover_color=COLOR_ERR,
                       text_color=COLOR_TEXT,
                       command=lambda p=path: self._delete_file(p)) \
            .grid(row=0, column=3, rowspan=2, padx=(4, 8), pady=4)

    # ------------------------------------------------------------------
    # 在线下载对话框
    # ------------------------------------------------------------------

    def _open_download_dialog(self) -> None:
        try:
            self._ensure_folder()
        except Exception as e:
            self._info_lbl.configure(text=f"目录创建失败: {e}", text_color=COLOR_ERR)
            return
        _DownloadDialog(self, self._folder_path(),
                         on_done=lambda count: self._on_download_done(count))

    def _on_download_done(self, count: int) -> None:
        if count > 0:
            self._info_lbl.configure(text=f"已下载 {count} 首", text_color=COLOR_OK)
        self._refresh_list()


# ============================================================================
# 在线下载对话框
# ============================================================================

class _DownloadDialog(ctk.CTkToplevel):
    """archive.org 搜索 + 下载弹窗。

    线程模型：搜索 / 下载用 threading 跑后台，结果通过 self.after() 回主线程。
    """

    def __init__(self, master, dest_folder: Path,
                  on_done) -> None:
        super().__init__(master)
        self.title("在线下载（archive.org · 免费音乐）")
        self.geometry("640x480")
        self.transient(master)
        self.grab_set()
        self._dest = dest_folder
        self._on_done = on_done
        self._downloaded = 0
        self._row_widgets: list[ctk.CTkFrame] = []
        self._build()

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(2, weight=1)

        # 搜索栏
        bar = ctk.CTkFrame(self, fg_color="transparent")
        bar.grid(row=0, column=0, sticky="ew", padx=14, pady=(14, 6))
        bar.grid_columnconfigure(0, weight=1)
        self._kw = ctk.CTkEntry(bar, placeholder_text="关键词（如 piano / jazz / ambient / 钢琴）")
        self._kw.grid(row=0, column=0, sticky="ew", padx=(0, 6))
        self._kw.bind("<Return>", lambda e: self._do_search())
        self._search_btn = ctk.CTkButton(bar, text="搜索", width=80,
                                           fg_color=COLOR_ACCENT,
                                           command=self._do_search)
        self._search_btn.grid(row=0, column=1)
        self._pick_btn = ctk.CTkButton(bar, text="一键推荐 20 首", width=140,
                                         fg_color=COLOR_PANEL_HI,
                                         hover_color=COLOR_ACCENT,
                                         command=self._do_recommend)
        self._pick_btn.grid(row=0, column=2, padx=(6, 0))

        # 状态行
        self._status = ctk.CTkLabel(self, text="提示：archive.org 上的音乐均为公共领域 / CC 协议，可放心下载",
                                      text_color=COLOR_MUTED, anchor="w")
        self._status.grid(row=1, column=0, sticky="ew", padx=14, pady=(0, 6))

        # 结果区
        self._scroll = ctk.CTkScrollableFrame(self, fg_color=COLOR_PANEL)
        self._scroll.grid(row=2, column=0, sticky="nsew", padx=14, pady=(0, 6))
        self._scroll.grid_columnconfigure(0, weight=1)

        # 关闭
        bot = ctk.CTkFrame(self, fg_color="transparent")
        bot.grid(row=3, column=0, sticky="ew", padx=14, pady=(0, 14))
        bot.grid_columnconfigure(0, weight=1)
        ctk.CTkButton(bot, text="完成", width=80, fg_color=COLOR_PANEL_HI,
                       hover_color=COLOR_ACCENT,
                       command=self._close) \
            .grid(row=0, column=1)

    # -- search ----------------------------------------------------------

    def _do_search(self) -> None:
        kw = self._kw.get().strip()
        if not kw:
            self._status.configure(text="请输入关键词", text_color=COLOR_WARN)
            return
        self._search_btn.configure(state="disabled", text="搜索中…")
        self._status.configure(text=f"搜索 '{kw}' …", text_color=COLOR_MUTED)
        self._clear_rows()
        threading.Thread(target=self._search_worker, args=(kw,), daemon=True).start()

    # -- 一键推荐 --------------------------------------------------------

    def _do_recommend(self) -> None:
        plan = "、".join(f"{n} 首{label}" for _, n, label in archive_org.DEFAULT_PICKS)
        if not messagebox.askyesno(
            "一键推荐 20 首",
            f"将从 archive.org（CC0 / 公共领域）下载约 20 首：\n"
            f"  {plan}\n\n"
            f"目标文件夹：{self._dest}\n"
            f"预计总大小：约 50-100 MB\n\n"
            f"开始下载？",
            parent=self,
        ):
            return
        self._search_btn.configure(state="disabled")
        self._pick_btn.configure(state="disabled", text="搜索中…")
        self._clear_rows()
        self._status.configure(text="正在搜索推荐曲目（每类一搜，约 30 秒）…",
                                 text_color=COLOR_MUTED)
        threading.Thread(target=self._recommend_worker, daemon=True).start()

    def _recommend_worker(self) -> None:
        def progress(category: str, picked: int, total: int) -> None:
            self.after(0, lambda c=category, t=total:
                self._pick_btn.configure(text=f"找到 {t} 首…"))
        try:
            hits = archive_org.collect_recommendations(progress_cb=progress)
        except Exception as e:
            self.after(0, lambda: self._on_recommend_failed(str(e)))
            return
        if not hits:
            self.after(0, lambda: self._on_recommend_failed("未找到合适的曲目"))
            return
        # 全部串行下载
        self.after(0, lambda h=hits: self._on_recommend_search_done(h))

    def _on_recommend_failed(self, msg: str) -> None:
        self._search_btn.configure(state="normal")
        self._pick_btn.configure(state="normal", text="一键推荐 20 首")
        self._status.configure(text=f"推荐失败: {msg}", text_color=COLOR_ERR)

    def _on_recommend_search_done(self, hits: list) -> None:
        # 先把所有曲目展示在列表里（带进度条按钮），然后启一条线程串行下载
        self._status.configure(text=f"已找到 {len(hits)} 首，开始下载…",
                                 text_color=COLOR_OK)
        self._pick_btn.configure(text=f"下载 0/{len(hits)}")
        for i, h in enumerate(hits):
            self._make_row(i, h)
        threading.Thread(target=self._recommend_download_worker,
                          args=(hits,), daemon=True).start()

    def _recommend_download_worker(self, hits: list) -> None:
        total = len(hits)
        ok = 0
        for i, h in enumerate(hits):
            row = self._row_widgets[i] if i < len(self._row_widgets) else None
            btn = None
            if row is not None:
                # row 的最后一个孩子是按钮
                children = row.winfo_children()
                if children: btn = children[-1]
            def progress(done: int, t: int, b=btn) -> None:
                if t > 0 and b is not None:
                    pct = int(100 * done / t)
                    self.after(0, lambda: b.configure(text=f"{pct}%"))
            try:
                path = archive_org.download(h, self._dest, progress_cb=progress)
            except Exception:
                path = None
            if path:
                ok += 1
                self._downloaded += 1
                if btn is not None:
                    self.after(0, lambda b=btn: b.configure(
                        text="已下", fg_color=COLOR_OK, state="disabled"))
            else:
                if btn is not None:
                    self.after(0, lambda b=btn: b.configure(
                        text="失败", fg_color=COLOR_ERR, state="disabled"))
            self.after(0, lambda i=i, t=total:
                self._pick_btn.configure(text=f"下载 {i + 1}/{t}"))
        self.after(0, lambda: self._on_recommend_done(ok, total))

    def _on_recommend_done(self, ok: int, total: int) -> None:
        self._search_btn.configure(state="normal")
        self._pick_btn.configure(state="normal", text="一键推荐 20 首")
        self._status.configure(
            text=f"完成：成功 {ok} / 共 {total} 首",
            text_color=COLOR_OK if ok else COLOR_ERR)

    def _search_worker(self, kw: str) -> None:
        try:
            hits = archive_org.search(kw, limit=15)
        except Exception as e:
            self.after(0, lambda: self._on_search_failed(str(e)))
            return
        # 二轮：每个 hit 取 mp3 直链（resolve），无 mp3 的丢弃
        resolved: list[archive_org.TrackHit] = []
        for h in hits:
            r = archive_org.resolve_mp3(h)
            if r and r.file_name:
                resolved.append(r)
        self.after(0, lambda: self._on_search_done(resolved))

    def _on_search_failed(self, msg: str) -> None:
        self._search_btn.configure(state="normal", text="搜索")
        self._status.configure(text=f"搜索失败: {msg}", text_color=COLOR_ERR)

    def _on_search_done(self, hits: list[archive_org.TrackHit]) -> None:
        self._search_btn.configure(state="normal", text="搜索")
        if not hits:
            self._status.configure(text="未找到结果，换个关键词试试",
                                     text_color=COLOR_WARN)
            return
        self._status.configure(text=f"找到 {len(hits)} 个结果",
                                 text_color=COLOR_OK)
        for i, h in enumerate(hits):
            self._make_row(i, h)

    # -- row & download --------------------------------------------------

    def _clear_rows(self) -> None:
        for w in self._row_widgets:
            try: w.destroy()
            except Exception: pass
        self._row_widgets.clear()

    def _make_row(self, idx: int, hit: archive_org.TrackHit) -> None:
        row = ctk.CTkFrame(self._scroll, fg_color=COLOR_PANEL_HI, corner_radius=8)
        row.grid(row=idx, column=0, sticky="ew", padx=4, pady=2)
        row.grid_columnconfigure(0, weight=1)
        self._row_widgets.append(row)

        title_lbl = ctk.CTkLabel(row, text=hit.display_title, anchor="w",
                                  text_color=COLOR_TEXT,
                                  wraplength=420, justify="left")
        title_lbl.grid(row=0, column=0, sticky="w", padx=10, pady=(8, 0))
        size_mb = hit.file_size / (1024 * 1024) if hit.file_size else 0
        sub_text = hit.display_artist
        if size_mb:
            sub_text += f"  ·  {size_mb:.1f} MB"
        ctk.CTkLabel(row, text=sub_text, anchor="w",
                      text_color=COLOR_MUTED,
                      font=ctk.CTkFont(size=11)) \
            .grid(row=1, column=0, sticky="w", padx=10, pady=(0, 8))

        btn = ctk.CTkButton(row, text="下载", width=70, height=28,
                              fg_color=COLOR_ACCENT)
        btn.grid(row=0, column=1, rowspan=2, padx=(4, 10), pady=4)
        btn.configure(command=lambda h=hit, b=btn: self._do_download(h, b))

    def _do_download(self, hit: archive_org.TrackHit, btn: ctk.CTkButton) -> None:
        btn.configure(state="disabled", text="0%")
        threading.Thread(target=self._download_worker,
                          args=(hit, btn), daemon=True).start()

    def _download_worker(self, hit: archive_org.TrackHit,
                          btn: ctk.CTkButton) -> None:
        def progress(done: int, total: int) -> None:
            if total <= 0: return
            pct = int(100 * done / total)
            self.after(0, lambda: btn.configure(text=f"{pct}%"))
        try:
            path = archive_org.download(hit, self._dest, progress_cb=progress)
        except Exception as e:
            self.after(0, lambda: btn.configure(text="失败", fg_color=COLOR_ERR))
            return
        if path is None:
            self.after(0, lambda: btn.configure(text="失败", fg_color=COLOR_ERR))
            return
        self._downloaded += 1
        self.after(0, lambda: btn.configure(text="已下", fg_color=COLOR_OK,
                                              state="disabled"))

    def _close(self) -> None:
        self.grab_release()
        try:
            self._on_done(self._downloaded)
        except Exception:
            pass
        self.destroy()
