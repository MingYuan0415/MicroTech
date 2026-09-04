#!/usr/bin/env python3
"""simctl - MicroTech LVGL simulator Agent client (JSON-RPC over TCP).

Usage:
  python3 sim/tools/simctl.py [--host H] [--port P] <command> [args]

Commands:
  ping
  step <ms>
  wait [--timeout-ms N]
  screenshot <name> [--no-wait]
  tree [> file.json]
  touch <down|move|up> <x> <y>
  key <boot|power> <press|release|click>
  navigate <app_id> [page_id]
  set-wifi <connected|disconnected>
  set-wifi-scan <json|file>   # {"records":[{"ssid":..,"rssi":..,"security":..}], "request":true,"trigger":true,"wait_scan":true}
  sd <mount|umount|write|list> [--name REC_x.wav --seconds 3]
  nvs <get|set|erase> <key> [value]
  connectivity
  apps
  set-time <epoch>
  set-power <voltage_mv> <pct> [charging]
  set-imu <pitch_deg> <roll_deg>
  pause <on|off>
"""
import argparse
import json
import os
import socket
import sys


def rpc(sock, method, params=None):
    rpc.rid = getattr(rpc, "rid", 0) + 1
    msg = {"id": rpc.rid, "method": method}
    if params is not None:
        msg["params"] = params
    sock.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode())


def main():
    ap = argparse.ArgumentParser(prog="simctl")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5002)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    sp = sub.add_parser("step"); sp.add_argument("ms", nargs="?", type=int, default=33)
    wp = sub.add_parser("wait"); wp.add_argument("--timeout-ms", type=int, default=5000)
    scp = sub.add_parser("screenshot"); scp.add_argument("name"); scp.add_argument("--no-wait", action="store_true")
    sub.add_parser("tree")
    tp = sub.add_parser("touch"); tp.add_argument("action"); tp.add_argument("x", type=int); tp.add_argument("y", type=int)
    kp = sub.add_parser("key"); kp.add_argument("button"); kp.add_argument("action")
    np_ = sub.add_parser("navigate"); np_.add_argument("app"); np_.add_argument("page", nargs="?")
    sw = sub.add_parser("set-wifi"); sw.add_argument("state")
    sws = sub.add_parser("set-wifi-scan"); sws.add_argument("json", help="JSON with records/request/trigger/wait_scan")
    sd = sub.add_parser("sd"); sd.add_argument("action", choices=["mount", "umount", "write", "list"]); sd.add_argument("--name"); sd.add_argument("--seconds", type=int)
    nvs = sub.add_parser("nvs"); nvs.add_argument("action", choices=["get", "set", "erase"]); nvs.add_argument("key"); nvs.add_argument("value", nargs="?")
    sub.add_parser("connectivity")
    sub.add_parser("apps")
    etp = sub.add_parser("set-time"); etp.add_argument("epoch", type=int)
    ep = sub.add_parser("set-power"); ep.add_argument("voltage", nargs="?"); ep.add_argument("pct", nargs="?"); ep.add_argument("charging", nargs="?", default="false")
    ei = sub.add_parser("set-imu"); ei.add_argument("pitch", type=float); ei.add_argument("roll", type=float)
    pp = sub.add_parser("pause"); pp.add_argument("enabled", choices=["on", "off"])
    ew = sub.add_parser("set-weather"); ew.add_argument("endpoint"); ew.add_argument("file")
    sub.add_parser("exit")
    pm = sub.add_parser("pm"); pm.add_argument("--off-ms", type=int, default=None); pm.add_argument("--standby-ms", type=int, default=None); pm.add_argument("--get", action="store_true")
    args = ap.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=3)
    except OSError as exc:
        print("simctl: cannot connect to %s:%d: %s" %
              (args.host, args.port, exc), file=sys.stderr)
        print("请先运行 python3 sim/dev.py，或检查模拟器会话和端口。",
              file=sys.stderr)
        sys.exit(2)

    with sock:
        if args.cmd == "ping":
            req = ("sim.ping", None)
        elif args.cmd == "step":
            req = ("sim.step", {"ms": args.ms})
        elif args.cmd == "wait":
            req = ("sim.wait_idle", {"timeout_ms": args.timeout_ms})
        elif args.cmd == "screenshot":
            req = ("sim.screenshot", {"name": args.name, "wait_idle": not args.no_wait})
        elif args.cmd == "tree":
            req = ("sim.tree", {})
        elif args.cmd == "touch":
            req = ("sim.touch", {"action": args.action, "x": args.x, "y": args.y})
        elif args.cmd == "key":
            req = ("sim.key", {"button": args.button, "action": args.action})
        elif args.cmd == "navigate":
            nav = {"app": args.app}
            if args.page:
                nav["page"] = args.page
            req = ("sim.navigate", nav)
        elif args.cmd == "set-wifi":
            req = ("sim.set_wifi", {"state": args.state})
        elif args.cmd == "set-wifi-scan":
            text = args.json
            if os.path.exists(text):
                text = open(text).read()
            req = ("sim.set_wifi_scan", json.loads(text))
        elif args.cmd == "sd":
            req = ("sim.sd", {"action": args.action,
                              **({"name": args.name} if args.name else {}),
                              **({"seconds": args.seconds} if args.seconds is not None else {})})
        elif args.cmd == "nvs":
            req = ("sim.nvs", {"action": args.action, "key": args.key,
                               **({"value": args.value} if args.value else {})})
        elif args.cmd == "connectivity":
            req = ("sim.connectivity", {})
        elif args.cmd == "apps":
            req = ("sim.apps", {})
        elif args.cmd == "set-time":
            req = ("sim.set_time", {"epoch": args.epoch})
        elif args.cmd == "set-power":
            power = {"charging": args.charging == "true"}
            if args.voltage not in (None, "-"):
                power["voltage"] = int(args.voltage)
            if args.pct not in (None, "-"):
                power["pct"] = int(args.pct)
            req = ("sim.set_power", power)
        elif args.cmd == "set-imu":
            req = ("sim.set_imu", {"pitch": args.pitch, "roll": args.roll})
        elif args.cmd == "pause":
            req = ("sim.pause", {"enabled": args.enabled == "on"})
        elif args.cmd == "set-weather":
            req = ("sim.set_weather", {"endpoint": args.endpoint,
                                       "status": 200,
                                       "body": open(args.file).read()})
        elif args.cmd == "exit":
            req = ("sim.exit", {})
        elif args.cmd == "pm":
            params = {}
            if args.get:
                params["get"] = True
            if args.off_ms is not None:
                params["off_ms"] = args.off_ms
            if args.standby_ms is not None:
                params["standby_ms"] = args.standby_ms
            req = ("sim.pm", params)
        reply = rpc(sock, *req)
        if not reply.get("ok"):
            print(json.dumps(reply, ensure_ascii=False))
        elif args.cmd == "tree":
            print(json.dumps(reply.get("result", {}).get("tree"), ensure_ascii=False))
        else:
            print(json.dumps(reply, ensure_ascii=False))
        sys.exit(0 if reply.get("ok") else 1)


if __name__ == "__main__":
    main()
