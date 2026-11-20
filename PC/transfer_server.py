#!/usr/bin/env python3
"""
Wii U Transfer - Servidor de streaming por red (PC Host)
Protocolo binario NFSA (TCP persistente + autodescubrimiento UDP) para servir
paquetes WUP por bloques (offset + length) sin copiar archivos a la SD.
"""

import argparse
import socket
import struct
import threading
from pathlib import Path
from typing import Callable, Optional

MAGIC = b"NFSA"
VERSION = 1

CMD_LIST   = 0x01
CMD_STAT   = 0x02
CMD_READ   = 0x03
CMD_CLOSE  = 0x04
CMD_PING   = 0x05
CMD_STREAM = 0x06

STATUS_OK      = 0
STATUS_ERR_BAD = 1
STATUS_ERR_NF  = 2
STATUS_ERR_IO  = 3

TCP_PORT = 7331
UDP_DISCOVERY_PORT = 7332
MAX_READ_CHUNK = 2 * 1024 * 1024  # 2 MB máximo por petición

EventCallback = Callable[[str, str], None]


def get_local_ip() -> str:
    """Obtiene la IP LAN real del PC mediante socket UDP sin enviar tráfico."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        return ip or "127.0.0.1"
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def format_bytes(num_bytes: int) -> str:
    """Formatea bytes a una cadena legible (B, KB, MB, GB)."""
    if num_bytes < 1024:
        return f"{num_bytes} B"
    kb = num_bytes / 1024.0
    if kb < 1024.0:
        return f"{kb:.1f} KB"
    mb = kb / 1024.0
    if mb < 1024.0:
        return f"{mb:.1f} MB"
    gb = mb / 1024.0
    return f"{gb:.2f} GB"


def scan_packages(root_dir: Path) -> list[dict]:
    """Escanea subcarpetas y devuelve {name, file_count, total_bytes, has_tmd}."""
    items: list[dict] = []
    if not root_dir.exists() or not root_dir.is_dir():
        return items

    for entry in sorted(root_dir.iterdir(), key=lambda p: p.name.lower()):
        if not entry.is_dir():
            continue
        files = [f for f in entry.rglob("*") if f.is_file()]
        total_size = 0
        has_tmd = False
        for f in files:
            try:
                total_size += f.stat().st_size
            except OSError:
                pass
            if f.name.lower() == "title.tmd":
                has_tmd = True
        items.append({
            "name": entry.name,
            "file_count": len(files),
            "total_bytes": total_size,
            "has_tmd": has_tmd,
        })
    return items


def _recv_exact(sock: socket.socket, n: int, stop_event: threading.Event) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        if stop_event.is_set():
            raise ConnectionError("Servidor detenido")
        try:
            chunk = sock.recv(n - len(buf))
        except socket.timeout:
            continue
        if not chunk:
            raise ConnectionError("Cliente desconectado")
        buf.extend(chunk)
    return bytes(buf)


def _send_packet(sock: socket.socket, status: int, payload: bytes = b"") -> int:
    header = struct.pack(">4sBBHI", MAGIC, VERSION, status, 0, len(payload))
    packet = header + payload
    sock.sendall(packet)
    return len(packet)


def _safe_resolve(root: Path, rel_path: str) -> Optional[Path]:
    rel_clean = rel_path.strip("/\\")
    base = root.resolve()
    target = (base / rel_clean).resolve()
    try:
        target.relative_to(base)
    except ValueError:
        return None
    return target


class TransferServer:
    """Servidor TCP/UDP reutilizable por la GUI y modo CLI."""

    def __init__(
        self,
        root_dir: str | Path,
        tcp_port: int = TCP_PORT,
        udp_port: int = UDP_DISCOVERY_PORT,
        on_event: Optional[EventCallback] = None,
    ) -> None:
        self._lock = threading.Lock()
        self._root = Path(root_dir).resolve()
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self._on_event = on_event

        self._stop_event = threading.Event()
        self._running = False
        self._tcp_sock: Optional[socket.socket] = None
        self._udp_sock: Optional[socket.socket] = None
        self._tcp_thread: Optional[threading.Thread] = None
        self._udp_thread: Optional[threading.Thread] = None

        self._active_clients: set[str] = set()
        self._bytes_sent_total = 0
        self._bytes_sent_window = 0

    def _emit(self, level: str, message: str) -> None:
        if self._on_event:
            self._on_event(level, message)

    @property
    def is_running(self) -> bool:
        return self._running

    def get_root(self) -> Path:
        with self._lock:
            return self._root

    def set_root(self, path: str | Path) -> None:
        resolved = Path(path).resolve()
        resolved.mkdir(parents=True, exist_ok=True)
        with self._lock:
            self._root = resolved
        self._emit("INFO", f"Carpeta de paquetes cambiada a: {resolved}")

    def get_stats(self, reset_window: bool = True) -> dict:
        with self._lock:
            window_bytes = self._bytes_sent_window
            if reset_window:
                self._bytes_sent_window = 0
            return {
                "active_clients": sorted(self._active_clients),
                "client_count": len(self._active_clients),
                "bytes_sent_total": self._bytes_sent_total,
                "bytes_sent_window": window_bytes,
            }

    def start(self) -> None:
        if self._running:
            return

        root = self.get_root()
        root.mkdir(parents=True, exist_ok=True)
        self._stop_event.clear()

        with self._lock:
            self._active_clients.clear()
            self._bytes_sent_window = 0

        tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        tcp_sock.settimeout(0.5)
        tcp_sock.bind(("0.0.0.0", self.tcp_port))
        tcp_sock.listen(8)

        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        udp_sock.settimeout(0.5)
        udp_sock.bind(("0.0.0.0", self.udp_port))

        self._tcp_sock = tcp_sock
        self._udp_sock = udp_sock
        self._running = True

        self._udp_thread = threading.Thread(target=self._udp_loop, daemon=True)
        self._tcp_thread = threading.Thread(target=self._tcp_loop, daemon=True)
        self._udp_thread.start()
        self._tcp_thread.start()

        ip = get_local_ip()
        self._emit("OK", f"Servidor iniciado en {ip}:{self.tcp_port} (UDP {self.udp_port})")

    def stop(self) -> None:
        if not self._running and self._tcp_sock is None and self._udp_sock is None:
            return

        self._stop_event.set()
        self._running = False

        for s in (self._tcp_sock, self._udp_sock):
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass
        self._tcp_sock = None
        self._udp_sock = None

        for t in (self._tcp_thread, self._udp_thread):
            if t is not None and t.is_alive():
                t.join(timeout=1.5)
        self._tcp_thread = None
        self._udp_thread = None

        with self._lock:
            self._active_clients.clear()
            self._bytes_sent_window = 0

        self._emit("WARN", "Servidor detenido.")

    def _udp_loop(self) -> None:
        sock = self._udp_sock
        if sock is None:
            return
        while not self._stop_event.is_set():
            try:
                data, addr = sock.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                break

            if data.startswith(b"NETFSA_DISCOVER"):
                reply = f"NETFSA_HERE|{socket.gethostname()}|{self.tcp_port}".encode("utf-8")
                try:
                    sock.sendto(reply, addr)
                    self._emit("INFO", f"Autodescubrimiento respondido a {addr[0]}")
                except OSError:
                    pass

    def _tcp_loop(self) -> None:
        sock = self._tcp_sock
        if sock is None:
            return
        while not self._stop_event.is_set():
            try:
                conn, addr = sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break

            t = threading.Thread(
                target=self._handle_client,
                args=(conn, addr),
                daemon=True,
            )
            t.start()

    def _record_sent(self, count: int) -> None:
        with self._lock:
            self._bytes_sent_total += count
            self._bytes_sent_window += count

    def _handle_client(self, conn: socket.socket, addr: tuple[str, int]) -> None:
        client_ip = addr[0]
        client_tag = f"{addr[0]}:{addr[1]}"
        conn.settimeout(5.0)
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
        except OSError:
            pass

        with self._lock:
            self._active_clients.add(client_ip)
        self._emit("OK", f"Consola/Cliente conectado: {client_tag}")

        open_files: dict[str, object] = {}
        try:
            while not self._stop_event.is_set():
                hdr = _recv_exact(conn, 12, self._stop_event)
                magic, ver, cmd, _flags, payload_len = struct.unpack(">4sBBHI", hdr)
                if magic != MAGIC or ver != VERSION:
                    self._record_sent(_send_packet(conn, STATUS_ERR_BAD, b"Bad header"))
                    continue

                payload = _recv_exact(conn, payload_len, self._stop_event) if payload_len > 0 else b""
                root = self.get_root()

                if cmd == CMD_PING:
                    self._record_sent(_send_packet(conn, STATUS_OK, b"PONG"))

                elif cmd == CMD_LIST:
                    sub = payload.decode("utf-8", errors="replace").strip("\x00")
                    lines = []
                    if not sub:
                        for pkg in scan_packages(root):
                            lines.append(f"D|{pkg['name']}|{pkg['total_bytes']}|{pkg['file_count']}")
                    else:
                        target = _safe_resolve(root, sub)
                        if not target or not target.is_dir():
                            self._record_sent(_send_packet(conn, STATUS_ERR_NF))
                            continue
                        for child in sorted(target.iterdir()):
                            kind = "D" if child.is_dir() else "F"
                            sz = child.stat().st_size if child.is_file() else 0
                            lines.append(f"{kind}|{child.name}|{sz}|0")
                    sent = _send_packet(conn, STATUS_OK, "\n".join(lines).encode("utf-8"))
                    self._record_sent(sent)

                elif cmd == CMD_STAT:
                    rel = payload.decode("utf-8", errors="replace").strip("\x00")
                    target = _safe_resolve(root, rel)
                    if not target or not target.exists():
                        self._record_sent(_send_packet(conn, STATUS_ERR_NF))
                        continue
                    is_dir = 1 if target.is_dir() else 0
                    size = target.stat().st_size if target.is_file() else 0
                    sent = _send_packet(conn, STATUS_OK, struct.pack(">BQ", is_dir, size))
                    self._record_sent(sent)

                elif cmd == CMD_READ:
                    if len(payload) < 12:
                        self._record_sent(_send_packet(conn, STATUS_ERR_BAD))
                        continue
                    offset, length = struct.unpack(">QI", payload[:12])
                    rel = payload[12:].decode("utf-8", errors="replace").strip("\x00")
                    if length > MAX_READ_CHUNK:
                        length = MAX_READ_CHUNK

                    target = _safe_resolve(root, rel)
                    if not target or not target.is_file():
                        self._record_sent(_send_packet(conn, STATUS_ERR_NF))
                        continue

                    key = str(target)
                    fp = open_files.get(key)
                    if fp is None:
                        fp = open(target, "rb")
                        open_files[key] = fp
                        self._emit("INFO", f"Streaming archivo: {rel}")

                    fp.seek(offset)
                    data = fp.read(length)
                    sent = _send_packet(conn, STATUS_OK, data)
                    self._record_sent(sent)

                elif cmd == CMD_CLOSE:
                    rel = payload.decode("utf-8", errors="replace").strip("\x00")
                    target = _safe_resolve(root, rel)
                    if target:
                        fp = open_files.pop(str(target), None)
                        if fp:
                            fp.close()
                    self._record_sent(_send_packet(conn, STATUS_OK))

                elif cmd == CMD_STREAM:
                    rel = payload.decode("utf-8", errors="replace").strip("\x00")
                    target = _safe_resolve(root, rel)
                    if not target or not target.is_file():
                        self._record_sent(_send_packet(conn, STATUS_ERR_NF))
                        continue

                    file_size = target.stat().st_size
                    self._emit("INFO", f"Streaming continuo: {rel} ({format_bytes(file_size)})")
                    sent_hdr = _send_packet(conn, STATUS_OK, struct.pack(">Q", file_size))
                    self._record_sent(sent_hdr)

                    stream_chunk = 256 * 1024
                    with open(target, "rb") as sf:
                        remaining = file_size
                        while remaining > 0 and not self._stop_event.is_set():
                            chunk = sf.read(min(stream_chunk, remaining))
                            if not chunk:
                                break
                            conn.sendall(chunk)
                            c_len = len(chunk)
                            remaining -= c_len
                            self._record_sent(c_len)

                else:
                    self._record_sent(_send_packet(conn, STATUS_ERR_BAD))

        except (ConnectionError, OSError) as exc:
            self._emit("INFO", f"Cliente desconectado ({client_tag}): {exc}")
        finally:
            for fp in open_files.values():
                try:
                    fp.close()
                except OSError:
                    pass
            try:
                conn.close()
            except OSError:
                pass
            with self._lock:
                self._active_clients.discard(client_ip)


def main() -> None:
    parser = argparse.ArgumentParser(description="Wii U Transfer Server (Modo CLI)")
    parser.add_argument("--cli", action="store_true", help="Ejecuta el servidor en modo consola")
    parser.add_argument("folder", nargs="?", default="./packages", help="Carpeta raíz de paquetes")
    parser.add_argument("--port", type=int, default=TCP_PORT, help="Puerto TCP (por defecto 7331)")
    args = parser.parse_args()

    def log_cb(level: str, msg: str) -> None:
        print(f"[{level}] {msg}")

    srv = TransferServer(args.folder, tcp_port=args.port, on_event=log_cb)
    pkgs = scan_packages(srv.get_root())
    print("==========================================")
    print("  Wii U Transfer Server (CLI)")
    print("==========================================")
    print(f"Carpeta raíz : {srv.get_root()}")
    print(f"Paquetes     : {len(pkgs)}")
    for p in pkgs:
        kind = "Paquete WUP" if p["has_tmd"] else "Carpeta"
        print(f"  - {p['name']} ({p['file_count']} archivos, {format_bytes(p['total_bytes'])}) [{kind}]")

    srv.start()
    try:
        while True:
            threading.Event().wait(1.0)
    except KeyboardInterrupt:
        srv.stop()


if __name__ == "__main__":
    main()
