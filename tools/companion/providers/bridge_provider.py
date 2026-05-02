"""bridge_provider —— 动态 app JSON 通道（a3a30002 RX / a3a30003 TX）。

复用 dynapp_sdk.router.Router 做消息路由；BleakClient 由 core 注入。
对 a3a30003 收到的 utf-8 JSON 解析后按 (from_app, type) dispatch。
内置 weather/music handler，与原 providers/weather_provider.py / media_provider.py 等价。
"""

from __future__ import annotations

import asyncio
import json
import logging
import sys
import time as _time
from typing import Any, Optional

from ..constants import BRIDGE_MAX_PAYLOAD, BRIDGE_RX_UUID, BRIDGE_TX_UUID
from ..shared.geoip_weather import get_weather
from ..shared.smtc import MediaState, SmtcMonitor, send_media_key
from ..shared.win_notifications import WinNotificationMonitor
from .base import Provider, ProviderContext

logger = logging.getLogger(__name__)


# 用现有 router；不依赖 DynappClient
import sys as _sys
import os as _os
_TOOLS_DIR = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
if _TOOLS_DIR not in _sys.path:
    _sys.path.insert(0, _TOOLS_DIR)
from dynapp_sdk.router import Router  # type: ignore


class BridgeProvider(Provider):
    name = "bridge"

    def __init__(self) -> None:
        self._unsubs: list = []
        self._router = Router()
        self._ctx: Optional[ProviderContext] = None
        self._smtc: SmtcMonitor | None = None
        self._winnotif: WinNotificationMonitor | None = None

    def subscriptions(self) -> list[str]:
        return [BRIDGE_TX_UUID]

    async def on_start(self, ctx: ProviderContext) -> None:
        self._ctx = ctx
        # 注册内置 handler
        self._router.register("weather", "req",   self._on_weather_req)
        self._router.register("music",   "req",   self._on_music_req)
        self._router.register("music",   "btn",   self._on_music_btn)

        # bus 收到 a3a30003 notify → 解码 + dispatch
        def _on_tx(payload: object) -> None:
            data = payload if isinstance(payload, (bytes, bytearray)) else b""
            asyncio.create_task(self._on_recv(bytes(data)))
        self._unsubs.append(
            ctx.bus.on(f"notify:{BRIDGE_TX_UUID.lower()}", _on_tx))

        # SMTC 主动推送 music/state
        if sys.platform == "win32":
            loop = asyncio.get_running_loop()
            async def _on_change(state: MediaState) -> None:
                await self.send("music", "state", body=self._media_to_body(state))
            self._smtc = SmtcMonitor(_on_change, loop)
            try:
                await self._smtc.start()
            except Exception as e:
                ctx.bus.emit("log", ("warn", self.name, f"smtc: {e}"))

            # Windows 通知 → notif/add（增量推送，不拉历史）
            async def _on_notif(item: dict) -> None:
                ctx.bus.emit("log", ("info", self.name,
                    f"notif/add app={item.get('app')!r} title={item.get('title')!r}"))
                ok = await self.send("notif_pkg", "add", body={
                    "title": (item.get("title") or "")[:31],
                    "body":  (item.get("body")  or "")[:95],
                    "ts":    int(item.get("ts") or _time.time()),
                    "cat":   item.get("cat") or "msg",
                })
                if not ok:
                    ctx.bus.emit("log", ("warn", self.name,
                        "notif/add send failed (not connected?)"))
            self._winnotif = WinNotificationMonitor(_on_notif, loop)
            try:
                await self._winnotif.start()
            except Exception as e:
                ctx.bus.emit("log", ("warn", self.name, f"winnotif: {e}"))

    async def on_stop(self, ctx: ProviderContext) -> None:
        for u in self._unsubs:
            try: u()
            except Exception: pass
        self._unsubs.clear()
        if self._smtc is not None:
            try: await self._smtc.stop()
            except Exception: pass
            self._smtc = None
        if self._winnotif is not None:
            try: await self._winnotif.stop()
            except Exception: pass
            self._winnotif = None
        self._ctx = None

    # ------------------------------------------------------------------
    # send
    # ------------------------------------------------------------------

    async def send(self, to_app: str, msg_type: str, body: Any = None) -> bool:
        if self._ctx is None or not self._ctx.is_connected():
            return False
        msg = {"to": to_app, "type": msg_type}
        if body is not None:
            msg["body"] = body
        payload = json.dumps(msg, ensure_ascii=False).encode("utf-8")
        if len(payload) > BRIDGE_MAX_PAYLOAD:
            self._ctx.bus.emit("log", ("warn", self.name,
                f"payload {len(payload)}B > {BRIDGE_MAX_PAYLOAD}, dropped"))
            return False
        try:
            await self._ctx.write(BRIDGE_RX_UUID, payload, response=False)
            return True
        except Exception as e:
            self._ctx.bus.emit("log", ("warn", self.name, f"send: {e}"))
            return False

    async def _on_recv(self, data: bytes) -> None:
        try:
            text = data.decode("utf-8")
            msg = json.loads(text)
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(msg, dict):
            return
        await self._router.dispatch(msg)

    # ------------------------------------------------------------------
    # handlers
    # ------------------------------------------------------------------

    async def _on_weather_req(self, msg: dict) -> None:
        body = msg.get("body") if isinstance(msg.get("body"), dict) else {}
        force = bool(body.get("force"))
        try:
            snap = await get_weather(force=force)
            await self.send("weather", "data", body={
                "temp_c":   round(snap.temp_c, 1),
                "temp_min": round(snap.temp_min, 1),
                "temp_max": round(snap.temp_max, 1),
                "humidity": int(snap.humidity),
                "code":     self._wmo_to_str(snap.wmo),
                "city":     snap.city,
                "desc":     snap.desc(),
                "ts":       int(_time.time()),
            })
        except Exception as e:
            await self.send("weather", "error", body={"msg": str(e)})

    async def _on_music_req(self, _msg: dict) -> None:
        if self._smtc is None:
            await self.send("music", "no_session")
            return
        state = await self._smtc.fetch_state()
        if state.title == "" and state.artist == "":
            await self.send("music", "no_session")
            return
        await self.send("music", "state", body=self._media_to_body(state))

    async def _on_music_btn(self, msg: dict) -> None:
        body = msg.get("body") if isinstance(msg.get("body"), dict) else {}
        action = body.get("id") or body.get("action") or ""
        try:
            send_media_key(action)
            if self._ctx:
                self._ctx.bus.emit("log", ("info", self.name, f"music btn {action}"))
        except Exception as e:
            if self._ctx:
                self._ctx.bus.emit("log", ("warn", self.name, f"key: {e}"))

    # ------------------------------------------------------------------
    # utils
    # ------------------------------------------------------------------

    @staticmethod
    def _media_to_body(state: MediaState) -> dict:
        return {
            "playing":  bool(state.playing),
            "position": max(0, state.position_sec),
            "duration": max(0, state.duration_sec),
            "title":    state.title[:48],
            "artist":   state.artist[:32],
            "ts":       int(_time.time()),
        }

    @staticmethod
    def _wmo_to_str(wmo: int) -> str:
        if wmo == 0: return "clear"
        if wmo in (1, 2): return "cloudy"
        if wmo == 3: return "overcast"
        if wmo in (45, 48): return "fog"
        if wmo in (51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82): return "rain"
        if wmo in (71, 73, 75, 77, 85, 86): return "snow"
        if wmo in (95, 96, 99): return "thunder"
        return "unknown"
