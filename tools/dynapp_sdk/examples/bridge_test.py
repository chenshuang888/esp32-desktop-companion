"""Dynamic App Bridge —— 命令行测试工具 (v2: to/from 路由)

用途：跟 ESP32 上某个 dynamic app 做 BLE 来回。

GATT:
    Service:  a3a30001-0000-4aef-b87e-4fa1e0c7e0f6
    rx char:  a3a30002-...   WRITE         (PC 写 → ESP 收)
    tx char:  a3a30003-...   READ + NOTIFY (ESP 推 → PC 收)

协议（默认 JSON 模式）:
    PC → ESP   { "to": <app>, "type": "msg", "body": "<text>" }
    ESP → PC   { "from": <app>, "type": "msg", "body": "..." }
    "to": "*" 表示广播。

用法 (从仓库根目录运行):
    # 跟 echo app 互发 msg（自动包 JSON）
    python tools/dynapp_sdk/examples/bridge_test.py --to echo
        > hello                  # 输入 "hello" + 回车
        # ESP 回: <echo> hello (echo)

    # 一次性发一条
    python tools/dynapp_sdk/examples/bridge_test.py --to echo --once "hi"

    # 自定义 type (高级)
    python tools/dynapp_sdk/examples/bridge_test.py --to echo --type ping --once ""

    # 裸字节模式（不包 JSON，给低层调试）
    python tools/dynapp_sdk/examples/bridge_test.py --raw
        > hello                  # 直接发 b"hello"

    # 指定 mac，跳过扫描
    python tools/dynapp_sdk/examples/bridge_test.py --addr XX:XX:XX:XX:XX:XX --to echo
"""

import argparse
import asyncio
import json
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME_HINT = "ESP32"
SVC_UUID = "a3a30001-0000-4aef-b87e-4fa1e0c7e0f6"
RX_UUID  = "a3a30002-0000-4aef-b87e-4fa1e0c7e0f6"
TX_UUID  = "a3a30003-0000-4aef-b87e-4fa1e0c7e0f6"

MAX_PAYLOAD = 200


async def scan_for_esp() -> str | None:
    print("[scan] scanning 5s for ESP32...")
    devices = await BleakScanner.discover(timeout=5.0)
    for d in devices:
        if d.name and DEVICE_NAME_HINT.lower() in d.name.lower():
            print(f"[scan] found {d.name} @ {d.address}")
            return d.address
    print("[scan] no candidate found")
    return None


def make_recv_handler(raw_mode: bool):
    def on_notify(_handle: int, data: bytearray) -> None:
        if raw_mode:
            try:
                print(f"[recv-raw] {data.decode('utf-8', errors='replace')}")
            except Exception:
                print(f"[recv-raw] {data!r}")
            return
        # JSON 模式：解析 + 按 from 显示
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            print(f"[recv] (binary) {data!r}")
            return
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            print(f"[recv-nonjson] {text}")
            return
        sender = msg.get("from", "<unknown>")
        mtype = msg.get("type", "<no-type>")
        # 把已知字段抠掉再显示剩余 body，方便看
        rest = {k: v for k, v in msg.items() if k not in ("from", "type")}
        if rest:
            body = json.dumps(rest, ensure_ascii=False)
            print(f"[recv] <{sender}> {mtype}: {body}")
        else:
            print(f"[recv] <{sender}> {mtype}")
    return on_notify


def build_payload(line: str, to: str | None, mtype: str, raw_mode: bool) -> bytes:
    if raw_mode:
        return line.encode("utf-8")
    # JSON 模式
    msg = {"to": to, "type": mtype, "body": line}
    return json.dumps(msg, ensure_ascii=False).encode("utf-8")


async def write_one(client: BleakClient, payload: bytes) -> bool:
    if len(payload) > MAX_PAYLOAD:
        print(f"[send] too long ({len(payload)} > {MAX_PAYLOAD}), dropped")
        return False
    await client.write_gatt_char(RX_UUID, payload, response=False)
    return True


async def run_interactive(addr: str, to: str | None, mtype: str, raw_mode: bool) -> None:
    print(f"[conn] connecting to {addr} ...")
    async with BleakClient(addr) as client:
        if not client.is_connected:
            print("[conn] failed")
            return
        print("[conn] connected, subscribing tx ...")
        await client.start_notify(TX_UUID, make_recv_handler(raw_mode))
        if raw_mode:
            print("[conn] ready (RAW). type and Enter to send. Ctrl+C to quit.")
        else:
            print(f"[conn] ready. to={to!r} type={mtype!r}. type body + Enter. Ctrl+C to quit.")

        loop = asyncio.get_event_loop()
        try:
            while True:
                line = await loop.run_in_executor(None, sys.stdin.readline)
                if not line:
                    break
                line = line.rstrip("\r\n")
                if not line and not raw_mode:
                    # 允许发空 body（type-only 消息），按回车两次确认
                    pass
                payload = build_payload(line, to, mtype, raw_mode)
                ok = await write_one(client, payload)
                if ok:
                    print(f"[sent] {payload.decode('utf-8', errors='replace')}")
        except (KeyboardInterrupt, asyncio.CancelledError):
            print("\n[bye]")
        finally:
            await client.stop_notify(TX_UUID)


async def run_once(addr: str, msg: str, to: str | None, mtype: str, raw_mode: bool) -> None:
    print(f"[conn] connecting to {addr} ...")
    async with BleakClient(addr) as client:
        await client.start_notify(TX_UUID, make_recv_handler(raw_mode))
        payload = build_payload(msg, to, mtype, raw_mode)
        ok = await write_one(client, payload)
        if ok:
            print(f"[sent] {payload.decode('utf-8', errors='replace')}")
        # 等 ESP 回 1s
        await asyncio.sleep(1.0)
        await client.stop_notify(TX_UUID)


async def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--addr", help="ESP32 MAC; if omitted, auto scan")
    p.add_argument("--to",   help="target app name (echo/alarm/...). required unless --raw")
    p.add_argument("--type", default="msg", help="message type field (default 'msg')")
    p.add_argument("--once", help="send one message body then exit")
    p.add_argument("--raw",  action="store_true",
                   help="raw bytes mode (no JSON wrap), for low-level debug")
    args = p.parse_args()

    if not args.raw and not args.to:
        sys.exit("--to <app> required (or use --raw for raw byte mode)")

    addr = args.addr or await scan_for_esp()
    if not addr:
        sys.exit("no device")

    if args.once is not None:
        await run_once(addr, args.once, args.to, args.type, args.raw)
    else:
        await run_interactive(addr, args.to, args.type, args.raw)


if __name__ == "__main__":
    asyncio.run(main())
