"""python -m companion 入口。"""

from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import sys
import time

from . import config as cfg
from .bus import EventBus
from .core import Companion
from .providers.bridge_provider import BridgeProvider
from .providers.media_provider import MediaProvider
from .providers.notify_provider import NotifyProvider
from .providers.system_provider import SystemProvider
from .providers.time_provider import TimeProvider
from .providers.upload_provider import UploadProvider
from .providers.weather_provider import WeatherProvider
from .runner import AsyncRunner


def _setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)5s %(name)s | %(message)s",
        datefmt="%H:%M:%S",
    )


def _build_companion(bus: EventBus, cfg_data: dict) -> Companion:
    comp = Companion(
        device_name=cfg_data.get("device_name", "ESP32-S3-DEMO"),
        bus=bus,
        device_address=cfg_data.get("device_address"),
    )
    enabled = cfg_data.get("providers", {})
    if enabled.get("time", True):    comp.register(TimeProvider())
    if enabled.get("weather", True): comp.register(WeatherProvider())
    if enabled.get("notify", True):  comp.register(NotifyProvider())
    if enabled.get("system", True):  comp.register(SystemProvider())
    if enabled.get("media", True):
        comp.register(MediaProvider(music_folder=cfg_data.get("music_folder")))
    if enabled.get("bridge", True):  comp.register(BridgeProvider())
    if enabled.get("upload", True):  comp.register(UploadProvider())
    return comp


def _run_no_gui(bus: EventBus, runner: AsyncRunner, cfg_data: dict) -> int:
    comp = _build_companion(bus, cfg_data)

    # 把日志事件打到 stdout
    def _print_log(payload: object) -> None:
        try:
            level, name, msg = payload  # type: ignore[misc]
        except Exception:
            return
        print(f"{time.strftime('%H:%M:%S')} [{level:4}] {name:7} {msg}",
              flush=True)
    bus.on("log", _print_log)

    fut = runner.submit(comp.run())
    print("companion: running (Ctrl+C to quit)", flush=True)

    stop = False
    def _sigint(*_):
        nonlocal stop
        stop = True
    try:
        signal.signal(signal.SIGINT, _sigint)
    except Exception:
        pass

    try:
        while not stop:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    print("\ncompanion: shutting down...", flush=True)
    comp.stop()
    try:
        fut.result(timeout=8.0)
    except Exception:
        pass
    runner.stop()
    return 0


def _run_gui(bus: EventBus, runner: AsyncRunner, cfg_data: dict) -> int:
    try:
        from .gui.app import CompanionApp
    except ImportError as e:
        print(f"GUI 依赖缺失: {e}\n请 pip install customtkinter", file=sys.stderr)
        return 2
    from .tray import Tray

    comp = _build_companion(bus, cfg_data)
    fut = runner.submit(comp.run())

    quit_event = {"called": False}

    def _quit_all() -> None:
        if quit_event["called"]:
            return
        quit_event["called"] = True
        comp.stop()
        try:
            fut.result(timeout=8.0)
        except Exception:
            pass
        try:
            app.quit_app()
        except Exception:
            pass
        runner.stop()

    def _on_quit_req(reason: str) -> None:
        if reason == "hide" and tray_started:
            return  # 仅隐藏，托盘还在
        # 没托盘 → 直接退
        _quit_all()

    app = CompanionApp(bus=bus, runner=runner, cfg_data=cfg_data,
                        on_quit_request=_on_quit_req)
    tray = Tray(on_show=lambda: app.root.after(0, app.show_window),
                 on_quit=lambda: app.root.after(0, _quit_all))
    tray_started = tray.start()

    try:
        app.mainloop()
    finally:
        if not quit_event["called"]:
            _quit_all()
        tray.stop()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m companion",
        description="ESP32 BLE 桌面伴侣（GUI + 后台）")
    parser.add_argument("--no-gui", action="store_true",
                         help="只跑后台，无 tkinter 窗口")
    parser.add_argument("--device", help="覆盖设备名（默认读 ~/.dynapp/companion.json）")
    parser.add_argument("--log-level", default="INFO",
                         choices=("DEBUG", "INFO", "WARNING", "ERROR"))
    args = parser.parse_args(argv)
    _setup_logging(args.log_level)

    cfg_data = cfg.load()
    if args.device:
        cfg_data["device_name"] = args.device

    bus = EventBus()
    runner = AsyncRunner()
    runner.start()
    bus.set_loop(runner.loop)

    try:
        if args.no_gui:
            return _run_no_gui(bus, runner, cfg_data)
        return _run_gui(bus, runner, cfg_data)
    finally:
        # 持久化配置
        cfg.save(cfg_data)


if __name__ == "__main__":
    sys.exit(main())
