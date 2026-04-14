#!/usr/bin/env python3
"""
Mac Clipboard WebSocket Bridge
Pure Python, zero external dependencies.

Usage:
    python3 clipboard_bridge.py [port] [token]

Examples:
    python3 clipboard_bridge.py             # ws://127.0.0.1:9001  (no auth)
    python3 clipboard_bridge.py 9001 mytoken  # ws://127.0.0.1:9001?token=mytoken

If connecting from a remote browser over SSH tunnel:
    ssh -L 9001:localhost:9001 user@your-mac
    then use ws://127.0.0.1:9001 in the browser

Protocol (JSON text frames):
    Client → Server:  {"cmd": "get"}                     → ask for Mac clipboard
    Server → Client:  {"cmd": "clipboard", "text": "..."} → Mac clipboard content
    Client → Server:  {"cmd": "set", "text": "..."}       → set Mac clipboard
    Server → Client:  {"cmd": "ok"}                       → set confirmed
    Server → Client:  {"cmd": "error", "msg": "..."}      → error
"""

import sys
import socket
import threading
import hashlib
import base64
import struct
import json
import subprocess
import urllib.parse

# ── Config ────────────────────────────────────────────────────────────────────
PORT  = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
TOKEN = sys.argv[2] if len(sys.argv) > 2 else None
HOST  = "0.0.0.0"   # localhost only — safe default
WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# ── WebSocket helpers ─────────────────────────────────────────────────────────

def _recv_exactly(conn: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed")
        buf += chunk
    return buf


def ws_recv(conn: socket.socket):
    """
    Read one complete WebSocket frame.
    Returns (opcode, text_payload) or (None, None) on close/error.
    Opcodes: 1=text, 2=binary, 8=close, 9=ping, 10=pong
    """
    try:
        b0, b1   = _recv_exactly(conn, 2)
        opcode   = b0 & 0x0F
        is_mask  = (b1 >> 7) & 1
        pay_len  = b1 & 0x7F

        if opcode == 8:               # CLOSE
            return None, None

        if pay_len == 126:
            pay_len = struct.unpack(">H", _recv_exactly(conn, 2))[0]
        elif pay_len == 127:
            pay_len = struct.unpack(">Q", _recv_exactly(conn, 8))[0]

        mask_key = _recv_exactly(conn, 4) if is_mask else None
        payload  = _recv_exactly(conn, pay_len)

        if mask_key:
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

        return opcode, payload.decode("utf-8", errors="replace")

    except (ConnectionError, OSError, struct.error):
        return None, None


def ws_send(conn: socket.socket, text: str):
    """Send a WebSocket text frame (server-side: no masking required)."""
    payload = text.encode("utf-8")
    n = len(payload)
    if   n < 126:       header = struct.pack("BB",    0x81, n)
    elif n < 0x10000:   header = struct.pack(">BBH",  0x81, 126, n)
    else:               header = struct.pack(">BBQ",  0x81, 127, n)
    conn.sendall(header + payload)


def ws_pong(conn: socket.socket, payload: bytes = b""):
    """Reply to a ping."""
    n = len(payload)
    conn.sendall(bytes([0x8A, n]) + payload)


def ws_handshake(conn: socket.socket, raw_request: bytes) -> bool:
    """
    Perform the HTTP→WebSocket upgrade.
    Returns True on success, False on failure (token mismatch, bad request, etc.)
    """
    text   = raw_request.decode("utf-8", errors="replace")
    lines  = text.split("\r\n")
    req_line = lines[0]  # "GET /?token=xxx HTTP/1.1"

    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()

    # Token check
    if TOKEN:
        try:
            path = req_line.split(" ")[1]
            qs   = urllib.parse.parse_qs(urllib.parse.urlparse(path).query)
            client_token = qs.get("token", [""])[0]
        except Exception:
            client_token = ""

        if client_token != TOKEN:
            conn.sendall(
                b"HTTP/1.1 403 Forbidden\r\n"
                b"Content-Length: 13\r\n\r\n"
                b"Invalid token"
            )
            return False

    ws_key = headers.get("sec-websocket-key", "")
    if not ws_key:
        conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
        return False

    accept = base64.b64encode(
        hashlib.sha1((ws_key + WS_MAGIC).encode()).digest()
    ).decode()

    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n"
    ).encode())
    return True


# ── Mac clipboard via pbcopy / pbpaste ───────────────────────────────────────

def mac_get_clipboard() -> str:
    result = subprocess.run(["pbpaste"], capture_output=True)
    return result.stdout.decode("utf-8", errors="replace")


def mac_set_clipboard(text: str):
    subprocess.run(["pbcopy"], input=text.encode("utf-8"))


# ── Per-connection handler ────────────────────────────────────────────────────

def handle_connection(conn: socket.socket, addr):
    print(f"[+] {addr}")
    try:
        # Read HTTP upgrade request (ends with \r\n\r\n)
        raw = b""
        while b"\r\n\r\n" not in raw:
            chunk = conn.recv(4096)
            if not chunk:
                return
            raw += chunk

        if not ws_handshake(conn, raw):
            return

        print(f"    WebSocket ready: {addr}")

        while True:
            opcode, text = ws_recv(conn)

            if opcode is None:
                break

            if opcode == 9:          # ping → pong
                ws_pong(conn, text.encode() if text else b"")
                continue

            if opcode not in (1, 2): # ignore non-text/binary frames
                continue

            try:
                msg = json.loads(text)
                cmd = msg.get("cmd")

                if cmd == "get":
                    clip = mac_get_clipboard()
                    ws_send(conn, json.dumps({"cmd": "clipboard", "text": clip}))
                    print(f"    GET → {len(clip)} chars")

                elif cmd == "set":
                    clip_text = msg.get("text", "")
                    mac_set_clipboard(clip_text)
                    ws_send(conn, json.dumps({"cmd": "ok"}))
                    print(f"    SET ← {len(clip_text)} chars: {clip_text[:40]!r}")

                elif cmd == "ping":
                    ws_send(conn, json.dumps({"cmd": "pong"}))

                else:
                    ws_send(conn, json.dumps({"cmd": "error", "msg": f"unknown cmd: {cmd}"}))

            except json.JSONDecodeError:
                ws_send(conn, json.dumps({"cmd": "error", "msg": "invalid JSON"}))
            except Exception as e:
                ws_send(conn, json.dumps({"cmd": "error", "msg": str(e)}))

    except (ConnectionError, OSError):
        pass
    except Exception as e:
        print(f"    Error in {addr}: {e}")
    finally:
        conn.close()
        print(f"[-] {addr}")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(10)

    print("=" * 50)
    print(f"📋  Mac Clipboard Bridge")
    print(f"    ws://{HOST}:{PORT}", end="")
    if TOKEN:
        print(f"?token={TOKEN}")
        print(f"\n    Use this URL in the browser:")
        print(f"    ws://{HOST}:{PORT}?token={TOKEN}")
    else:
        print()
        print(f"\n    No token (localhost only — reasonably safe)")
    print("=" * 50)
    print("Ctrl+C to stop\n")

    try:
        while True:
            conn, addr = server.accept()
            t = threading.Thread(target=handle_connection, args=(conn, addr), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.close()


main()
