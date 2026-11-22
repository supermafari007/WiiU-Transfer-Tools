#!/usr/bin/env python3
"""
Instalador Gráfico Oficial para Windows - Wii U Transfer Tools v2.0.0 (por El Diuca)
Instala el programa para PC, genera el icono oficial .ICO, crea accesos directos en el
Escritorio y Menú Inicio (usando pythonw.exe sin ventana negra de consola), configura
las reglas del Firewall de Windows para la Wii U y genera un desinstalador limpio.
"""

import ctypes
import os
import shutil
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox

from PIL import Image, ImageTk

from ui_assets import ensure_pc_assets, make_rounded_rect_img
from wiiu_transfer_gui import (
    APP_VERSION,
    COL_BG,
    COL_BORDER,
    COL_BTN_BG,
    COL_BTN_HV,
    COL_CARD,
    COL_CARD_SOFT,
    COL_CYAN,
    COL_CYAN_DK,
    COL_CYAN_HI,
    COL_GREEN,
    COL_GREEN_HI,
    COL_GREEN_HV,
    COL_HEADER,
    COL_MUTED,
    COL_TEXT,
    COL_TRACK,
    RoundedButton,
    RoundedCard,
    apply_dark_titlebar,
    hex_to_rgba,
)


def get_pythonw_executable() -> Path:
    py_exe = Path(sys.executable).resolve()
    pyw = py_exe.with_name("pythonw.exe")
    if pyw.exists():
        return pyw
    return py_exe


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


class InstallerWizard:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(f"Instalador — Wii U Transfer Tools v{APP_VERSION} (PC)")
        self.root.geometry("560x510")
        self.root.resizable(False, False)
        self.root.configure(bg=COL_BG)
        apply_dark_titlebar(self.root)

        self.src_dir = Path(__file__).resolve().parent
        default_dest = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "Programs" / "WiiU_Transfer_Tools"
        self.var_install_dir = tk.StringVar(value=str(default_dest))

        app_png, hdr_png = ensure_pc_assets(self.src_dir / "assets")
        self._app_icon_tk = ImageTk.PhotoImage(Image.open(app_png))
        self._hdr_icon_tk = ImageTk.PhotoImage(Image.open(hdr_png))
        try:
            self.root.iconphoto(True, self._app_icon_tk)
        except Exception:
            pass

        self.var_desktop = tk.BooleanVar(value=True)
        self.var_startmenu = tk.BooleanVar(value=True)
        self.var_firewall = tk.BooleanVar(value=True)
        self.var_launch = tk.BooleanVar(value=True)
        self.var_status = tk.StringVar(value="Listo para instalar en tu PC.")

        self._build_ui()

    def _build_ui(self) -> None:
        # Header Oscuro con icono oficial
        hdr = tk.Frame(self.root, bg=COL_HEADER)
        hdr.pack(fill="x")
        tk.Label(hdr, image=self._hdr_icon_tk, bg=COL_HEADER).pack(side="left", padx=(18, 12), pady=12)
        tbox = tk.Frame(hdr, bg=COL_HEADER)
        tbox.pack(side="left", pady=12)
        tk.Label(
            tbox,
            text="Instalar Wii U Transfer Tools en tu PC",
            bg=COL_HEADER,
            fg="#FFFFFF",
            font=("Segoe UI", 14, "bold"),
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

        # Carpeta de destino
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
        entry = tk.Entry(
            drow,
            textvariable=self.var_install_dir,
            bg=COL_CARD_SOFT,
            fg=COL_TEXT,
            insertbackground=COL_TEXT,
            relief="flat",
            highlightbackground=COL_BORDER,
            highlightthickness=1,
            font=("Segoe UI", 9),
        )
        entry.pack(side="left", fill="x", expand=True, ipady=5, padx=(0, 8))
        RoundedButton(
            drow, "Cambiar...", self._choose_dir, width=96, height=30
        ).pack(side="right")

        # Opciones de instalación
        opt_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=10, pad=14, height=188)
        opt_card.pack(fill="x", pady=(0, 12))
        opt_card.pack_propagate(False)

        tk.Label(
            opt_card.inner,
            text="Opciones del instalador:",
            bg=COL_CARD,
            fg=COL_CYAN_HI,
            font=("Segoe UI", 10, "bold"),
        ).pack(anchor="w", pady=(0, 6))

        self._add_checkbox(
            opt_card.inner,
            "Crear icono en el Escritorio (abre directo sin ventana negra CMD)",
            self.var_desktop,
        )
        self._add_checkbox(
            opt_card.inner,
            "Agregar al Menú Inicio de Windows",
            self.var_startmenu,
        )
        self._add_checkbox(
            opt_card.inner,
            "Permitir conexión automática con la Wii U en el Firewall de Windows",
            self.var_firewall,
        )
        self._add_checkbox(
            opt_card.inner,
            "Abrir Wii U Transfer Tools al finalizar la instalación",
            self.var_launch,
        )

        # Barra de progreso
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

        self.prog_canvas = tk.Canvas(
            prog_card.inner, height=12, bg=COL_TRACK, highlightthickness=0, bd=0
        )
        self.prog_canvas.pack(fill="x")
        self._prog_fill = self.prog_canvas.create_rectangle(0, 0, 0, 12, fill=COL_GREEN, outline="")

        # Botones inferiores
        btn_bar = tk.Frame(body, bg=COL_BG)
        btn_bar.pack(fill="x")

        RoundedButton(
            btn_bar,
            text="Cancelar",
            command=self.root.destroy,
            width=120,
            height=38,
            radius=9,
            bg_outer=COL_BG,
            fill=COL_BTN_BG,
            hover=COL_BTN_HV,
            border=COL_BORDER,
            fg=COL_TEXT,
            font=("Segoe UI", 10, "bold"),
        ).pack(side="left")

        self.btn_install = RoundedButton(
            btn_bar,
            text="Instalar Ahora",
            command=self._run_install,
            width=190,
            height=38,
            radius=9,
            bg_outer=COL_BG,
            fill=COL_GREEN,
            hover=COL_GREEN_HV,
            border=None,
            fg="#FFFFFF",
            font=("Segoe UI", 10, "bold"),
        )
        self.btn_install.pack(side="right")

    def _add_checkbox(self, parent, text: str, var: tk.BooleanVar) -> None:
        cb = tk.Checkbutton(
            parent,
            text=text,
            variable=var,
            bg=COL_CARD,
            fg=COL_TEXT,
            activebackground=COL_CARD,
            activeforeground="#FFFFFF",
            selectcolor=COL_CARD_SOFT,
            font=("Segoe UI", 9),
            anchor="w",
            bd=0,
            highlightthickness=0,
            cursor="hand2",
        )
        cb.pack(fill="x", pady=2)

    def _choose_dir(self) -> None:
        chosen = filedialog.askdirectory(
            title="Seleccionar carpeta de instalación",
            initialdir=self.var_install_dir.get(),
        )
        if chosen:
            self.var_install_dir.set(str(Path(chosen) / "WiiU_Transfer_Tools"))

    def _set_progress(self, ratio: float, status: str) -> None:
        self.var_status.set(status)
        w = max(1, self.prog_canvas.winfo_width())
        self.prog_canvas.coords(self._prog_fill, 0, 0, int(w * ratio), 12)
        self.root.update_idletasks()

    def _run_install(self) -> None:
        dest = Path(self.var_install_dir.get().strip()).resolve()
        try:
            self._set_progress(0.15, "Creando carpeta de instalación y copiando archivos...")
            dest.mkdir(parents=True, exist_ok=True)
            (dest / "packages").mkdir(parents=True, exist_ok=True)
            (dest / "assets").mkdir(parents=True, exist_ok=True)

            files_to_copy = [
                "wiiu_transfer_gui.py",
                "transfer_server.py",
                "ui_assets.py",
                "Instalar_WiiU_Transfer_Tools.py",
                "requirements.txt",
            ]
            for fname in files_to_copy:
                src_f = self.src_dir / fname
                if src_f.exists() and src_f.resolve() != (dest / fname).resolve():
                    shutil.copy2(src_f, dest / fname)

            # Si hay paquetes en la carpeta origen y destino está vacío, copiarlos o mantener enlace
            src_pkgs = self.src_dir / "packages"
            if src_pkgs.exists() and src_pkgs.resolve() != (dest / "packages").resolve():
                for item in src_pkgs.iterdir():
                    target_item = dest / "packages" / item.name
                    if item.is_file() and not target_item.exists():
                        shutil.copy2(item, target_item)

            self._set_progress(0.40, "Generando icono oficial de alta resolución (.ICO)...")
            ico_path = ensure_ico_file(dest / "assets")
            pyw_exe = get_pythonw_executable()
            gui_script = dest / "wiiu_transfer_gui.py"

            # Crear lanzador sin consola en la carpeta instalada
            launcher_bat = dest / "Iniciar_WiiU_Transfer_Tools.bat"
            launcher_bat.write_text(
                "@echo off\r\n"
                "cd /d \"%~dp0\"\r\n"
                f"start \"\" \"{pyw_exe}\" \"{gui_script}\"\r\n",
                encoding="utf-8",
            )

            # Crear accesos directos
            if self.var_desktop.get():
                self._set_progress(0.60, "Creando acceso directo en el Escritorio...")
                desktop_dir = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"
                if not desktop_dir.exists():
                    desktop_dir = Path.home() / "Desktop"
                create_windows_shortcut(
                    lnk_path=desktop_dir / "Wii U Transfer Tools.lnk",
                    target_exe=pyw_exe,
                    arguments=f'"{gui_script}"',
                    working_dir=dest,
                    icon_path=ico_path,
                    description="Enviar e instalar juegos en tu Wii U (por El Diuca)",
                )

            if self.var_startmenu.get():
                self._set_progress(0.75, "Agregando acceso directo al Menú Inicio...")
                start_menu = (
                    Path(os.environ.get("APPDATA", ""))
                    / "Microsoft"
                    / "Windows"
                    / "Start Menu"
                    / "Programs"
                )
                if start_menu.exists():
                    create_windows_shortcut(
                        lnk_path=start_menu / "Wii U Transfer Tools.lnk",
                        target_exe=pyw_exe,
                        arguments=f'"{gui_script}"',
                        working_dir=dest,
                        icon_path=ico_path,
                        description="Enviar e instalar juegos en tu Wii U (por El Diuca)",
                    )

            if self.var_firewall.get():
                self._set_progress(0.88, "Configurando permiso para Wii U en el Firewall de Windows...")
                for proto, port in (("TCP", 7331), ("UDP", 7332)):
                    subprocess.run(
                        [
                            "netsh",
                            "advfirewall",
                            "firewall",
                            "add",
                            "rule",
                            f"name=Wii U Transfer Tools ({proto} {port})",
                            "dir=in",
                            "action=allow",
                            f"protocol={proto}",
                            f"localport={port}",
                        ],
                        capture_output=True,
                    )

            # Crear desinstalador
            uninst_bat = dest / "Desinstalar_WiiU_Transfer_Tools.bat"
            uninst_bat.write_text(
                "@echo off\r\n"
                "echo Eliminando accesos directos de Wii U Transfer Tools...\r\n"
                "del /f /q \"%USERPROFILE%\\Desktop\\Wii U Transfer Tools.lnk\" 2>nul\r\n"
                "del /f /q \"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Wii U Transfer Tools.lnk\" 2>nul\r\n"
                "echo Acceso directo eliminado. Puedes borrar esta carpeta si deseas quitar los juegos.\r\n"
                "pause\r\n",
                encoding="utf-8",
            )

            self._set_progress(1.0, "¡Instalación completada con éxito!")
            self.lbl_status.configure(fg=COL_GREEN_HI)

            if self.var_launch.get():
                subprocess.Popen([str(pyw_exe), str(gui_script)], cwd=str(dest))

            messagebox.showinfo(
                "Instalación completada",
                f"Wii U Transfer Tools v{APP_VERSION} se instaló correctamente en:\n{dest}\n\n"
                "Ya puedes abrirlo desde el icono de tu Escritorio o Menú Inicio.",
            )
            self.root.destroy()
        except Exception as exc:
            self.lbl_status.configure(fg="#FF5C6C")
            self.var_status.set(f"Error durante la instalación: {exc}")
            messagebox.showerror("Error de instalación", str(exc))


def main() -> None:
    root = tk.Tk()
    InstallerWizard(root)
    root.mainloop()


if __name__ == "__main__":
    main()
