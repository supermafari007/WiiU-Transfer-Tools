#!/usr/bin/env python3
"""
Instalador Gráfico Oficial para Windows (WebView2) - Wii U Transfer Tools v2.0.2 (por El Diuca)
Instala el programa WebView2 para PC, genera el icono oficial .ICO, crea accesos directos
en el Escritorio y Menú Inicio (sin ventana negra CMD), configura las reglas del Firewall
de Windows para la Wii U y crea un desinstalador limpio.
"""

import base64
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image
import webview

from ui_assets import ensure_pc_assets

APP_VERSION = "2.0.2"


def get_pythonw_executable() -> Path:
    py_exe = Path(sys.executable).resolve()
    pyw = py_exe.with_name("pythonw.exe")
    return pyw if pyw.exists() else py_exe


def ensure_ico_file(assets_dir: Path) -> Path:
    app_png, _ = ensure_pc_assets(assets_dir)
    ico_path = assets_dir / "wiiu_transfer.ico"
    img = Image.open(app_png).convert("RGBA")
    img.save(
        ico_path,
        format="ICO",
        sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)],
    )
    return ico_path


def create_windows_shortcut(
    lnk_path: Path,
    target_exe: Path,
    arguments: str,
    working_dir: Path,
    icon_path: Path,
    description: str,
) -> bool:
    ps_script = (
        "$ws = New-Object -ComObject WScript.Shell; "
        f"$s = $ws.CreateShortcut('{str(lnk_path)}'); "
        f"$s.TargetPath = '{str(target_exe)}'; "
        f"$s.Arguments = '{arguments}'; "
        f"$s.WorkingDirectory = '{str(working_dir)}'; "
        f"$s.IconLocation = '{str(icon_path)},0'; "
        f"$s.Description = '{description}'; "
        "$s.Save()"
    )
    res = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
        capture_output=True,
        text=True,
    )
    return res.returncode == 0 and lnk_path.exists()


INSTALLER_HTML = r"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<style>
  * { box-sizing: border-box; user-select: none; }
  body {
    margin: 0; background: #0f141b; color: #ecf3f9;
    font-family: "Segoe UI", system-ui, sans-serif;
    display: flex; flex-direction: column; height: 100vh;
  }
  header {
    background: #0b3954; border-bottom: 2px solid #00aeef;
    padding: 14px 18px; display: flex; align-items: center; gap: 12px;
  }
  header img { width: 46px; height: 35px; }
  header h1 { margin: 0; font-size: 16px; color: #fff; }
  header p { margin: 2px 0 0; font-size: 12px; color: #a8d8ec; }
  main { flex: 1; padding: 16px 18px; display: flex; flex-direction: column; gap: 12px; }
  .card {
    background: #18202b; border: 1px solid #2e3d50; border-radius: 12px; padding: 12px 14px;
  }
  .label { font-size: 12.5px; font-weight: 700; margin-bottom: 7px; color: #4dd4ff; }
  .row { display: flex; gap: 8px; }
  input[type="text"] {
    flex: 1; background: #121922; border: 1px solid #2e3d50; color: #ecf3f9;
    border-radius: 8px; padding: 8px 10px; font-size: 12.5px; outline: none;
  }
  label.chk {
    display: flex; align-items: center; gap: 9px; font-size: 13px; padding: 4px 0; cursor: pointer;
  }
  input[type="checkbox"] { accent-color: #00aeef; width: 16px; height: 16px; }
  .track { height: 12px; background: #273445; border-radius: 999px; overflow: hidden; margin-top: 8px; }
  .fill { height: 100%; width: 0%; background: #17a95b; transition: width 0.25s; }
  .status { font-size: 12.5px; font-weight: 600; color: #ecf3f9; }
  .actions { display: flex; justify-content: space-between; margin-top: auto; }
  .btn {
    background: #253141; border: 1px solid #3a4d66; color: #ecf3f9;
    border-radius: 9px; padding: 9px 18px; font-size: 13px; font-weight: 700; cursor: pointer;
  }
  .btn:hover { background: #314156; }
  .btn-green { background: #17a95b; border-color: #17a95b; color: #fff; }
  .btn-green:hover { background: #138a49; }
</style>
</head>
<body>
  <header>
    <img id="icon" src="" alt="">
    <div>
      <h1>Instalar Wii U Transfer Tools en tu PC</h1>
      <p>Versión 2.0.2 (WebView2) • Desarrollado por El Diuca</p>
    </div>
  </header>
  <main>
    <div class="card">
      <div class="label">Carpeta de instalación:</div>
      <div class="row">
        <input id="dir" type="text" value="">
        <button class="btn" onclick="pickDir()">Cambiar...</button>
      </div>
    </div>
    <div class="card">
      <div class="label">Opciones del instalador:</div>
      <label class="chk"><input id="cDesktop" type="checkbox" checked> Crear acceso directo en el Escritorio (sin ventana negra CMD)</label>
      <label class="chk"><input id="cStart" type="checkbox" checked> Agregar al Menú Inicio de Windows</label>
      <label class="chk"><input id="cFw" type="checkbox" checked> Permitir conexión automática con la Wii U en el Firewall</label>
      <label class="chk"><input id="cLaunch" type="checkbox" checked> Abrir Wii U Transfer Tools al finalizar</label>
    </div>
    <div class="card">
      <div id="status" class="status">Listo para instalar en tu PC.</div>
      <div class="track"><div id="fill" class="fill"></div></div>
    </div>
    <div class="actions">
      <button class="btn" onclick="window.pywebview.api.cancel()">Cancelar</button>
      <button id="btnGo" class="btn btn-green" onclick="doInstall()">Instalar Ahora</button>
    </div>
  </main>
<script>
  window.addEventListener('pywebviewready', async () => {
    const init = await window.pywebview.api.get_init();
    document.getElementById('icon').src = init.icon_b64;
    document.getElementById('dir').value = init.default_dir;
  });
  async function pickDir() {
    const p = await window.pywebview.api.choose_dir(document.getElementById('dir').value);
    if (p) document.getElementById('dir').value = p;
  }
  async function doInstall() {
    document.getElementById('btnGo').disabled = true;
    document.getElementById('status').textContent = 'Instalando archivos y creando accesos directos...';
    document.getElementById('fill').style.width = '55%';
    const res = await window.pywebview.api.run_install({
      dest: document.getElementById('dir').value,
      desktop: document.getElementById('cDesktop').checked,
      startmenu: document.getElementById('cStart').checked,
      firewall: document.getElementById('cFw').checked,
      launch: document.getElementById('cLaunch').checked,
    });
    document.getElementById('fill').style.width = res.ok ? '100%' : '0%';
    document.getElementById('status').textContent = res.message;
    document.getElementById('status').style.color = res.ok ? '#2fe082' : '#ff5c6c';
    if (!res.ok) document.getElementById('btnGo').disabled = false;
  }
</script>
</body>
</html>
"""


class InstallerApi:
    def __init__(self) -> None:
        self.src_dir = Path(__file__).resolve().parent
        self.window = None
        _, hdr_png = ensure_pc_assets(self.src_dir / "assets")
        self._icon_b64 = "data:image/png;base64," + base64.b64encode(hdr_png.read_bytes()).decode("ascii")
        self.default_dir = str(
            Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "Programs" / "WiiU_Transfer_Tools"
        )

    def get_init(self) -> dict:
        return {"icon_b64": self._icon_b64, "default_dir": self.default_dir}

    def choose_dir(self, current: str) -> str:
        if not self.window:
            return current
        res = self.window.create_file_dialog(webview.FOLDER_DIALOG, directory=current)
        if res and len(res) > 0:
            return str(Path(res[0]) / "WiiU_Transfer_Tools")
        return current

    def cancel(self) -> None:
        if self.window:
            self.window.destroy()

    def run_install(self, opts: dict) -> dict:
        try:
            dest = Path(opts["dest"]).resolve()
            dest.mkdir(parents=True, exist_ok=True)
            (dest / "packages").mkdir(parents=True, exist_ok=True)
            (dest / "assets").mkdir(parents=True, exist_ok=True)

            for fname in (
                "wiiu_transfer_gui.py",
                "transfer_server.py",
                "ui_assets.py",
                "Instalar_WiiU_Transfer_Tools.py",
                "requirements.txt",
            ):
                src_f = self.src_dir / fname
                if src_f.exists() and src_f.resolve() != (dest / fname).resolve():
                    shutil.copy2(src_f, dest / fname)

            ico_path = ensure_ico_file(dest / "assets")
            pyw_exe = get_pythonw_executable()
            gui_script = dest / "wiiu_transfer_gui.py"

            (dest / "Iniciar_WiiU_Transfer_Tools.bat").write_text(
                f'@echo off\r\ncd /d "%~dp0"\r\nstart "" "{pyw_exe}" "{gui_script}"\r\n',
                encoding="utf-8",
            )

            if opts.get("desktop", True):
                desktop_dir = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"
                if not desktop_dir.exists():
                    desktop_dir = Path.home() / "Desktop"
                create_windows_shortcut(
                    desktop_dir / "Wii U Transfer Tools.lnk",
                    pyw_exe,
                    f'"{gui_script}"',
                    dest,
                    ico_path,
                    "Enviar e instalar juegos en tu Wii U (por El Diuca)",
                )

            if opts.get("startmenu", True):
                start_menu = (
                    Path(os.environ.get("APPDATA", ""))
                    / "Microsoft"
                    / "Windows"
                    / "Start Menu"
                    / "Programs"
                )
                if start_menu.exists():
                    create_windows_shortcut(
                        start_menu / "Wii U Transfer Tools.lnk",
                        pyw_exe,
                        f'"{gui_script}"',
                        dest,
                        ico_path,
                        "Enviar e instalar juegos en tu Wii U (por El Diuca)",
                    )

            if opts.get("firewall", True):
                for proto, port in (("TCP", 7331), ("UDP", 7332)):
                    subprocess.run(
                        [
                            "netsh", "advfirewall", "firewall", "add", "rule",
                            f"name=Wii U Transfer Tools ({proto} {port})",
                            "dir=in", "action=allow", f"protocol={proto}", f"localport={port}",
                        ],
                        capture_output=True,
                    )

            (dest / "Desinstalar_WiiU_Transfer_Tools.bat").write_text(
                "@echo off\r\n"
                'del /f /q "%USERPROFILE%\\Desktop\\Wii U Transfer Tools.lnk" 2>nul\r\n'
                'del /f /q "%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Wii U Transfer Tools.lnk" 2>nul\r\n'
                "echo Accesos directos eliminados.\r\npause\r\n",
                encoding="utf-8",
            )

            if opts.get("launch", True):
                subprocess.Popen([str(pyw_exe), str(gui_script)], cwd=str(dest))

            return {"ok": True, "message": f"¡Instalado con éxito en {dest}!"}
        except Exception as exc:
            return {"ok": False, "message": f"Error: {exc}"}


def main() -> None:
    api = InstallerApi()
    win = webview.create_window(
        title=f"Instalador — Wii U Transfer Tools v{APP_VERSION}",
        html=INSTALLER_HTML,
        js_api=api,
        width=580,
        height=500,
        resizable=False,
        background_color="#0F141B",
    )
    api.window = win
    webview.start(gui="edgechromium" if sys.platform.startswith("win") else None, debug=False)


if __name__ == "__main__":
    main()
