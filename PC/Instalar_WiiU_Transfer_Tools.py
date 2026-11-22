#!/usr/bin/env python3
"""
Instalador y Desinstalador Oficial en un solo .EXE para Windows
Wii U Transfer Tools v2.0.2 (por El Diuca)
"""

import os
import shutil
import subprocess
import sys
import tkinter as tk
import winreg
from pathlib import Path
from tkinter import filedialog, messagebox

from PIL import Image, ImageTk

from ui_assets import ensure_pc_assets
from wiiu_transfer_gui import (
    APP_VERSION,
    COL_BG,
    COL_BORDER,
    COL_BTN_BG,
    COL_BTN_HV,
    COL_CARD,
    COL_CARD_SOFT,
    COL_CYAN,
    COL_CYAN_HI,
    COL_GREEN,
    COL_GREEN_HI,
    COL_GREEN_HV,
    COL_HEADER,
    COL_TEXT,
    COL_TRACK,
    RoundedButton,
    RoundedCard,
    apply_dark_titlebar,
)

UNINSTALL_REG_PATH = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\WiiUTransferTools"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def _silent_run(cmd: list[str]) -> subprocess.CompletedProcess:
    si = None
    if os.name == "nt":
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        creationflags=NO_WINDOW,
        startupinfo=si,
    )


def get_bundle_dir() -> Path:
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return Path(sys._MEIPASS)
    return Path(__file__).resolve().parent


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
    lnk_path.parent.mkdir(parents=True, exist_ok=True)
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
    res = _silent_run(
        ["powershell", "-NoProfile", "-WindowStyle", "Hidden", "-ExecutionPolicy", "Bypass", "-Command", ps_script]
    )
    return res.returncode == 0 and lnk_path.exists()


def register_windows_uninstaller(install_dir: Path, uninstaller_exe: Path, icon_path: Path) -> None:
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, UNINSTALL_REG_PATH) as key:
            winreg.SetValueEx(key, "DisplayName", 0, winreg.REG_SZ, "Wii U Transfer Tools")
            winreg.SetValueEx(key, "DisplayVersion", 0, winreg.REG_SZ, APP_VERSION)
            winreg.SetValueEx(key, "Publisher", 0, winreg.REG_SZ, "El Diuca")
            winreg.SetValueEx(key, "URLInfoAbout", 0, winreg.REG_SZ, "https://github.com/supermafari007/WiiU-Transfer-Tools")
            winreg.SetValueEx(key, "InstallLocation", 0, winreg.REG_SZ, str(install_dir))
            winreg.SetValueEx(key, "DisplayIcon", 0, winreg.REG_SZ, f"{icon_path},0")
            winreg.SetValueEx(key, "UninstallString", 0, winreg.REG_SZ, f'"{uninstaller_exe}" --uninstall')
            winreg.SetValueEx(key, "NoModify", 0, winreg.REG_DWORD, 1)
            winreg.SetValueEx(key, "NoRepair", 0, winreg.REG_DWORD, 1)
    except Exception:
        pass


def run_uninstall_flow() -> None:
    root = tk.Tk()
    root.withdraw()
    if not messagebox.askyesno(
        "Desinstalar Wii U Transfer Tools",
        "¿Deseas desinstalar Wii U Transfer Tools de tu PC?\n\n"
        "(Se eliminarán el programa, los accesos directos y la entrada en Windows; "
        "si tienes juegos dentro de 'packages', esos archivos no se borrarán.)",
    ):
        root.destroy()
        return

    install_dir = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent

    # Cerrar el programa si estuviera abierto
    _silent_run(["taskkill", "/F", "/IM", "WiiU_Transfer_Tools.exe"])

    # Eliminar accesos directos
    desktop_lnk = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop" / "Wii U Transfer Tools.lnk"
    if desktop_lnk.exists():
        try:
            desktop_lnk.unlink()
        except Exception:
            pass

    start_progs = Path(os.environ.get("APPDATA", "")) / "Microsoft" / "Windows" / "Start Menu" / "Programs"
    for p in (start_progs / "Wii U Transfer Tools.lnk", start_progs / "Wii U Transfer Tools"):
        try:
            if p.is_file():
                p.unlink()
            elif p.is_dir():
                shutil.rmtree(p, ignore_errors=True)
        except Exception:
            pass

    # Eliminar entrada de "Aplicaciones instaladas" de Windows
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, UNINSTALL_REG_PATH)
    except Exception:
        pass

    # Eliminar binario principal y assets
    for fname in ("WiiU_Transfer_Tools.exe", "Iniciar_WiiU_Transfer_Tools.bat"):
        fp = install_dir / fname
        if fp.exists():
            try:
                fp.unlink()
            except Exception:
                pass
    assets_d = install_dir / "assets"
    if assets_d.exists():
        shutil.rmtree(assets_d, ignore_errors=True)

    # Programar borrado del propio desinstalador al cerrar
    if getattr(sys, "frozen", False):
        self_exe = Path(sys.executable).resolve()
        subprocess.Popen(
            f'cmd /c ping 127.0.0.1 -n 3 >nul & del /f /q "{self_exe}" & rmdir "{install_dir}" 2>nul',
            shell=True,
            creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0,
        )

    messagebox.showinfo(
        "Desinstalación completada",
        "Wii U Transfer Tools ha sido desinstalado correctamente de tu PC.",
    )
    root.destroy()


class InstallerWizard:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(f"Instalador — Wii U Transfer Tools v{APP_VERSION}")
        self.root.geometry("600x585")
        self.root.minsize(580, 565)
        self.root.resizable(False, False)
        self.root.configure(bg=COL_BG)
        apply_dark_titlebar(self.root)

        self.bundle_dir = get_bundle_dir()
        default_dest = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "Programs" / "WiiU_Transfer_Tools"
        self.var_install_dir = tk.StringVar(value=str(default_dest))

        app_png, hdr_png = ensure_pc_assets(self.bundle_dir / "assets")
        self._app_icon_tk = ImageTk.PhotoImage(Image.open(app_png))
        self._hdr_icon_tk = ImageTk.PhotoImage(Image.open(hdr_png))
        try:
            self.root.iconphoto(True, self._app_icon_tk)
        except Exception:
            pass

        self.var_desktop = tk.BooleanVar(value=True)
        self.var_startmenu = tk.BooleanVar(value=True)
        self.var_firewall = tk.BooleanVar(value=True)
        self.var_status = tk.StringVar(value="Listo para instalar Wii U Transfer Tools en tu PC.")
        self._installing = False

        self._build_ui()

    def _build_ui(self) -> None:
        hdr = tk.Frame(self.root, bg=COL_HEADER)
        hdr.pack(fill="x")
        tk.Label(hdr, image=self._hdr_icon_tk, bg=COL_HEADER).pack(side="left", padx=(18, 12), pady=12)
        tbox = tk.Frame(hdr, bg=COL_HEADER)
        tbox.pack(side="left", pady=12)
        tk.Label(
            tbox,
            text="Instalar Wii U Transfer Tools",
            bg=COL_HEADER,
            fg="#FFFFFF",
            font=("Segoe UI", 15, "bold"),
        ).pack(anchor="w")
        tk.Label(
            tbox,
            text=f"Versión {APP_VERSION}  •  Desarrollado por El Diuca",
            bg=COL_HEADER,
            fg="#A8D8EC",
            font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", pady=(2, 0))
        tk.Frame(self.root, bg=COL_CYAN, height=2).pack(fill="x")

        body = tk.Frame(self.root, bg=COL_BG)
        body.pack(fill="both", expand=True, padx=18, pady=14)

        dir_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=10, pad=12, height=82)
        dir_card.pack(fill="x", pady=(0, 12))
        dir_card.pack_propagate(False)
        tk.Label(
            dir_card.inner,
            text="Carpeta donde se instalará el programa:",
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", pady=(0, 6))

        drow = tk.Frame(dir_card.inner, bg=COL_CARD)
        drow.pack(fill="x")
        tk.Entry(
            drow,
            textvariable=self.var_install_dir,
            bg=COL_CARD_SOFT,
            fg=COL_TEXT,
            insertbackground=COL_TEXT,
            relief="flat",
            highlightbackground=COL_BORDER,
            highlightthickness=1,
            font=("Segoe UI", 9),
        ).pack(side="left", fill="x", expand=True, ipady=5, padx=(0, 8))
        RoundedButton(drow, "Cambiar...", self._choose_dir, width=96, height=30).pack(side="right")

        opt_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=10, pad=14, height=160)
        opt_card.pack(fill="x", pady=(0, 12))
        opt_card.pack_propagate(False)
        tk.Label(
            opt_card.inner,
            text="Opciones de instalación:",
            bg=COL_CARD,
            fg=COL_CYAN_HI,
            font=("Segoe UI", 10, "bold"),
        ).pack(anchor="w", pady=(0, 6))

        self._add_checkbox(opt_card.inner, "Crear icono de acceso directo en el Escritorio", self.var_desktop)
        self._add_checkbox(opt_card.inner, "Agregar al Menú Inicio (con Desinstalador)", self.var_startmenu)
        self._add_checkbox(opt_card.inner, "Permitir conexión con la Wii U en el Firewall de Windows", self.var_firewall)

        prog_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=10, pad=12, height=66)
        prog_card.pack(fill="x", pady=(0, 14))
        prog_card.pack_propagate(False)

        self.lbl_status = tk.Label(
            prog_card.inner,
            textvariable=self.var_status,
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 9, "bold"),
        )
        self.lbl_status.pack(anchor="w", pady=(0, 6))

        self.prog_canvas = tk.Canvas(prog_card.inner, height=12, bg=COL_TRACK, highlightthickness=0, bd=0)
        self.prog_canvas.pack(fill="x")
        self._prog_fill = self.prog_canvas.create_rectangle(0, 0, 0, 12, fill=COL_GREEN, outline="")

        btn_bar = tk.Frame(body, bg=COL_BG)
        btn_bar.pack(fill="x")
        RoundedButton(
            btn_bar, "Cancelar", self.root.destroy, width=120, height=38, radius=9,
            bg_outer=COL_BG, fill=COL_BTN_BG, hover=COL_BTN_HV, border=COL_BORDER,
            fg=COL_TEXT, font=("Segoe UI", 10, "bold"),
        ).pack(side="left")

        RoundedButton(
            btn_bar, "Instalar Ahora", self._run_install, width=190, height=38, radius=9,
            bg_outer=COL_BG, fill=COL_GREEN, hover=COL_GREEN_HV, border=None,
            fg="#FFFFFF", font=("Segoe UI", 10, "bold"),
        ).pack(side="right")

    def _add_checkbox(self, parent, text: str, var: tk.BooleanVar) -> None:
        tk.Checkbutton(
            parent, text=text, variable=var, bg=COL_CARD, fg=COL_TEXT,
            activebackground=COL_CARD, activeforeground="#FFFFFF",
            selectcolor=COL_CARD_SOFT, font=("Segoe UI", 9),
            anchor="w", bd=0, highlightthickness=0, cursor="hand2",
        ).pack(fill="x", pady=2)

    def _choose_dir(self) -> None:
        chosen = filedialog.askdirectory(title="Seleccionar carpeta de instalación", initialdir=self.var_install_dir.get())
        if chosen:
            self.var_install_dir.set(str(Path(chosen) / "WiiU_Transfer_Tools"))

    def _set_progress(self, ratio: float, status: str) -> None:
        self.var_status.set(status)
        w = max(1, self.prog_canvas.winfo_width())
        self.prog_canvas.coords(self._prog_fill, 0, 0, int(w * ratio), 12)
        self.root.update_idletasks()

    def _run_install(self) -> None:
        if self._installing:
            return
        self._installing = True
        dest = Path(self.var_install_dir.get().strip()).resolve()
        try:
            self._set_progress(0.15, "Preparando carpeta de instalación...")
            dest.mkdir(parents=True, exist_ok=True)
            (dest / "packages").mkdir(parents=True, exist_ok=True)
            (dest / "assets").mkdir(parents=True, exist_ok=True)

            # Cerrar instancia anterior si estuviera abierta (sin mostrar consola)
            _silent_run(["taskkill", "/F", "/IM", "WiiU_Transfer_Tools.exe"])

            self._set_progress(0.35, "Instalando Wii U Transfer Tools...")
            embedded_app = self.bundle_dir / "payload" / "WiiU_Transfer_Tools.exe"
            installed_app = dest / "WiiU_Transfer_Tools.exe"
            if embedded_app.exists():
                shutil.copy2(embedded_app, installed_app)
            else:
                raise FileNotFoundError(f"No se encontró el binario de la aplicación en {embedded_app}")

            ico_path = ensure_ico_file(dest / "assets")

            self._set_progress(0.55, "Registrando Desinstalador en Windows...")
            uninst_exe = dest / "Desinstalar_WiiU_Transfer_Tools.exe"
            if getattr(sys, "frozen", False):
                self_exe = Path(sys.executable).resolve()
                if self_exe != uninst_exe.resolve():
                    shutil.copy2(self_exe, uninst_exe)
                register_windows_uninstaller(dest, uninst_exe, ico_path)

            if self.var_desktop.get():
                self._set_progress(0.70, "Creando icono en el Escritorio...")
                desktop_dir = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"
                if not desktop_dir.exists():
                    desktop_dir = Path.home() / "Desktop"
                create_windows_shortcut(
                    desktop_dir / "Wii U Transfer Tools.lnk",
                    installed_app, "", dest, ico_path,
                    "Enviar e instalar juegos en tu Wii U (por El Diuca)",
                )

            if self.var_startmenu.get():
                self._set_progress(0.82, "Agregando al Menú Inicio...")
                start_folder = (
                    Path(os.environ.get("APPDATA", ""))
                    / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Wii U Transfer Tools"
                )
                create_windows_shortcut(
                    start_folder / "Wii U Transfer Tools.lnk",
                    installed_app, "", dest, ico_path,
                    "Enviar e instalar juegos en tu Wii U (por El Diuca)",
                )
                if uninst_exe.exists():
                    create_windows_shortcut(
                        start_folder / "Desinstalar Wii U Transfer Tools.lnk",
                        uninst_exe, "--uninstall", dest, ico_path,
                        "Desinstalar Wii U Transfer Tools de tu PC",
                    )

            if self.var_firewall.get():
                self._set_progress(0.92, "Configurando Firewall de Windows...")
                for proto, port in (("TCP", 7331), ("UDP", 7332)):
                    _silent_run(
                        [
                            "netsh", "advfirewall", "firewall", "add", "rule",
                            f"name=Wii U Transfer Tools ({proto} {port})",
                            "dir=in", "action=allow", f"protocol={proto}", f"localport={port}",
                        ]
                    )

            self._set_progress(1.0, "¡Instalación completada!")
            self.lbl_status.configure(fg=COL_GREEN_HI)
            self.root.update()

            lanzar = messagebox.askyesno(
                "Instalación completada",
                f"Wii U Transfer Tools v{APP_VERSION} se instaló correctamente.\n\n"
                "Ya tienes el icono en el Escritorio y Menú Inicio, y puedes desinstalarlo "
                "cuando quieras desde Aplicaciones instaladas de Windows.\n\n"
                "¿Deseas abrir Wii U Transfer Tools ahora?",
                parent=self.root,
            )
            self.root.destroy()
            if lanzar:
                subprocess.Popen([str(installed_app)], cwd=str(dest))
        except Exception as exc:
            self._installing = False
            self.lbl_status.configure(fg="#FF5C6C")
            self.var_status.set(f"Error: {exc}")
            messagebox.showerror("Error de instalación", str(exc), parent=self.root)


def main() -> None:
    if "--uninstall" in sys.argv:
        run_uninstall_flow()
        return
    root = tk.Tk()
    InstallerWizard(root)
    root.mainloop()


if __name__ == "__main__":
    main()
