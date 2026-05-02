"""持久化配置：~/.dynapp/companion.json。

字段：
  device_name    扫描时匹配的子串，默认 ESP32-S3-DEMO
  device_address 缓存上次连上的 MAC，加速下次启动
  providers      provider 启用开关 dict
  notify_history 手动通知最近 5 条历史
"""

from __future__ import annotations

import json
import logging
import os
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

CONFIG_DIR  = Path(os.path.expanduser("~")) / ".dynapp"
CONFIG_PATH = CONFIG_DIR / "companion.json"

DEFAULT: dict[str, Any] = {
    "device_name": "ESP32-S3-DEMO",
    "device_address": None,
    "providers": {
        "time":    True,
        "weather": True,
        "notify":  True,
        "system":  True,
        "media":   True,
        "bridge":  True,
        "upload":  True,
    },
    "music_folder": str(Path(os.path.expanduser("~")) / "Music" / "Watch"),
    "notify_history": [],
}


def load() -> dict[str, Any]:
    if not CONFIG_PATH.exists():
        return dict(DEFAULT)
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        logger.warning("config load failed: %s; using defaults", e)
        return dict(DEFAULT)
    merged = dict(DEFAULT)
    if isinstance(data, dict):
        merged.update(data)
        if isinstance(data.get("providers"), dict):
            merged["providers"] = {**DEFAULT["providers"], **data["providers"]}
    return merged


def save(cfg: dict[str, Any]) -> None:
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception as e:
        logger.warning("config save failed: %s", e)
