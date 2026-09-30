#!/usr/bin/env python3
"""Command-line helper for the SLA-USB print drive.

  python tools/slausb.py status
  python tools/slausb.py upload model.goo      # copy into the drive's flash (<= ~13 MB)
  python tools/slausb.py stream model.goo      # serve from this PC, any size; keep running
  python tools/slausb.py events                # follow the live activity log
  python tools/slausb.py ota .pio/build/sla-usb/firmware.bin

The device defaults to sla-usb.local; use --device <ip> or set SLAUSB_DEVICE.
Only the standard library is used.
"""

import argparse
import http.client
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def fmt(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


class Device:
    def __init__(self, host):
        self.host = host

    def _conn(self, timeout=30):
        return http.client.HTTPConnection(self.host, 80, timeout=timeout)

    def request(self, method, path, body=None, headers=None, timeout=30):
        c = self._conn(timeout)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        try:
            payload = json.loads(data or b"{}")
        except ValueError:
            payload = {"error": data.decode(errors="replace")}
        if r.status != 200:
            raise SystemExit(f"error {r.status}: {payload.get('error', payload)}")
        return payload

    def status(self):
        return self.request("GET", "/api/status")

    def post_form(self, path, fields, timeout=60):
        return self.request("POST", path, urllib.parse.urlencode(fields),
                            {"Content-Type": "application/x-www-form-urlencoded"}, timeout)

    def post_file(self, path, filename, label):
        """Multipart upload streamed from disk with a progress line."""
        size = os.path.getsize(filename)
        boundary = uuid.uuid4().hex
        head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                f"filename=\"{os.path.basename(filename)}\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n").encode()
        tail = f"\r\n--{boundary}--\r\n".encode()
        c = self._conn(timeout=300)
        c.putrequest("POST", path)
        c.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        c.putheader("Content-Length", str(len(head) + size + len(tail)))
        c.endheaders()
        c.send(head)
        sent, t0 = 0, time.time()
        with open(filename, "rb") as f:
            while chunk := f.read(64 * 1024):
                c.send(chunk)
                sent += len(chunk)
                rate = sent / max(time.time() - t0, 0.1)
                print(f"\r{label}: {fmt(sent)} / {fmt(size)}  ({fmt(rate)}/s)   ", end="", flush=True)
        c.send(tail)
        r = c.getresponse()
        payload = json.loads(r.read() or b"{}")
        print()
        if r.status != 200:
            raise SystemExit(f"error {r.status}: {payload.get('error', payload)}")
        return payload, time.time() - t0


def print_status(s):
    f, g, p, u = s["file"], s["goo"], s["progress"], s["usb"]
    usb = "detached" if not u["attached"] else "printer connected" if u["hostConnected"] else "no USB host"
    if u["busy"]:
        usb += ", printer reading"
    print(f"USB:     {usb}")
    if not f:
        print("File:    (drive empty)")
    else:
        where = f"streamed from {f['url']}" if f["kind"] == "stream" else "in flash"
        print(f"File:    {f['name']}  {fmt(f['size'])}  ({where})")
    if g:
        print(f"Print:   {g['layers']} layers x {g['layerHeight'] * 1000:.0f} um, {g['exposure']} s, "
              f"{g['printTime'] // 3600} h {g['printTime'] % 3600 // 60} min, {g['grams']:.1f} g, {g['machine']}")
        if p["layer"] >= 0:
            pct = 100 * (p["layer"] + 1) / g["layers"]
            print(f"Reading: layer {p['layer'] + 1} / {g['layers']} ({pct:.1f}%)")
    if "stream" in s:
        st = s["stream"]
        print(f"Stream:  {st['cached']}/{st['slots']} chunks cached, {st['fetched']} fetched, "
              f"{st['errors']} errors, {st['kbps']} KB/s" + (f", last error: {st['lastError']}" if st["lastError"] else ""))
    w = s["wifi"]
    print(f"Wi-Fi:   {w['ssid']} {w['ip']} ({w['rssi']} dBm)")


# --- serving files with Range support ----------------------------------------

class RangeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    files = {}  # url path -> local file
    stats = {"requests": 0, "bytes": 0, "last": ""}
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def _send_error(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _serve(self, body):
        path = self.files.get(urllib.parse.unquote(self.path.split("?")[0]))
        if not path:
            return self._send_error(404)
        size = os.path.getsize(path)
        start, end = 0, size - 1
        m = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
        if m:
            if m.group(1):
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
            else:
                start = size - int(m.group(2))
            end = min(end, size - 1)
            if start > end or start < 0:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                return self.end_headers()
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        length = end - start + 1
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        if not body:
            return
        with open(path, "rb") as f:
            f.seek(start)
            self.wfile.write(f.read(length))
        with self.lock:
            self.stats["requests"] += 1
            self.stats["bytes"] += length
            self.stats["last"] = f"{fmt(start)}-{fmt(end + 1)}"

    def do_GET(self):
        self._serve(True)

    def do_HEAD(self):
        self._serve(False)


def local_ip_towards(host):
    ip = socket.gethostbyname(host)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, 80))
        return s.getsockname()[0]
    finally:
        s.close()


# --- commands ------------------------------------------------------------------

def cmd_status(dev, args):
    print_status(dev.status())


def cmd_upload(dev, args):
    payload, secs = dev.post_file("/upload", args.file, "Uploading")
    print(f"Done: {payload['name']} ({fmt(payload['size'])}) in {secs:.1f} s. The printer sees it now.")


def cmd_ota(dev, args):
    payload, secs = dev.post_file("/api/firmware", args.file, "Firmware")
    print(f"Firmware installed ({fmt(payload['bytes'])}); the device is restarting.")


def cmd_stream(dev, args):
    path = os.path.abspath(args.file)
    name = os.path.basename(path)
    url_path = "/f/" + urllib.parse.quote(name)
    RangeHandler.files[urllib.parse.unquote(url_path)] = path
    server = ThreadingHTTPServer(("0.0.0.0", args.port), RangeHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    ip = local_ip_towards(dev.host)
    url = f"http://{ip}:{args.port}{url_path}"
    print(f"Serving {name} ({fmt(os.path.getsize(path))}) at {url}")
    print("If Windows asks, allow Python through the firewall (private networks).")
    payload = dev.post_form("/api/stream", {"url": url, "name": name}, timeout=60)
    print(f"The drive now shows {payload['name']}. Keep this running (and this PC awake) until the print is done.")
    print("Ctrl-C to stop serving.\n")
    try:
        while True:
            time.sleep(5)
            try:
                s = dev.status()
            except Exception as e:  # device busy or rebooting
                print(f"  (device not reachable: {e})")
                continue
            g, p = s["goo"], s["progress"]
            layer = f"layer {p['layer'] + 1}/{g['layers']}" if g and p["layer"] >= 0 else "not printing yet"
            st = RangeHandler.stats
            print(f"  {time.strftime('%H:%M:%S')}  served {st['requests']} requests / {fmt(st['bytes'])}"
                  f"  last {st['last'] or '-'}  |  printer: {layer}")
    except KeyboardInterrupt:
        s = dev.status()
        if s["usb"]["busy"]:
            print("\nWarning: the printer is still reading the file; the print will fail without this server.")
        server.shutdown()


def cmd_events(dev, args):
    since = 0
    while True:
        r = dev.request("GET", f"/api/events?since={since}")
        for e in r["events"]:
            if e["id"] < since or (e["id"] == since and since):
                continue  # re-sent growing read; skip for a simple tail
            if e["kind"] in ("read", "write"):
                if e["region"] == "file":
                    text = f"file {fmt(e['a'])} - {fmt(e['b'])}"
                    if e.get("la", -1) >= 0:
                        text += f"  layer {e['la'] + 1}" + (f"-{e['lb'] + 1}" if e["lb"] != e["la"] else "")
                else:
                    text = f"{e['region']} sector {e['a'] // 512} ({fmt(e['b'] - e['a'])})"
            else:
                text = e["text"]
            print(f"{time.strftime('%H:%M:%S')}  {e['kind']:<6} {text}")
        since = r["next"]
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default=os.environ.get("SLAUSB_DEVICE", "sla-usb.local"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("upload"); p.add_argument("file")
    p = sub.add_parser("stream"); p.add_argument("file"); p.add_argument("--port", type=int, default=8765)
    sub.add_parser("events")
    p = sub.add_parser("ota"); p.add_argument("file")
    args = ap.parse_args()
    dev = Device(args.device)
    {"status": cmd_status, "upload": cmd_upload, "stream": cmd_stream,
     "events": cmd_events, "ota": cmd_ota}[args.cmd](dev, args)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
