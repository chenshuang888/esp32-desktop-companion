"""weather plugin —— 动态 app 调 ble.send('req') 取天气数据。

通用服务（bind_app=None），任何动态 app 都可以发 from=weather/type=req 来请求。
"""

from __future__ import annotations

import time

from companion.plugin_sdk import Plugin
from companion.plugin_sdk.platform import geoip_weather

get_weather = geoip_weather.get_weather


_WMO = {
    0: "clear", 1: "cloudy", 2: "cloudy", 3: "overcast",
    45: "fog", 48: "fog",
    51: "rain", 53: "rain", 55: "rain", 56: "rain", 57: "rain",
    61: "rain", 63: "rain", 65: "rain", 66: "rain", 67: "rain",
    71: "snow", 73: "snow", 75: "snow", 77: "snow", 85: "snow", 86: "snow",
    80: "rain", 81: "rain", 82: "rain",
    95: "thunder", 96: "thunder", 99: "thunder",
}


class WeatherPlugin(Plugin):
    plugin_id = "weather"
    title     = ""           # 无 GUI 页
    bind_app  = None          # 通用服务

    async def on_message(self, msg: dict) -> None:
        # 只关心 from=weather/type=req
        if msg.get("from") != "weather" or msg.get("type") != "req":
            return
        body = msg.get("body") if isinstance(msg.get("body"), dict) else {}
        force = bool(body.get("force"))
        try:
            snap = await get_weather(force=force)
            self.tx_to("weather", "data", body={
                "temp_c":   round(snap.temp_c, 1),
                "temp_min": round(snap.temp_min, 1),
                "temp_max": round(snap.temp_max, 1),
                "humidity": int(snap.humidity),
                "code":     _WMO.get(snap.wmo, "unknown"),
                "city":     snap.city,
                "desc":     snap.desc(),
                "ts":       int(time.time()),
            })
        except Exception as e:
            self.log.warning("weather fetch: %s", e)
            self.tx_to("weather", "error", body={"msg": str(e)})
