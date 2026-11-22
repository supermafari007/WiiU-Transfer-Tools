#!/usr/bin/env python3
"""
Wii U Transfer Tools v2.0.2 - Aplicación de Escritorio para Windows (Microsoft Edge WebView2)
Desarrollado por El Diuca.
Interfaz moderna nativa en WebView2 con servidor NFSA integrado, progreso en vivo por juego,
auto-actualizador conectado a GitHub y descarga directa de WiiU_Transfer.wuhb.
"""

import base64
import json
import os
import queue
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser
from pathlib import Path

import webview

from transfer_server import (
    TCP_PORT,
    UDP_DISCOVERY_PORT,
    TransferServer,
    format_bytes,
    get_local_ip,
    scan_packages,
)
from ui_assets import ensure_pc_assets

APP_VERSION = "2.0.2"
GITHUB_REPO_URL = "https://github.com/supermafari007/WiiU-Transfer-Tools"
VERSION_JSON_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/version.json"
RAW_PC_BASE_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/PC"
DEFAULT_WUHB_URL = "https://github.com/supermafari007/WiiU-Transfer-Tools/releases/download/v2.0.2/WiiU_Transfer.wuhb"


def open_in_explorer(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    if sys.platform.startswith("win"):
        os.startfile(str(path))  # type: ignore[attr-defined]
    elif sys.platform == "darwin":
        subprocess.Popen(["open", str(path)])
    else:
        subprocess.Popen(["xdg-open", str(path)])


def is_version_newer(current_ver: str, remote_ver: str) -> bool:
    try:
        c_parts = [int(x) for x in current_ver.strip().lstrip("vV").split(".")]
        r_parts = [int(x) for x in remote_ver.strip().lstrip("vV").split(".")]
        max_len = max(len(c_parts), len(r_parts))
        c_parts += [0] * (max_len - len(c_parts))
        r_parts += [0] * (max_len - len(r_parts))
        return r_parts > c_parts
    except Exception:
        return False


HTML_UI = r"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Wii U Transfer Tools</title>
<style>
  :root {
    --bg: #0f141b;
    --header: #0b3954;
    --card: #18202b;
    --card-soft: #121922;
    --border: #2e3d50;
    --cyan: #00aeef;
    --cyan-dk: #0086b8;
    --cyan-hi: #4dd4ff;
    --cyan-sub: #a8d8ec;
    --green: #17a95b;
    --green-hi: #2fe082;
    --red: #d92b3a;
    --amber-bg: #3b280b;
    --amber-bd: #f59e0b;
    --amber-tx: #fcd34d;
    --text: #ecf3f9;
    --muted: #8ea3b8;
    --btn: #253141;
    --btn-hv: #314156;
    --track: #273445;
  }
  * { box-sizing: border-box; user-select: none; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: "Segoe UI", system-ui, -apple-system, sans-serif;
    display: flex;
    flex-direction: column;
    height: 100vh;
    overflow: hidden;
  }
  header {
    background: var(--header);
    border-bottom: 2px solid var(--cyan);
    padding: 12px 20px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 14px;
    flex-shrink: 0;
  }
  .brand { display: flex; align-items: center; gap: 14px; }
  .brand img { width: 52px; height: 40px; object-fit: contain; }
  .brand h1 { margin: 0; font-size: 20px; font-weight: 700; color: #fff; letter-spacing: 0.2px; }
  .brand p { margin: 2px 0 0; font-size: 12px; color: var(--cyan-sub); }
  .header-actions { display: flex; align-items: center; gap: 10px; }
  .btn-pill {
    background: #072638;
    border: 1px solid var(--cyan);
    color: var(--cyan-sub);
    border-radius: 999px;
    padding: 7px 14px;
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s ease;
  }
  .btn-pill:hover { background: #0f3b54; color: #fff; }
  .status-pill {
    display: flex;
    align-items: center;
    gap: 8px;
    background: #072638;
    color: #fff;
    border-radius: 999px;
    padding: 7px 16px;
    font-size: 12.5px;
    font-weight: 700;
    transition: background 0.2s ease;
  }
  .status-pill.online { background: var(--green); }
  .status-dot {
    width: 9px; height: 9px; border-radius: 50%;
    background: #8ea3b8;
  }
  .status-pill.online .status-dot {
    background: #b4ffd2;
    box-shadow: 0 0 8px #b4ffd2;
  }
  #updateBanner {
    display: none;
    background: var(--amber-bg);
    border-bottom: 1px solid var(--amber-bd);
    padding: 8px 20px;
    align-items: center;
    justify-content: space-between;
    font-size: 13px;
    color: var(--amber-tx);
    font-weight: 600;
  }
  #updateBanner.show { display: flex; }
  .btn-amber {
    background: var(--amber-bd);
    color: #111827;
    border: none;
    border-radius: 8px;
    padding: 6px 14px;
    font-weight: 700;
    font-size: 12px;
    cursor: pointer;
  }
  main {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 14px 18px;
    min-height: 0;
  }
  .card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 14px;
    padding: 12px 14px;
  }
  .toolbar {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }
  .toolbar label { font-size: 13px; font-weight: 600; white-space: nowrap; }
  .folder-input {
    flex: 1;
    background: var(--card-soft);
    border: 1px solid var(--border);
    color: var(--text);
    border-radius: 8px;
    padding: 8px 10px;
    font-size: 12.5px;
    outline: none;
  }
  .btn {
    background: var(--btn);
    border: 1px solid #3a4d66;
    color: var(--text);
    border-radius: 8px;
    padding: 8px 13px;
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s ease, transform 0.08s ease;
    white-space: nowrap;
  }
  .btn:hover { background: var(--btn-hv); }
  .btn:active { transform: scale(0.98); }
  .btn-primary {
    background: var(--cyan);
    border-color: var(--cyan);
    color: #fff;
    padding: 8px 18px;
    font-weight: 700;
  }
  .btn-primary:hover { background: var(--cyan-dk); }
  .btn-danger {
    background: var(--red);
    border-color: var(--red);
    color: #fff;
    padding: 8px 18px;
    font-weight: 700;
  }
  .btn-danger:hover { background: #b51f2c; }
  .btn-green {
    background: var(--green);
    border-color: var(--green);
    color: #fff;
    font-weight: 700;
  }
  .btn-green:hover { background: #138a49; }

  .grid {
    flex: 1;
    display: grid;
    grid-template-columns: 1.65fr 1fr;
    gap: 12px;
    min-height: 0;
  }
  .panel {
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .panel-hdr {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 10px;
  }
  .panel-hdr h2 { margin: 0; font-size: 15px; font-weight: 700; }
  .panel-hdr span { font-size: 11.5px; color: var(--muted); }

  .table-wrap {
    flex: 1;
    overflow-y: auto;
    background: var(--card-soft);
    border: 1px solid var(--border);
    border-radius: 10px;
  }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th {
    position: sticky; top: 0;
    background: #202b3a;
    color: var(--text);
    text-align: left;
    padding: 9px 12px;
    font-size: 12px;
    font-weight: 700;
    border-bottom: 1px solid var(--border);
  }
  th.right, td.right { text-align: right; }
  th.center, td.center { text-align: center; }
  td {
    padding: 9px 12px;
    border-bottom: 1px solid #1e2938;
  }
  tbody tr { cursor: pointer; transition: background 0.12s; }
  tbody tr:hover { background: #162636; }
  .badge {
    display: inline-block;
    padding: 3px 10px;
    border-radius: 999px;
    font-size: 11.5px;
    font-weight: 700;
  }
  .badge-ready { background: #1b2a3b; color: var(--cyan-sub); }
  .badge-sending { background: #0d3a52; color: var(--cyan-hi); border: 1px solid var(--cyan); }
  .badge-done { background: #103a24; color: var(--green-hi); border: 1px solid var(--green); }
  .empty-state {
    padding: 38px 20px;
    text-align: center;
    color: var(--muted);
    font-size: 13px;
    line-height: 1.6;
  }

  .stat-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 5px 0;
    font-size: 13px;
  }
  .stat-row .k { color: var(--muted); }
  .stat-row .v { font-weight: 700; }
  .v-green { color: var(--green-hi); }
  .v-cyan { color: var(--cyan-hi); }

  .prog-box {
    margin: 8px 0;
    background: var(--card-soft);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 10px;
  }
  .prog-track {
    width: 100%;
    height: 12px;
    background: var(--track);
    border-radius: 999px;
    overflow: hidden;
  }
  .prog-fill {
    height: 100%;
    width: 0%;
    background: linear-gradient(90deg, var(--cyan), var(--cyan-hi));
    border-radius: 999px;
    transition: width 0.25s ease;
  }
  .prog-text {
    margin-top: 6px;
    font-size: 11.5px;
    color: var(--cyan-sub);
    font-weight: 600;
  }
  .guide-box {
    margin-top: auto;
    background: #112838;
    border: 1px solid #1e4b66;
    border-radius: 10px;
    padding: 11px 12px;
    font-size: 12.5px;
    line-height: 1.55;
  }
  .guide-box strong { color: var(--cyan-hi); display: block; margin-bottom: 4px; }

  .log-card {
    height: 124px;
    flex-shrink: 0;
    display: flex;
    flex-direction: column;
  }
  .log-card h3 { margin: 0 0 6px; font-size: 13px; font-weight: 700; }
  .log-body {
    flex: 1;
    overflow-y: auto;
    background: var(--card-soft);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 7px 10px;
    font-family: Consolas, "Cascadia Mono", monospace;
    font-size: 11.5px;
    line-height: 1.45;
    user-select: text;
  }
  .log-INFO { color: var(--text); }
  .log-OK { color: var(--green-hi); }
  .log-WARN { color: #f5b041; }
  .log-ERR { color: #ff5c6c; }

  /* Modal Acerca de */
  .modal-backdrop {
    position: fixed; inset: 0;
    background: rgba(5, 10, 16, 0.72);
    display: none;
    align-items: center;
    justify-content: center;
    z-index: 99;
  }
  .modal-backdrop.open { display: flex; }
  .modal {
    width: 560px;
    background: var(--card);
    border: 2px solid var(--cyan);
    border-radius: 16px;
    overflow: hidden;
    box-shadow: 0 20px 50px rgba(0,0,0,0.6);
  }
  .modal-hdr {
    background: var(--header);
    padding: 14px 18px;
    border-bottom: 2px solid var(--cyan);
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .modal-hdr img { width: 42px; height: 32px; }
  .modal-hdr h3 { margin: 0; font-size: 16px; color: #fff; }
  .modal-hdr small { color: var(--cyan-sub); font-size: 12px; }
  .modal-body { padding: 16px 18px; display: flex; flex-direction: column; gap: 12px; font-size: 13px; }
  .sub-card {
    background: var(--card-soft);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 12px;
  }
  .gh-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
  .upd-actions { display: flex; gap: 8px; margin-top: 10px; flex-wrap: wrap; }
  .modal-footer { padding: 0 18px 16px; display: flex; justify-content: flex-end; }
</style>
</head>
<body>
  <header>
    <div class="brand">
      <img id="hdrIcon" src="" alt="Wii U">
      <div>
        <h1>Wii U Transfer Tools</h1>
        <p id="hdrSub">Envía e instala juegos desde tu PC a la Wii U • Ver. 2.0.2 • Por El Diuca</p>
      </div>
    </div>
    <div class="header-actions">
      <button class="btn-pill" onclick="openAbout()">i &nbsp;Acerca de / GitHub</button>
      <div id="statusPill" class="status-pill">
        <span class="status-dot"></span>
        <span id="statusText">En espera</span>
      </div>
    </div>
  </header>

  <div id="updateBanner">
    <span id="updateBannerText">¡Nueva versión disponible en GitHub!</span>
    <button class="btn-amber" onclick="openAbout()">Actualizar ahora</button>
  </div>

  <main>
    <div class="card toolbar">
      <label>Carpeta de juegos:</label>
      <input id="folderInput" class="folder-input" readonly value="">
      <button class="btn" onclick="chooseFolder()">Cambiar carpeta...</button>
      <button class="btn" onclick="refreshPackages()">Actualizar</button>
      <button class="btn" onclick="openFolder()">Abrir carpeta</button>
      <button id="btnToggle" class="btn btn-primary" onclick="toggleServer()">Compartir Juegos</button>
    </div>

    <div class="grid">
      <div class="card panel">
        <div class="panel-hdr">
          <h2 id="pkgCountTitle">Juegos en tu PC (0)</h2>
          <span>Doble clic para abrir la carpeta del juego</span>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Nombre del Juego</th>
                <th class="right" style="width:110px;">Tamaño</th>
                <th class="center" style="width:175px;">Estado</th>
              </tr>
            </thead>
            <tbody id="pkgBody"></tbody>
          </table>
        </div>
      </div>

      <div class="card panel">
        <div class="panel-hdr">
          <h2>Conexión con tu Wii U</h2>
        </div>
        <div class="stat-row"><span class="k">Consola Wii U:</span><span id="stClient" class="v v-green">Esperando conexión...</span></div>
        <div class="stat-row"><span class="k">Enviando ahora:</span><span id="stGame" class="v v-cyan">Ninguno</span></div>

        <div class="prog-box">
          <div class="prog-track"><div id="progFill" class="prog-fill"></div></div>
          <div id="progDetail" class="prog-text">Enviado total: 0 B</div>
        </div>

        <div class="stat-row"><span class="k">Velocidad:</span><span id="stSpeed" class="v v-green">0.00 MB/s</span></div>
        <div class="stat-row"><span class="k">Red de este PC:</span><span id="stIp" class="v">Detectando...</span></div>

        <div class="guide-box">
          <strong>¿Cómo instalar en tu Wii U?</strong>
          1. Abre <b>Wii U Transfer Tools</b> en tu consola<br>
          2. Elige arriba <b>Modo Red</b> con el botón <b>[X]</b><br>
          3. Pulsa <b>[Y]</b> para buscar y <b>[A]</b> para instalar
        </div>
      </div>
    </div>

    <div class="card log-card">
      <h3>Estado y mensajes recientes</h3>
      <div id="logBox" class="log-body"></div>
    </div>
  </main>

  <div id="aboutModal" class="modal-backdrop" onclick="if(event.target===this) closeAbout()">
    <div class="modal">
      <div class="modal-hdr">
        <img id="modalIcon" src="" alt="Wii U">
        <div>
          <h3>Acerca de Wii U Transfer Tools</h3>
          <small id="modalVer">Versión 2.0.2 • Desarrollado por El Diuca (WebView2)</small>
        </div>
      </div>
      <div class="modal-body">
        <div class="sub-card" style="line-height:1.55;">
          Herramienta gratuita de código abierto para enviar e instalar juegos de Wii U directamente desde tu
          <b>PC (WebView2)</b>, <b>Celular Android 9+</b> o <b>Memoria USB (exFAT, FAT32, NTFS y FAT)</b>
          por Red Wi-Fi, adaptador LAN USB o cable USB, sin pasos técnicos.
        </div>
        <div class="sub-card gh-row">
          <div>
            <div style="font-size:11px; color:var(--muted); font-weight:700;">PROYECTO OFICIAL EN GITHUB</div>
            <div style="color:var(--cyan-hi); font-weight:700; margin-top:2px;">github.com/supermafari007/WiiU-Transfer-Tools</div>
          </div>
          <button class="btn" style="border-color:var(--cyan);" onclick="openGitHub()">Abrir en GitHub</button>
        </div>
        <div class="sub-card" style="border-color: var(--green);">
          <div id="updStatus" style="font-weight:700; color:var(--green-hi);">Programa al día (Ver. 2.0.2) • Actualizador conectado a GitHub</div>
          <div style="font-size:12px; color:var(--muted); margin-top:4px;">Actualiza este programa o descarga la última App para tu Wii U (.wuhb):</div>
          <div class="upd-actions">
            <button class="btn btn-green" onclick="updatePcFromGitHub()">Actualizar Programa PC</button>
            <button class="btn btn-primary" onclick="downloadWuhb()">Descargar App Wii U (.wuhb)</button>
            <button class="btn" onclick="checkUpdatesManual()">Comprobar</button>
          </div>
        </div>
      </div>
      <div class="modal-footer">
        <button class="btn" onclick="closeAbout()">Cerrar</button>
      </div>
    </div>
  </div>

<script>
  let initialized = false;

  function renderPackages(pkgs) {
    const body = document.getElementById('pkgBody');
    document.getElementById('pkgCountTitle').textContent = `Juegos en tu PC (${pkgs.length})`;
    if (!pkgs.length) {
      body.innerHTML = `<tr><td colspan="3" class="empty-state">No hay juegos en esta carpeta.<br>Copia tus carpetas de juegos aquí o pulsa <b>Cambiar carpeta...</b></td></tr>`;
      return;
    }
    body.innerHTML = pkgs.map(p => {
      let cls = 'badge-ready';
      if (p.status.startsWith('Enviando')) cls = 'badge-sending';
      else if (p.status.startsWith('Instalado')) cls = 'badge-done';
      const safeName = p.name.replace(/'/g, "\\'");
      return `<tr ondblclick="openPkg('${safeName}')">
        <td><b>${p.name}</b></td>
        <td class="right">${p.size_str}</td>
        <td class="center"><span class="badge ${cls}">${p.status}</span></td>
      </tr>`;
    }).join('');
  }

  function appendLogs(logs) {
    if (!logs || !logs.length) return;
    const box = document.getElementById('logBox');
    for (const [lvl, txt] of logs) {
      const div = document.createElement('div');
      div.className = 'log-' + lvl;
      div.textContent = txt;
      box.appendChild(div);
    }
    box.scrollTop = box.scrollHeight;
  }

  async function pollState() {
    if (!window.pywebview || !window.pywebview.api) return;
    try {
      const s = await window.pywebview.api.get_state();
      if (!initialized && s.icon_b64) {
        document.getElementById('hdrIcon').src = s.icon_b64;
        document.getElementById('modalIcon').src = s.icon_b64;
        initialized = true;
      }
      document.getElementById('folderInput').value = s.folder;
      document.getElementById('stIp').textContent = s.ip_label;
      document.getElementById('stClient').textContent = s.client_label;
      document.getElementById('stGame').textContent = s.current_game;
      document.getElementById('stSpeed').textContent = s.speed_str;
      document.getElementById('progFill').style.width = s.progress_pct + '%';
      document.getElementById('progDetail').textContent = s.progress_detail;

      const pill = document.getElementById('statusPill');
      const btn = document.getElementById('btnToggle');
      if (s.running) {
        pill.classList.add('online');
        document.getElementById('statusText').textContent = `Compartiendo con Wii U • ${s.ip_raw}`;
        btn.textContent = 'Detener Envío';
        btn.className = 'btn btn-danger';
      } else {
        pill.classList.remove('online');
        document.getElementById('statusText').textContent = 'En espera';
        btn.textContent = 'Compartir Juegos';
        btn.className = 'btn btn-primary';
      }

      if (s.update_available) {
        document.getElementById('updateBanner').classList.add('show');
        document.getElementById('updateBannerText').textContent = `¡Nueva versión v${s.remote_version} disponible en GitHub!`;
        document.getElementById('updStatus').textContent = `¡Nueva versión v${s.remote_version} disponible para descargar!`;
        document.getElementById('updStatus').style.color = '#fcd34d';
      }

      renderPackages(s.packages);
      appendLogs(s.new_logs);
    } catch (e) {}
  }

  async function toggleServer() {
    await window.pywebview.api.toggle_server();
    pollState();
  }
  async function chooseFolder() {
    await window.pywebview.api.choose_folder();
    pollState();
  }
  async function refreshPackages() {
    await window.pywebview.api.refresh_packages();
    pollState();
  }
  async function openFolder() { await window.pywebview.api.open_root_folder(); }
  async function openPkg(name) { await window.pywebview.api.open_pkg_folder(name); }
  async function openGitHub() { await window.pywebview.api.open_github(); }
  function openAbout() { document.getElementById('aboutModal').classList.add('open'); }
  function closeAbout() { document.getElementById('aboutModal').classList.remove('open'); }

  async function checkUpdatesManual() {
    const el = document.getElementById('updStatus');
    el.textContent = 'Consultando GitHub...';
    el.style.color = '#4dd4ff';
    const res = await window.pywebview.api.check_updates();
    el.textContent = res.message;
    el.style.color = res.newer ? '#fcd34d' : '#2fe082';
  }
  async function updatePcFromGitHub() {
    const el = document.getElementById('updStatus');
    el.textContent = 'Descargando última versión desde GitHub...';
    el.style.color = '#4dd4ff';
    const res = await window.pywebview.api.self_update_pc();
    el.textContent = res.message;
    el.style.color = res.ok ? '#2fe082' : '#ff5c6c';
  }
  async function downloadWuhb() {
    const el = document.getElementById('updStatus');
    el.textContent = 'Descargando WiiU_Transfer.wuhb...';
    el.style.color = '#4dd4ff';
    const res = await window.pywebview.api.download_wuhb();
    el.textContent = res.message;
    el.style.color = res.ok ? '#2fe082' : '#ff5c6c';
  }

  window.addEventListener('pywebviewready', () => {
    pollState();
    setInterval(pollState, 500);
  });
</script>
</body>
</html>
"""


class WebViewApi:
    def __init__(self) -> None:
        self.base_dir = Path(__file__).resolve().parent
        self.packages_dir = (self.base_dir / "packages").resolve()
        self.packages_dir.mkdir(parents=True, exist_ok=True)
        self.window = None

        _, header_icon_path = ensure_pc_assets(self.base_dir / "assets")
        raw_icon = header_icon_path.read_bytes()
        self._icon_b64 = "data:image/png;base64," + base64.b64encode(raw_icon).decode("ascii")

        self.log_queue: queue.Queue[tuple[str, str]] = queue.Queue()
        self.server = TransferServer(
            self.packages_dir,
            tcp_port=TCP_PORT,
            udp_port=UDP_DISCOVERY_PORT,
            on_event=self._enqueue_log,
        )
        self._last_stat_time = time.monotonic()
        self._last_speed_mbs = 0.0
        self._cached_pkgs = scan_packages(self.packages_dir)

        self._remote_version = APP_VERSION
        self._remote_wuhb_url = DEFAULT_WUHB_URL
        self._update_available = False

        self._enqueue_log(
            "INFO",
            f"Wii U Transfer Tools v{APP_VERSION} (WebView2) listo. Pon tus juegos en la carpeta y pulsa 'Compartir Juegos'.",
        )
        threading.Thread(target=self._check_updates_worker, daemon=True).start()

    def _enqueue_log(self, level: str, message: str) -> None:
        ts = time.strftime("%H:%M:%S")
        self.log_queue.put((level, f"[{ts}] {message}"))

    def _check_updates_worker(self) -> dict:
        try:
            req = urllib.request.Request(
                f"{VERSION_JSON_URL}?t={int(time.time())}",
                headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
            )
            with urllib.request.urlopen(req, timeout=6) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            rem = str(data.get("version", APP_VERSION)).strip()
            wuhb = str(data.get("wiiu_wuhb_url", DEFAULT_WUHB_URL)).strip()
            newer = is_version_newer(APP_VERSION, rem)
            self._remote_version = rem
            self._remote_wuhb_url = wuhb or DEFAULT_WUHB_URL
            self._update_available = newer
            if newer:
                self._enqueue_log("OK", f"Nueva versión v{rem} disponible en GitHub.")
                return {"newer": True, "message": f"¡Nueva versión v{rem} disponible en GitHub!"}
            return {"newer": False, "message": f"Programa al día (Ver. {APP_VERSION})."}
        except Exception as exc:
            return {"newer": False, "message": f"No se pudo consultar GitHub: {exc}"}

    def get_state(self) -> dict:
        now = time.monotonic()
        elapsed = now - self._last_stat_time
        stats = self.server.get_stats(reset_window=(elapsed >= 0.45))
        if elapsed >= 0.45:
            self._last_speed_mbs = (stats["bytes_sent_window"] / (1024.0 * 1024.0)) / max(elapsed, 0.001)
            self._last_stat_time = now

        ip = get_local_ip()
        clients = stats["active_clients"]
        cur_pkg = stats.get("current_pkg") or ""
        pkg_sent = stats.get("pkg_bytes_sent", 0)
        pkg_total = stats.get("pkg_total_bytes", 0)
        completed = stats.get("completed_pkgs", set())

        if cur_pkg and pkg_total > 0:
            pct = min(100, int((pkg_sent / float(pkg_total)) * 100))
            prog_detail = f"Enviado: {format_bytes(pkg_sent)} de {format_bytes(pkg_total)} ({pct}%)"
        else:
            pct = 0
            prog_detail = f"Enviado total: {format_bytes(stats['bytes_sent_total'])}"

        pkgs_out = []
        for p in self._cached_pkgs:
            name = p["name"]
            if name in completed:
                st = "Instalado en Wii U"
            elif name == cur_pkg and pkg_total > 0:
                st = f"Enviando a Wii U ({min(99, pct)}%)"
            else:
                st = "Listo para instalar" if p.get("has_tmd", True) else "Carpeta de archivos"
            pkgs_out.append({
                "name": name,
                "size_str": format_bytes(p["total_bytes"]),
                "status": st,
            })

        logs = []
        while True:
            try:
                logs.append(self.log_queue.get_nowait())
            except queue.Empty:
                break

        return {
            "icon_b64": self._icon_b64,
            "folder": str(self.packages_dir),
            "running": self.server.is_running,
            "ip_raw": ip,
            "ip_label": f"{ip} (Automática)",
            "client_label": f"Conectada ({clients[0]})" if clients else "Esperando conexión...",
            "current_game": cur_pkg if cur_pkg else "Ninguno",
            "speed_str": f"{self._last_speed_mbs:.2f} MB/s",
            "progress_pct": pct,
            "progress_detail": prog_detail,
            "packages": pkgs_out,
            "new_logs": logs,
            "update_available": self._update_available,
            "remote_version": self._remote_version,
        }

    def toggle_server(self) -> None:
        if self.server.is_running:
            self.server.stop()
        else:
            try:
                self.server.start()
            except OSError as exc:
                self._enqueue_log("ERR", f"No se pudo compartir en red: {exc}")

    def choose_folder(self) -> None:
        if not self.window:
            return
        res = self.window.create_file_dialog(
            webview.FOLDER_DIALOG, directory=str(self.packages_dir)
        )
        if res and len(res) > 0:
            self.packages_dir = Path(res[0]).resolve()
            self.server.set_root(self.packages_dir)
            self._cached_pkgs = scan_packages(self.packages_dir)

    def refresh_packages(self) -> None:
        self._cached_pkgs = scan_packages(self.packages_dir)
        self._enqueue_log("INFO", f"Lista actualizada: {len(self._cached_pkgs)} juego(s) encontrado(s).")

    def open_root_folder(self) -> None:
        open_in_explorer(self.packages_dir)

    def open_pkg_folder(self, name: str) -> None:
        target = self.packages_dir / name
        if target.exists():
            open_in_explorer(target)

    def open_github(self) -> None:
        webbrowser.open(GITHUB_REPO_URL)

    def check_updates(self) -> dict:
        return self._check_updates_worker()

    def self_update_pc(self) -> dict:
        files = ["transfer_server.py", "wiiu_transfer_gui.py", "ui_assets.py", "Instalar_WiiU_Transfer_Tools.py"]
        try:
            for fname in files:
                req = urllib.request.Request(
                    f"{RAW_PC_BASE_URL}/{fname}?t={int(time.time())}",
                    headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
                )
                with urllib.request.urlopen(req, timeout=12) as resp:
                    blob = resp.read()
                if len(blob) > 128:
                    (self.base_dir / fname).write_bytes(blob)
            self._enqueue_log("OK", "Programa PC actualizado desde GitHub. Reinicia la app para aplicar cambios.")
            return {"ok": True, "message": "¡Actualizado con éxito! Cierra y vuelve a abrir el programa."}
        except Exception as exc:
            return {"ok": False, "message": f"Error al actualizar: {exc}"}

    def download_wuhb(self) -> dict:
        if not self.window:
            return {"ok": False, "message": "Ventana no lista."}
        res = self.window.create_file_dialog(
            webview.SAVE_DIALOG,
            directory=str(self.base_dir),
            save_filename="WiiU_Transfer.wuhb",
            file_types=("Aplicación Wii U (*.wuhb)", "Todos (*.*)"),
        )
        if not res:
            return {"ok": False, "message": "Descarga cancelada."}
        save_path = Path(res[0] if isinstance(res, (list, tuple)) else res)
        try:
            req = urllib.request.Request(
                f"{self._remote_wuhb_url}?t={int(time.time())}",
                headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
            )
            with urllib.request.urlopen(req, timeout=25) as resp:
                blob = resp.read()
            save_path.write_bytes(blob)
            self._enqueue_log("OK", f"WiiU_Transfer.wuhb guardado en: {save_path}")
            return {"ok": True, "message": f"Guardado ({format_bytes(len(blob))}) en {save_path.name}"}
        except Exception as exc:
            return {"ok": False, "message": f"Error descargando .wuhb: {exc}"}

    def shutdown(self) -> None:
        self.server.stop()


def main() -> None:
    api = WebViewApi()
    win = webview.create_window(
        title=f"Wii U Transfer Tools v{APP_VERSION} — PC (WebView2)",
        html=HTML_UI,
        js_api=api,
        width=1080,
        height=730,
        min_size=(920, 630),
        background_color="#0F141B",
    )
    api.window = win
    win.events.closed += api.shutdown
    webview.start(gui="edgechromium" if sys.platform.startswith("win") else None, debug=False)


if __name__ == "__main__":
    main()
