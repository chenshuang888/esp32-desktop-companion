"""Windows 系统通知监听（UserNotificationListener）。

对外提供：
  WinNotificationMonitor  异步监听 Windows 通知中心，新通知到达时回调 on_change(item)

设计：
  - 启动时拉一次现有未读快照，把 id 全塞进 _seen_ids 当基线（不推），
    之后增量收到 NotificationChanged(Added) 时再推。
  - 权限拒绝 / winsdk 缺失 / 非 Windows  → 静默降级，warn log，monitor 仍可
    create / start / stop（只是不会回调）。
  - COM 事件来自 worker 线程，统一用 run_coroutine_threadsafe 桥到 asyncio loop。

通知项 schema（回调收到的 dict）::
    {
        "id":    int,        # NotificationId（用作去重 key）
        "app":   str,        # AppDisplayName，例如 "微信" / "Outlook"
        "title": str,        # ToastGeneric 第 1 行
        "body":  str,        # ToastGeneric 第 2 行起合并
        "ts":    int,        # 通知到达 unix 秒
        "cat":   str,        # 推断的 cat（msg/mail/call/cal/social/news/alert）
    }
"""

from __future__ import annotations

import asyncio
import logging
import sys
import time as _time
from typing import Awaitable, Callable, Optional

logger = logging.getLogger(__name__)


# 应用名 → cat 关键字表（小写匹配子串）
_CAT_RULES = [
    ("call",   ("phone", "call", "电话", "通话", "skype", "teams call")),
    ("mail",   ("mail", "outlook", "thunderbird", "邮件", "foxmail")),
    ("cal",    ("calendar", "日历", "schedule")),
    ("social", ("twitter", "x.com", "facebook", "instagram", "微博", "知乎",
                "tiktok", "douyin", "小红书", "linkedin")),
    ("news",   ("news", "新闻")),
    ("alert",  ("alert", "warning", "security", "警", "防火墙", "defender")),
    # msg 兜底（微信/QQ/钉钉/Telegram/Discord/...）
    ("msg",    ("wechat", "微信", "qq", "钉钉", "dingtalk", "telegram",
                "discord", "slack", "messenger", "whatsapp", "lark", "飞书")),
]


def _infer_cat(app_name: str) -> str:
    s = (app_name or "").lower()
    for cat, kws in _CAT_RULES:
        for kw in kws:
            if kw in s:
                return cat
    return "msg"


class WinNotificationMonitor:
    """订阅 UserNotificationListener，新通知到达时回调 on_change(item)。"""

    def __init__(self,
                 on_change: Callable[[dict], Awaitable[None]],
                 loop: asyncio.AbstractEventLoop,
                 poll_interval: float = 2.0) -> None:
        self._on_change = on_change
        self._loop = loop
        self._poll_interval = poll_interval
        self._listener = None
        self._kinds = None
        self._seen_ids: set[int] = set()
        self._available = sys.platform == "win32"
        self._poll_task: Optional[asyncio.Task] = None
        self._stopped = asyncio.Event()

    async def start(self) -> None:
        if not self._available:
            logger.warning("win_notifications unavailable: not windows")
            return

        try:
            from winsdk.windows.ui.notifications.management import (
                UserNotificationListener,
                UserNotificationListenerAccessStatus,
            )
            from winsdk.windows.ui.notifications import NotificationKinds
        except ImportError as e:
            logger.warning("winsdk import failed; win_notifications disabled: %s", e)
            self._available = False
            return

        listener = UserNotificationListener.current
        try:
            status = await listener.request_access_async()
        except Exception as e:
            logger.warning("request_access failed; win_notifications disabled: %s", e)
            self._available = False
            return

        if status != UserNotificationListenerAccessStatus.ALLOWED:
            logger.warning(
                "win notification access not granted (status=%s); "
                "open Windows 设置 → 隐私 → 通知 授权 Python 即可", status)
            self._available = False
            return

        self._listener = listener
        self._kinds = NotificationKinds.TOAST

        # 用现有未读做基线 —— 全部记入 seen，不回调
        try:
            existing = await listener.get_notifications_async(self._kinds)
            for n in existing:
                try:
                    self._seen_ids.add(int(n.id))
                except Exception:
                    pass
            logger.info("win_notifications baseline: %d existing", len(self._seen_ids))
        except Exception as e:
            logger.warning("baseline fetch failed: %s", e)

        # NotificationChanged 事件订阅在非 UWP 进程里会返回 HRESULT 0x80070490
        # ("找不到元素")。改用定时轮询 get_notifications_async + diff seen_ids。
        self._stopped.clear()
        self._poll_task = asyncio.create_task(self._poll_loop())

    async def stop(self) -> None:
        self._stopped.set()
        if self._poll_task is not None:
            try:
                await asyncio.wait_for(self._poll_task, timeout=2.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                self._poll_task.cancel()
            except Exception:
                pass
            self._poll_task = None
        self._listener = None
        self._seen_ids.clear()

    # ------------------------------------------------------------------
    # internal
    # ------------------------------------------------------------------

    async def _poll_loop(self) -> None:
        while not self._stopped.is_set():
            try:
                await asyncio.wait_for(
                    self._stopped.wait(), timeout=self._poll_interval)
                return  # stop
            except asyncio.TimeoutError:
                pass
            try:
                await self._sweep()
            except Exception:
                logger.exception("win_notifications sweep crashed")

    async def _sweep(self) -> None:
        if self._listener is None or self._kinds is None:
            return
        try:
            current = await self._listener.get_notifications_async(self._kinds)
        except Exception:
            logger.exception("get_notifications failed")
            return

        current_ids: set[int] = set()
        new_items: list[dict] = []
        for n in current:
            try:
                nid = int(n.id)
            except Exception:
                continue
            current_ids.add(nid)
            if nid in self._seen_ids:
                continue
            item = self._extract(n)
            if item is not None:
                new_items.append(item)
            else:
                logger.debug("win_notif: id=%s extract returned None (skipped)", nid)

        added = current_ids - self._seen_ids
        removed = self._seen_ids - current_ids
        if added or removed:
            logger.info("win_notif sweep: +%d -%d total=%d push=%d",
                        len(added), len(removed), len(current_ids), len(new_items))

        # 同步基线：移除已被用户清掉的；加入新的
        self._seen_ids = current_ids

        for it in new_items:
            try:
                await self._on_change(it)
            except Exception:
                logger.exception("on_change crashed")

    def _extract(self, notification) -> Optional[dict]:
        try:
            nid = int(notification.id)
        except Exception:
            nid = 0

        app_name = ""
        try:
            app_info = notification.app_info
            if app_info is not None:
                disp = getattr(app_info, "display_info", None)
                if disp is not None:
                    app_name = getattr(disp, "display_name", "") or ""
        except Exception:
            pass

        title = ""
        body = ""
        try:
            visual = notification.notification.visual
        except Exception:
            visual = None
        if visual is not None:
            binding = None
            try:
                binding = visual.get_binding("ToastGeneric")
            except Exception:
                binding = None
            if binding is None:
                try:
                    for b in visual.bindings:
                        binding = b
                        break
                except Exception:
                    binding = None
            if binding is not None:
                try:
                    texts = list(binding.get_text_elements())
                    lines: list[str] = []
                    for t in texts:
                        s = getattr(t, "text", "") or ""
                        if s:
                            lines.append(s)
                    if lines:
                        title = lines[0]
                        body = "\n".join(lines[1:]) if len(lines) > 1 else ""
                except Exception:
                    pass

        if not title and not body:
            return None

        ts = int(_time.time())
        try:
            ct = notification.creation_time
            if ct is not None:
                ts = int(ct.timestamp())
        except Exception:
            pass

        cat = _infer_cat(app_name or title)

        return {
            "id":    nid,
            "app":   app_name,
            "title": title or app_name or "(通知)",
            "body":  body,
            "ts":    ts,
            "cat":   cat,
        }
