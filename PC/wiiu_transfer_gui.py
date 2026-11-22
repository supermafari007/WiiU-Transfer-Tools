#!/usr/bin/env python3
"""
Wii U Transfer Tools v2.0.0 - Interfaz Gráfica para PC (Tkinter + Pillow)
Lenguaje 100% simple, barra de progreso de envío en vivo, verificador automático de
actualizaciones en GitHub y ventana modal "Acerca de Wii U Transfer Tools".
"""

import ctypes
import json
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
import urllib.request
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter import font as tkfont

from PIL import Image, ImageTk

from transfer_server import (
    TCP_PORT,
    UDP_DISCOVERY_PORT,
    TransferServer,
    format_bytes,
    get_local_ip,
    scan_packages,
)
from ui_assets import ensure_pc_assets, make_rounded_corners_9slice, make_rounded_rect_img

APP_VERSION = "2.0.2"
GITHUB_REPO_URL = "https://github.com/supermafari007/WiiU-Transfer-Tools"
VERSION_JSON_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/version.json"
RAW_PC_BASE_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/PC"
DEFAULT_WUHB_URL = "https://github.com/supermafari007/WiiU-Transfer-Tools/releases/download/v2.0.2/WiiU_Transfer.wuhb"
DEFAULT_PC_SETUP_URL = "https://github.com/supermafari007/WiiU-Transfer-Tools/releases/download/v2.0.2/WiiU_Transfer_Tools_Setup_v2.0.2.exe"

# Habilitar nitidez HiDPI en Windows
if sys.platform.startswith("win"):
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

# Paleta oscura oficial Wii U Transfer Tools (por El Diuca)
COL_BG        = "#0F141B"
COL_HEADER    = "#0B3954"
COL_CARD      = "#18202B"
COL_CARD_SOFT = "#121922"
COL_TIP_BG    = "#112838"
COL_TIP_BD    = "#1E4B66"
COL_BORDER    = "#2E3D50"
COL_CYAN      = "#00AEEF"
COL_CYAN_DK   = "#0086B8"
COL_CYAN_HI   = "#4DD4FF"
COL_GREEN     = "#17A95B"
COL_GREEN_HV  = "#138A49"
COL_GREEN_HI  = "#2FE082"
COL_RED       = "#D92B3A"
COL_RED_HV    = "#B51F2C"
COL_AMBER_BG  = "#3B280B"
COL_AMBER_BD  = "#F59E0B"
COL_AMBER_TX  = "#FCD34D"
COL_BTN_BG    = "#253141"
COL_BTN_HV    = "#314156"
COL_BTN_BD    = "#3A4D66"
COL_TEXT      = "#ECF3F9"
COL_MUTED     = "#8EA3B8"
COL_PILL_OFF  = "#072638"
COL_TRACK     = "#273445"


def hex_to_rgba(hex_color: str, alpha: int = 255) -> tuple[int, int, int, int]:
    h = hex_color.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), alpha)


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


def apply_dark_titlebar(win: tk.Tk | tk.Toplevel) -> None:
    if sys.platform.startswith("win"):
        try:
            win.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
            dark_val = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                hwnd, 20, ctypes.byref(dark_val), ctypes.sizeof(dark_val)
            )
        except Exception:
            pass


class RoundedCard(tk.Canvas):
    """Contenedor tarjeta con esquinas redondeadas 9-slice (redimensionado instantáneo sin lag)."""

    def __init__(
        self,
        parent,
        bg_outer: str = COL_BG,
        fill: str = COL_CARD,
        border: str = COL_BORDER,
        radius: int = 12,
        pad: int = 12,
        **kwargs,
    ) -> None:
        super().__init__(parent, bg=bg_outer, highlightthickness=0, bd=0, **kwargs)
        self._bg_outer = bg_outer
        self._fill = fill
        self._border = border
        self._radius = max(2, radius)
        self._pad = pad

        tl, tr, bl, br = make_rounded_corners_9slice(
            self._radius,
            fill=hex_to_rgba(fill),
            border=hex_to_rgba(border),
            border_w=1,
            bg=hex_to_rgba(bg_outer),
        )
        self._c_imgs = (
            ImageTk.PhotoImage(tl),
            ImageTk.PhotoImage(tr),
            ImageTk.PhotoImage(bl),
            ImageTk.PhotoImage(br),
        )

        self._rect_v = self.create_rectangle(0, 0, 1, 1, fill=fill, outline="", width=0)
        self._rect_h = self.create_rectangle(0, 0, 1, 1, fill=fill, outline="", width=0)
        self._ln_top = self.create_line(0, 0, 1, 0, fill=border, width=1)
        self._ln_bot = self.create_line(0, 0, 1, 0, fill=border, width=1)
        self._ln_lft = self.create_line(0, 0, 0, 1, fill=border, width=1)
        self._ln_rgt = self.create_line(0, 0, 0, 1, fill=border, width=1)
        self._img_tl = self.create_image(0, 0, anchor="nw", image=self._c_imgs[0])
        self._img_tr = self.create_image(0, 0, anchor="nw", image=self._c_imgs[1])
        self._img_bl = self.create_image(0, 0, anchor="nw", image=self._c_imgs[2])
        self._img_br = self.create_image(0, 0, anchor="nw", image=self._c_imgs[3])

        self.inner = tk.Frame(self, bg=fill, bd=0, highlightthickness=0)
        self._win_item = self.create_window(pad, pad, anchor="nw", window=self.inner)
        self.bind("<Configure>", self._on_resize)

    def _on_resize(self, event) -> None:
        w, h = event.width, event.height
        if w < 16 or h < 16:
            return
        r = min(self._radius, w // 2, h // 2)

        self.coords(self._rect_v, r, 1, w - r, h - 1)
        self.coords(self._rect_h, 1, r, w - 1, h - r)
        self.coords(self._ln_top, r, 0, w - r, 0)
        self.coords(self._ln_bot, r, h - 1, w - r, h - 1)
        self.coords(self._ln_lft, 0, r, 0, h - r)
        self.coords(self._ln_rgt, w - 1, r, w - 1, h - r)

        self.coords(self._img_tl, 0, 0)
        self.coords(self._img_tr, w - r, 0)
        self.coords(self._img_bl, 0, h - r)
        self.coords(self._img_br, w - r, h - r)

        iw = max(1, w - 2 * self._pad)
        ih = max(1, h - 2 * self._pad)
        self.coords(self._win_item, self._pad, self._pad)
        self.itemconfigure(self._win_item, width=iw, height=ih)


class RoundedButton(tk.Canvas):
    """Botón moderno redondeado con hover y texto nítido."""

    def __init__(
        self,
        parent,
        text: str,
        command,
        width: int,
        height: int = 34,
        radius: int = 8,
        bg_outer: str = COL_CARD,
        fill: str = COL_BTN_BG,
        hover: str = COL_BTN_HV,
        border: str | None = COL_BTN_BD,
        fg: str = COL_TEXT,
        font: tuple = ("Segoe UI", 9),
    ) -> None:
        try:
            f_obj = tkfont.Font(font=font)
            width = max(width, f_obj.measure(text) + 28)
        except Exception:
            pass
        super().__init__(
            parent,
            width=width,
            height=height,
            bg=bg_outer,
            highlightthickness=0,
            bd=0,
            cursor="hand2",
        )
        self._btn_w = width
        self._btn_h = height
        self._radius = radius
        self._bg_outer = bg_outer
        self._fill = fill
        self._hover = hover
        self._border = border
        self._fg = fg
        self._text = text
        self._font = font
        self._command = command
        self._hovered = False

        self._img_norm: ImageTk.PhotoImage | None = None
        self._img_hov: ImageTk.PhotoImage | None = None
        self._bg_item = self.create_image(0, 0, anchor="nw")
        self._txt_item = self.create_text(
            width // 2, height // 2, text=text, fill=fg, font=font
        )
        self._rebuild_images()

        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)
        self.bind("<ButtonRelease-1>", self._on_click)

    def _rebuild_images(self) -> None:
        bd = hex_to_rgba(self._border) if self._border else None
        im1 = make_rounded_rect_img(
            self._btn_w, self._btn_h, self._radius,
            fill=hex_to_rgba(self._fill), border=bd, border_w=1,
            bg=hex_to_rgba(self._bg_outer),
        )
        im2 = make_rounded_rect_img(
            self._btn_w, self._btn_h, self._radius,
            fill=hex_to_rgba(self._hover), border=bd, border_w=1,
            bg=hex_to_rgba(self._bg_outer),
        )
        self._img_norm = ImageTk.PhotoImage(im1)
        self._img_hov = ImageTk.PhotoImage(im2)
        self.itemconfigure(self._bg_item, image=self._img_hov if self._hovered else self._img_norm)

    def configure_style(
        self,
        text: str | None = None,
        fill: str | None = None,
        hover: str | None = None,
        border: str | None = None,
        fg: str | None = None,
    ) -> None:
        if text is not None:
            self._text = text
            self.itemconfigure(self._txt_item, text=text)
        if fill is not None:
            self._fill = fill
        if hover is not None:
            self._hover = hover
        self._border = border
        if fg is not None:
            self._fg = fg
            self.itemconfigure(self._txt_item, fill=fg)
        self._rebuild_images()

    def _on_enter(self, _e) -> None:
        self._hovered = True
        if self._img_hov:
            self.itemconfigure(self._bg_item, image=self._img_hov)

    def _on_leave(self, _e) -> None:
        self._hovered = False
        if self._img_norm:
            self.itemconfigure(self._bg_item, image=self._img_norm)

    def _on_click(self, event) -> None:
        if 0 <= event.x <= self._btn_w and 0 <= event.y <= self._btn_h and self._command:
            self._command()


class StatusPill(tk.Canvas):
    """Cápsula de estado redondeada en el header."""

    def __init__(self, parent, width: int = 232, height: int = 34) -> None:
        super().__init__(
            parent, width=width, height=height, bg=COL_HEADER, highlightthickness=0, bd=0
        )
        self._min_w = width
        self._pill_w = width
        self._pill_h = height
        self._img: ImageTk.PhotoImage | None = None
        self._font = tkfont.Font(family="Segoe UI", size=10, weight="bold")
        self._bg_item = self.create_image(0, 0, anchor="nw")
        self._dot_item = self.create_oval(14, height // 2 - 4, 22, height // 2 + 4, fill="#9AE6B4", outline="")
        self._txt_item = self.create_text(
            width // 2 + 6, height // 2, text="En espera", fill="#FFFFFF", font=self._font
        )
        self.set_status("En espera", COL_PILL_OFF, "#8EA3B8")

    def set_status(self, text: str, bg_hex: str, dot_hex: str) -> None:
        dot_x = 16
        text_x0 = dot_x + 8 + 10
        self._pill_w = max(self._min_w, text_x0 + self._font.measure(text) + 18)
        self.configure(width=self._pill_w)
        im = make_rounded_rect_img(
            self._pill_w, self._pill_h, self._pill_h // 2,
            fill=hex_to_rgba(bg_hex), border=None, border_w=0,
            bg=hex_to_rgba(COL_HEADER),
        )
        self._img = ImageTk.PhotoImage(im)
        self.itemconfigure(self._bg_item, image=self._img)
        cy = self._pill_h // 2
        self.coords(self._dot_item, dot_x, cy - 4, dot_x + 8, cy + 4)
        self.itemconfigure(self._dot_item, fill=dot_hex)
        avail_center = (text_x0 + self._pill_w - 18) // 2
        self.coords(self._txt_item, avail_center, cy)
        self.itemconfigure(self._txt_item, text=text)


class WiiUTransferApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(f"Wii U Transfer Tools v{APP_VERSION} — Programa para PC (por El Diuca)")
        self.root.geometry("1120x740")
        self.root.minsize(1020, 660)
        self.root.configure(bg=COL_BG)

        apply_dark_titlebar(self.root)

        if getattr(sys, "frozen", False):
            self.base_dir = Path(sys.executable).resolve().parent
        else:
            self.base_dir = Path(__file__).resolve().parent
        self.packages_dir = (self.base_dir / "packages").resolve()
        self.packages_dir.mkdir(parents=True, exist_ok=True)

        app_icon_path, header_icon_path = ensure_pc_assets(self.base_dir / "assets")
        self._app_icon_tk = ImageTk.PhotoImage(Image.open(app_icon_path))
        self._header_icon_tk = ImageTk.PhotoImage(Image.open(header_icon_path))
        try:
            self.root.iconphoto(True, self._app_icon_tk)
        except tk.TclError:
            pass

        self.log_queue: queue.Queue[tuple[str, str]] = queue.Queue()
        self.server = TransferServer(
            self.packages_dir,
            tcp_port=TCP_PORT,
            udp_port=UDP_DISCOVERY_PORT,
            on_event=self._enqueue_log,
        )

        self._last_stat_time = time.monotonic()
        self._pkg_paths: dict[str, Path] = {}
        self._pkg_iids: dict[str, str] = {}
        self._pkg_meta: dict[str, dict] = {}

        # Datos de actualización remota desde GitHub
        self._remote_version = APP_VERSION
        self._remote_wuhb_url = DEFAULT_WUHB_URL
        self._remote_pc_setup_url = DEFAULT_PC_SETUP_URL
        self._remote_notes = ""
        self._update_available = False

        local_ip = get_local_ip()
        self.var_folder = tk.StringVar(value=str(self.packages_dir))
        self.var_ip = tk.StringVar(value=f"{local_ip} (Automática)")
        self.var_clients = tk.StringVar(value="Esperando conexión...")
        self.var_current_game = tk.StringVar(value="Ninguno")
        self.var_prog_detail = tk.StringVar(value="Enviado total: 0 B")
        self.var_speed = tk.StringVar(value="0.00 MB/s")
        self.var_pkg_count = tk.StringVar(value="Juegos en tu PC (0)")
        self.var_tip = tk.StringVar(
            value=(
                "1. Abre Wii U Transfer Tools en tu consola\n"
                "2. Elige arriba 'Modo Red' con el botón [X]\n"
                "3. Pulsa [Y] para buscar y [A] para instalar"
            )
        )

        self._setup_styles()
        self._build_ui()
        self._refresh_packages()

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(120, self._poll_ui)
        self._check_updates_async(notify_if_up_to_date=False)

    def _setup_styles(self) -> None:
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        style.configure(
            "Transfer.Treeview",
            background=COL_CARD_SOFT,
            fieldbackground=COL_CARD_SOFT,
            foreground=COL_TEXT,
            rowheight=34,
            borderwidth=0,
            relief="flat",
            font=("Segoe UI", 10),
        )
        style.configure(
            "Transfer.Treeview.Heading",
            background="#202B3A",
            foreground=COL_TEXT,
            relief="flat",
            borderwidth=0,
            font=("Segoe UI", 9, "bold"),
            padding=(8, 6),
        )
        style.map(
            "Transfer.Treeview.Heading",
            background=[("active", "#28364A")],
        )
        style.map(
            "Transfer.Treeview",
            background=[("selected", "#11384D")],
            foreground=[("selected", "#FFFFFF")],
        )
        style.configure(
            "Clean.Vertical.TScrollbar",
            gripcount=0,
            background="#324359",
            troughcolor=COL_CARD_SOFT,
            bordercolor=COL_CARD_SOFT,
            arrowcolor=COL_MUTED,
            relief="flat",
        )

    def _enqueue_log(self, level: str, message: str) -> None:
        ts = time.strftime("%H:%M:%S")
        self.log_queue.put((level, f"[{ts}] {message}"))

    def _build_ui(self) -> None:
        # 1. Header Oscuro con acento Cyan + Botón "i Acerca de / GitHub"
        header = tk.Frame(self.root, bg=COL_HEADER)
        header.pack(fill="x", side="top")

        icon_lbl = tk.Label(header, image=self._header_icon_tk, bg=COL_HEADER, bd=0)
        icon_lbl.pack(side="left", padx=(20, 12), pady=12)

        title_box = tk.Frame(header, bg=COL_HEADER)
        title_box.pack(side="left", pady=12)
        tk.Label(
            title_box,
            text="Wii U Transfer Tools",
            bg=COL_HEADER,
            fg="#FFFFFF",
            font=("Segoe UI", 17, "bold"),
        ).pack(anchor="w")
        tk.Label(
            title_box,
            text=f"Envía e instala juegos a tu Wii U  •  Ver. {APP_VERSION}  •  El Diuca",
            bg=COL_HEADER,
            fg="#A8D8EC",
            font=("Segoe UI", 9),
        ).pack(anchor="w", pady=(2, 0))

        self.status_pill = StatusPill(header, width=190, height=34)
        self.status_pill.pack(side="right", padx=(8, 20), pady=18)

        self.btn_about = RoundedButton(
            header,
            text="i  Acerca de",
            command=self._open_about_dialog,
            width=124,
            height=34,
            radius=17,
            bg_outer=COL_HEADER,
            fill=COL_PILL_OFF,
            hover="#0F354D",
            border=COL_CYAN,
            fg="#A8D8EC",
            font=("Segoe UI", 9, "bold"),
        )
        self.btn_about.pack(side="right", padx=(8, 4), pady=18)

        tk.Frame(self.root, bg=COL_CYAN, height=2).pack(fill="x", side="top")

        # Banner de alerta de actualización automática (oculto si está al día)
        self.update_banner = tk.Frame(
            self.root,
            bg=COL_AMBER_BG,
            highlightbackground=COL_AMBER_BD,
            highlightthickness=1,
        )
        self.lbl_update_banner = tk.Label(
            self.update_banner,
            text="¡Nueva versión disponible en GitHub!",
            bg=COL_AMBER_BG,
            fg=COL_AMBER_TX,
            font=("Segoe UI", 9, "bold"),
        )
        self.lbl_update_banner.pack(side="left", padx=14, pady=6)
        RoundedButton(
            self.update_banner,
            text="Actualizar ahora",
            command=self._open_about_dialog,
            width=130,
            height=26,
            radius=6,
            bg_outer=COL_AMBER_BG,
            fill=COL_AMBER_BD,
            hover="#D97706",
            border=None,
            fg="#111827",
            font=("Segoe UI", 9, "bold"),
        ).pack(side="right", padx=12, pady=5)

        # Contenedor principal
        main = tk.Frame(self.root, bg=COL_BG)
        main.pack(fill="both", expand=True, padx=16, pady=12)

        # 2. Barra de control (tarjeta redondeada)
        ctrl_card = RoundedCard(main, bg_outer=COL_BG, fill=COL_CARD, radius=12, pad=10, height=60)
        ctrl_card.pack(fill="x", pady=(0, 12))
        ctrl_card.pack_propagate(False)

        ctrl_inner = ctrl_card.inner
        tk.Label(
            ctrl_inner,
            text="Carpeta de juegos:",
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 9, "bold"),
        ).pack(side="left", padx=(4, 8))

        folder_entry = tk.Entry(
            ctrl_inner,
            textvariable=self.var_folder,
            state="readonly",
            readonlybackground=COL_CARD_SOFT,
            fg=COL_TEXT,
            relief="flat",
            highlightbackground=COL_BORDER,
            highlightthickness=1,
            font=("Segoe UI", 9),
        )
        folder_entry.pack(side="left", fill="x", expand=True, ipady=6, padx=(0, 8))

        RoundedButton(
            ctrl_inner, "Cambiar carpeta...", self._choose_folder, width=136, height=34
        ).pack(side="left", padx=3)
        RoundedButton(
            ctrl_inner, "Actualizar", self._refresh_packages, width=88, height=34
        ).pack(side="left", padx=3)
        RoundedButton(
            ctrl_inner, "Abrir carpeta", self._open_root_folder, width=108, height=34
        ).pack(side="left", padx=(3, 8))

        self.btn_toggle = RoundedButton(
            ctrl_inner,
            text="Compartir Juegos",
            command=self.toggle_server,
            width=162,
            height=34,
            radius=8,
            fill=COL_CYAN,
            hover=COL_CYAN_DK,
            border=None,
            fg="#FFFFFF",
            font=("Segoe UI", 10, "bold"),
        )
        self.btn_toggle.pack(side="right")

        # 3. Cuerpo dividido (2 columnas)
        body = tk.Frame(main, bg=COL_BG)
        body.pack(fill="both", expand=True, pady=(0, 12))
        body.columnconfigure(0, weight=64)
        body.columnconfigure(1, weight=36)
        body.rowconfigure(0, weight=1)

        # Columna izquierda: Juegos en tu PC
        left_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=12, pad=14)
        left_card.grid(row=0, column=0, sticky="nsew", padx=(0, 12))

        left_hdr = tk.Frame(left_card.inner, bg=COL_CARD)
        left_hdr.pack(fill="x", pady=(0, 8))
        tk.Label(
            left_hdr,
            textvariable=self.var_pkg_count,
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 12, "bold"),
        ).pack(side="left")
        tk.Label(
            left_hdr,
            text="Doble clic para abrir carpeta del juego",
            bg=COL_CARD,
            fg=COL_MUTED,
            font=("Segoe UI", 8),
        ).pack(side="right", pady=3)

        tree_wrap = tk.Frame(left_card.inner, bg=COL_CARD, highlightbackground=COL_BORDER, highlightthickness=1)
        tree_wrap.pack(fill="both", expand=True)

        cols = ("name", "size", "status")
        self.tree = ttk.Treeview(
            tree_wrap,
            columns=cols,
            show="headings",
            style="Transfer.Treeview",
            selectmode="browse",
        )
        self.tree.heading("name", text="Nombre del Juego", anchor="w")
        self.tree.heading("size", text="Tamaño", anchor="e")
        self.tree.heading("status", text="Estado", anchor="center")

        self.tree.column("name", width=310, minwidth=180, stretch=True, anchor="w")
        self.tree.column("size", width=110, minwidth=90, stretch=False, anchor="e")
        self.tree.column("status", width=165, minwidth=140, stretch=False, anchor="center")

        vsb = ttk.Scrollbar(
            tree_wrap, orient="vertical", command=self.tree.yview, style="Clean.Vertical.TScrollbar"
        )
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")
        self.tree.bind("<Double-1>", self._on_tree_double_click)

        # Columna derecha: Conexión con tu Wii U + Progreso en vivo
        right_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=12, pad=14)
        right_card.grid(row=0, column=1, sticky="nsew")

        tk.Label(
            right_card.inner,
            text="Conexión con tu Wii U",
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 12, "bold"),
        ).pack(anchor="w", pady=(0, 6))

        stats_box = tk.Frame(right_card.inner, bg=COL_CARD)
        stats_box.pack(fill="x")

        self._add_stat_row(stats_box, "Consola Wii U:", self.var_clients, COL_GREEN_HI)
        self._add_stat_row(stats_box, "Enviando ahora:", self.var_current_game, COL_CYAN_HI)

        # Barra de progreso de envío en vivo
        prog_frame = tk.Frame(right_card.inner, bg=COL_CARD)
        prog_frame.pack(fill="x", pady=(6, 4))
        self.prog_canvas = tk.Canvas(
            prog_frame, height=12, bg=COL_TRACK, highlightthickness=0, bd=0
        )
        self.prog_canvas.pack(fill="x")
        self._prog_rect = self.prog_canvas.create_rectangle(0, 0, 0, 12, fill=COL_CYAN, outline="")
        tk.Label(
            prog_frame,
            textvariable=self.var_prog_detail,
            bg=COL_CARD,
            fg=COL_MUTED,
            font=("Segoe UI", 8, "bold"),
        ).pack(anchor="w", pady=(3, 0))

        stats_box2 = tk.Frame(right_card.inner, bg=COL_CARD)
        stats_box2.pack(fill="x")
        self._add_stat_row(stats_box2, "Velocidad:", self.var_speed, COL_GREEN_HI)
        self._add_stat_row(stats_box2, "Red de este PC:", self.var_ip, COL_TEXT)

        tip_card = RoundedCard(
            right_card.inner,
            bg_outer=COL_CARD,
            fill=COL_TIP_BG,
            border=COL_TIP_BD,
            radius=10,
            pad=10,
            height=116,
        )
        tip_card.pack(fill="x", pady=(10, 0))
        tip_card.pack_propagate(False)

        tk.Label(
            tip_card.inner,
            text="¿Cómo instalar en tu Wii U?",
            bg=COL_TIP_BG,
            fg=COL_CYAN_HI,
            font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", pady=(0, 3))
        tk.Label(
            tip_card.inner,
            textvariable=self.var_tip,
            justify="left",
            bg=COL_TIP_BG,
            fg=COL_TEXT,
            font=("Segoe UI", 9),
        ).pack(anchor="nw")

        # 4. Estado y mensajes recientes
        log_card = RoundedCard(main, bg_outer=COL_BG, fill=COL_CARD, radius=12, pad=12, height=154)
        log_card.pack(fill="x")
        log_card.pack_propagate(False)

        tk.Label(
            log_card.inner,
            text="Estado y mensajes recientes",
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 11, "bold"),
        ).pack(anchor="w", pady=(0, 6))

        log_box = RoundedCard(
            log_card.inner,
            bg_outer=COL_CARD,
            fill=COL_CARD_SOFT,
            border=COL_BORDER,
            radius=8,
            pad=6,
        )
        log_box.pack(fill="both", expand=True)

        self.log_text = tk.Text(
            log_box.inner,
            height=4,
            bg=COL_CARD_SOFT,
            fg=COL_TEXT,
            relief="flat",
            bd=0,
            state="disabled",
            font=("Consolas", 9),
            wrap="word",
        )
        log_sb = ttk.Scrollbar(
            log_box.inner, orient="vertical", command=self.log_text.yview, style="Clean.Vertical.TScrollbar"
        )
        self.log_text.configure(yscrollcommand=log_sb.set)
        self.log_text.pack(side="left", fill="both", expand=True)
        log_sb.pack(side="right", fill="y")

        self.log_text.tag_configure("INFO", foreground=COL_TEXT)
        self.log_text.tag_configure("OK", foreground=COL_GREEN_HI)
        self.log_text.tag_configure("WARN", foreground="#F5B041")
        self.log_text.tag_configure("ERR", foreground="#FF5C6C")

        self._enqueue_log(
            "INFO",
            f"Wii U Transfer Tools v{APP_VERSION} listo. Pon tus juegos en la carpeta y pulsa 'Compartir Juegos'.",
        )

    def _add_stat_row(self, parent, label: str, var: tk.StringVar, val_color: str) -> None:
        row = tk.Frame(parent, bg=COL_CARD)
        row.pack(fill="x", pady=2)
        tk.Label(row, text=label, bg=COL_CARD, fg=COL_MUTED, font=("Segoe UI", 9)).pack(side="left")
        tk.Label(
            row, textvariable=var, bg=COL_CARD, fg=val_color, font=("Segoe UI", 9, "bold")
        ).pack(side="right")

    def _choose_folder(self) -> None:
        chosen = filedialog.askdirectory(
            title="Seleccionar carpeta de juegos de Wii U",
            initialdir=str(self.packages_dir),
        )
        if not chosen:
            return
        self.packages_dir = Path(chosen).resolve()
        self.var_folder.set(str(self.packages_dir))
        self.server.set_root(self.packages_dir)
        self._refresh_packages()

    def _open_root_folder(self) -> None:
        open_in_explorer(self.packages_dir)

    def _on_tree_double_click(self, _event) -> None:
        sel = self.tree.selection()
        if not sel:
            return
        path = self._pkg_paths.get(sel[0])
        if path and path.exists():
            open_in_explorer(path)

    def _refresh_packages(self) -> None:
        for iid in self.tree.get_children():
            self.tree.delete(iid)
        self._pkg_paths.clear()
        self._pkg_iids.clear()
        self._pkg_meta.clear()

        pkgs = scan_packages(self.packages_dir)
        self.var_pkg_count.set(f"Juegos en tu PC ({len(pkgs)})")
        for pkg in pkgs:
            status = "Listo para instalar" if pkg["has_tmd"] else "Carpeta de archivos"
            iid = self.tree.insert(
                "",
                "end",
                values=(
                    pkg["name"],
                    format_bytes(pkg["total_bytes"]),
                    status,
                ),
            )
            self._pkg_paths[iid] = self.packages_dir / pkg["name"]
            self._pkg_iids[pkg["name"]] = iid
            self._pkg_meta[pkg["name"]] = pkg

    def toggle_server(self) -> None:
        if self.server.is_running:
            self.server.stop()
            self.btn_toggle.configure_style(
                text="Compartir Juegos", fill=COL_CYAN, hover=COL_CYAN_DK, fg="#FFFFFF"
            )
            self.status_pill.set_status("En espera", COL_PILL_OFF, "#CBD5E0")
        else:
            try:
                self.server.start()
                ip = get_local_ip()
                self.var_ip.set(f"{ip} (Automática)")
                self.btn_toggle.configure_style(
                    text="Detener Envío", fill=COL_RED, hover=COL_RED_HV, fg="#FFFFFF"
                )
                self.status_pill.set_status(f"Compartiendo • {ip}", COL_GREEN, "#A7F3D0")
            except OSError as exc:
                self._enqueue_log("ERR", f"No se pudo compartir en red: {exc}")

    def _poll_ui(self) -> None:
        while True:
            try:
                level, line = self.log_queue.get_nowait()
            except queue.Empty:
                break
            tag = level if level in ("INFO", "OK", "WARN", "ERR") else "INFO"
            self.log_text.configure(state="normal")
            self.log_text.insert("end", line + "\n", tag)
            self.log_text.see("end")
            self.log_text.configure(state="disabled")

        now = time.monotonic()
        elapsed = now - self._last_stat_time
        if elapsed >= 0.45:
            stats = self.server.get_stats(reset_window=True)
            clients = stats["active_clients"]
            if clients:
                self.var_clients.set(f"Conectada ({clients[0]})")
            else:
                self.var_clients.set("Esperando conexión...")

            cur_pkg = stats.get("current_pkg") or ""
            pkg_sent = stats.get("pkg_bytes_sent", 0)
            pkg_total = stats.get("pkg_total_bytes", 0)
            completed = stats.get("completed_pkgs", set())

            if cur_pkg:
                self.var_current_game.set(cur_pkg[:26])
            else:
                self.var_current_game.set("Ninguno")

            cw = max(1, self.prog_canvas.winfo_width())
            if cur_pkg and pkg_total > 0:
                ratio = min(1.0, max(0.0, pkg_sent / float(pkg_total)))
                pct = int(ratio * 100)
                self.prog_canvas.coords(self._prog_rect, 0, 0, int(cw * ratio), 12)
                self.var_prog_detail.set(
                    f"Enviado: {format_bytes(pkg_sent)} de {format_bytes(pkg_total)} ({pct}%)"
                )
            else:
                self.prog_canvas.coords(self._prog_rect, 0, 0, 0, 12)
                self.var_prog_detail.set(
                    f"Enviado total: {format_bytes(stats['bytes_sent_total'])}"
                )

            # Actualizar columna Estado en la tabla de juegos
            for name, iid in self._pkg_iids.items():
                meta = self._pkg_meta.get(name, {})
                if name in completed:
                    new_st = "Instalado en Wii U"
                elif name == cur_pkg and pkg_total > 0:
                    pct = min(99, int((pkg_sent / float(pkg_total)) * 100))
                    new_st = f"Enviando a Wii U ({pct}%)"
                else:
                    new_st = "Listo para instalar" if meta.get("has_tmd", True) else "Carpeta de archivos"
                cur_vals = self.tree.item(iid, "values")
                if len(cur_vals) == 3 and cur_vals[2] != new_st:
                    self.tree.item(iid, values=(cur_vals[0], cur_vals[1], new_st))

            mb_s = (stats["bytes_sent_window"] / (1024.0 * 1024.0)) / max(elapsed, 0.001)
            self.var_speed.set(f"{mb_s:.2f} MB/s")
            self._last_stat_time = now

        if self.root.winfo_exists():
            self.root.after(120, self._poll_ui)

    # =========================================================================
    # Verificador de Actualizaciones en GitHub + Modal "Acerca de"
    # =========================================================================
    def _check_updates_async(self, notify_if_up_to_date: bool = False) -> None:
        def worker() -> None:
            try:
                req = urllib.request.Request(
                    f"{VERSION_JSON_URL}?t={int(time.time())}",
                    headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
                )
                with urllib.request.urlopen(req, timeout=6) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                rem_ver = str(data.get("version", APP_VERSION)).strip()
                wuhb_url = str(data.get("wiiu_wuhb_url", DEFAULT_WUHB_URL)).strip()
                setup_url = str(data.get("pc_setup_url", DEFAULT_PC_SETUP_URL)).strip()
                notes = str(data.get("notes", "")).strip()
                newer = is_version_newer(APP_VERSION, rem_ver)

                def on_ui() -> None:
                    self._remote_version = rem_ver
                    self._remote_wuhb_url = wuhb_url or DEFAULT_WUHB_URL
                    self._remote_pc_setup_url = setup_url or DEFAULT_PC_SETUP_URL
                    self._remote_notes = notes
                    self._update_available = newer
                    if newer:
                        self.lbl_update_banner.configure(
                            text=f"¡Nueva versión v{rem_ver} disponible en GitHub! ({notes or 'Mejoras disponibles'})"
                        )
                        self.update_banner.pack(fill="x", side="top", padx=16, pady=(8, 0))
                        self._enqueue_log("OK", f"Nueva versión v{rem_ver} disponible en GitHub.")
                    else:
                        self.update_banner.pack_forget()
                        if notify_if_up_to_date:
                            messagebox.showinfo(
                                "Actualizaciones",
                                f"Ya tienes instalada la versión más reciente (v{APP_VERSION}).",
                            )

                if self.root.winfo_exists():
                    self.root.after(0, on_ui)
            except Exception as exc:
                if notify_if_up_to_date and self.root.winfo_exists():
                    self.root.after(
                        0,
                        lambda: messagebox.showwarning(
                            "Actualizaciones",
                            f"No se pudo conectar con GitHub:\n{exc}",
                        ),
                    )

        threading.Thread(target=worker, daemon=True).start()

    def _open_about_dialog(self) -> None:
        dlg = tk.Toplevel(self.root)
        dlg.title("Acerca de Wii U Transfer Tools")
        dlg.geometry("700x520")
        dlg.minsize(680, 500)
        dlg.resizable(False, False)
        dlg.configure(bg=COL_BG)
        dlg.transient(self.root)
        dlg.grab_set()
        apply_dark_titlebar(dlg)
        try:
            dlg.iconphoto(False, self._app_icon_tk)
        except Exception:
            pass

        # Header del diálogo
        hdr = tk.Frame(dlg, bg=COL_HEADER)
        hdr.pack(fill="x")
        tk.Label(hdr, image=self._header_icon_tk, bg=COL_HEADER).pack(side="left", padx=(18, 12), pady=12)
        h_txt = tk.Frame(hdr, bg=COL_HEADER)
        h_txt.pack(side="left", pady=12)
        tk.Label(
            h_txt,
            text="Acerca de Wii U Transfer Tools",
            bg=COL_HEADER,
            fg="#FFFFFF",
            font=("Segoe UI", 14, "bold"),
        ).pack(anchor="w")
        tk.Label(
            h_txt,
            text=f"Versión {APP_VERSION}  •  Desarrollado por El Diuca",
            bg=COL_HEADER,
            fg="#A8D8EC",
            font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w")
        tk.Frame(dlg, bg=COL_CYAN, height=2).pack(fill="x")

        body = tk.Frame(dlg, bg=COL_BG)
        body.pack(fill="both", expand=True, padx=20, pady=16)

        # Descripción breve
        desc_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=10, pad=14, height=96)
        desc_card.pack(fill="x", pady=(0, 12))
        desc_card.pack_propagate(False)
        tk.Label(
            desc_card.inner,
            text=(
                "Herramienta gratuita de código abierto para enviar e instalar juegos de Wii U "
                "directamente desde tu PC, Celular Android o Memoria USB (exFAT, FAT32, NTFS y FAT) "
                "por Red Wi-Fi, adaptador LAN USB o cable USB, sin pasos técnicos."
            ),
            justify="left",
            anchor="w",
            wraplength=620,
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 9),
        ).pack(fill="x", anchor="w")

        # Enlace oficial a GitHub
        gh_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=10, pad=14, height=80)
        gh_card.pack(fill="x", pady=(0, 12))
        gh_card.pack_propagate(False)
        RoundedButton(
            gh_card.inner,
            text="Abrir en GitHub",
            command=lambda: webbrowser.open(GITHUB_REPO_URL),
            width=148,
            height=34,
            radius=8,
            bg_outer=COL_CARD,
            fill=COL_BTN_BG,
            hover=COL_BTN_HV,
            border=COL_CYAN,
            fg="#FFFFFF",
            font=("Segoe UI", 9, "bold"),
        ).pack(side="right", padx=(12, 0), pady=6)
        gh_left = tk.Frame(gh_card.inner, bg=COL_CARD)
        gh_left.pack(side="left", fill="x", expand=True)
        tk.Label(
            gh_left,
            text="PROYECTO OFICIAL EN GITHUB",
            bg=COL_CARD,
            fg=COL_MUTED,
            font=("Segoe UI", 8, "bold"),
        ).pack(anchor="w")
        tk.Label(
            gh_left,
            text="https://github.com/supermafari007/WiiU-Transfer-Tools",
            bg=COL_CARD,
            fg=COL_CYAN_HI,
            font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", pady=(2, 0))

        # Centro de Actualizaciones (PC + Wii U .wuhb)
        upd_card = RoundedCard(
            body,
            bg_outer=COL_BG,
            fill=COL_CARD,
            border=COL_AMBER_BD if self._update_available else COL_GREEN,
            radius=10,
            pad=14,
            height=144,
        )
        upd_card.pack(fill="x", pady=(0, 14))
        upd_card.pack_propagate(False)

        var_upd_status = tk.StringVar(
            value=(
                f"¡Nueva versión v{self._remote_version} disponible en GitHub!"
                if self._update_available
                else f"Programa al día (Ver. {APP_VERSION})  •  Actualizador conectado a GitHub"
            )
        )
        lbl_upd = tk.Label(
            upd_card.inner,
            textvariable=var_upd_status,
            justify="left",
            anchor="w",
            wraplength=620,
            bg=COL_CARD,
            fg=COL_AMBER_TX if self._update_available else COL_GREEN_HI,
            font=("Segoe UI", 9, "bold"),
        )
        lbl_upd.pack(fill="x", anchor="w", pady=(0, 4))

        tk.Label(
            upd_card.inner,
            text="Puedes actualizar este programa de PC o descargar la última App para tu consola Wii U (.wuhb):",
            justify="left",
            anchor="w",
            wraplength=620,
            bg=COL_CARD,
            fg=COL_MUTED,
            font=("Segoe UI", 8),
        ).pack(fill="x", anchor="w", pady=(0, 10))

        btn_row = tk.Frame(upd_card.inner, bg=COL_CARD)
        btn_row.pack(fill="x")

        RoundedButton(
            btn_row,
            text="Actualizar Programa PC",
            command=lambda: self._self_update_pc_from_github(var_upd_status, lbl_upd),
            width=192,
            height=36,
            radius=8,
            bg_outer=COL_CARD,
            fill=COL_GREEN,
            hover=COL_GREEN_HV,
            border=None,
            fg="#FFFFFF",
            font=("Segoe UI", 9, "bold"),
        ).pack(side="left", padx=(0, 8))

        RoundedButton(
            btn_row,
            text="Descargar App Wii U (.wuhb)",
            command=lambda: self._download_wuhb_from_github(var_upd_status, lbl_upd),
            width=228,
            height=36,
            radius=8,
            bg_outer=COL_CARD,
            fill=COL_CYAN,
            hover=COL_CYAN_DK,
            border=None,
            fg="#FFFFFF",
            font=("Segoe UI", 9, "bold"),
        ).pack(side="left", padx=(0, 8))

        RoundedButton(
            btn_row,
            text="Comprobar",
            command=lambda: self._check_updates_async(notify_if_up_to_date=True),
            width=116,
            height=36,
            radius=8,
            bg_outer=COL_CARD,
            fill=COL_BTN_BG,
            hover=COL_BTN_HV,
            border=COL_BORDER,
            fg=COL_TEXT,
            font=("Segoe UI", 9, "bold"),
        ).pack(side="right")

        RoundedButton(
            body,
            text="Cerrar ventana",
            command=dlg.destroy,
            width=144,
            height=36,
            radius=8,
            bg_outer=COL_BG,
            fill=COL_BTN_BG,
            hover=COL_BTN_HV,
            border=COL_BORDER,
            fg=COL_TEXT,
            font=("Segoe UI", 9, "bold"),
        ).pack(side="right")

    def _self_update_pc_from_github(self, var_status: tk.StringVar, lbl: tk.Label) -> None:
        var_status.set("Descargando instalador de actualización desde GitHub...")
        lbl.configure(fg=COL_CYAN_HI)

        def worker() -> None:
            if getattr(sys, "frozen", False):
                try:
                    import tempfile
                    dl_path = Path(tempfile.gettempdir()) / f"WiiU_Transfer_Tools_Setup_v{self._remote_version}.exe"
                    req = urllib.request.Request(
                        f"{self._remote_pc_setup_url}?t={int(time.time())}",
                        headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
                    )
                    with urllib.request.urlopen(req, timeout=30) as resp:
                        dl_path.write_bytes(resp.read())
                    subprocess.Popen([str(dl_path)])
                    if self.root.winfo_exists():
                        self.root.after(0, self.on_close)
                except Exception as exc:
                    if self.root.winfo_exists():
                        self.root.after(
                            0,
                            lambda: (
                                var_status.set(f"Error al descargar actualizador: {exc}"),
                                lbl.configure(fg="#FF5C6C"),
                            ),
                        )
                return

            files = ["transfer_server.py", "wiiu_transfer_gui.py", "ui_assets.py"]
            ok_count = 0
            try:
                for fname in files:
                    req = urllib.request.Request(
                        f"{RAW_PC_BASE_URL}/{fname}?t={int(time.time())}",
                        headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
                    )
                    with urllib.request.urlopen(req, timeout=12) as resp:
                        content = resp.read()
                    if len(content) > 128:
                        (self.base_dir / fname).write_bytes(content)
                        ok_count += 1
            except Exception as exc:
                if self.root.winfo_exists():
                    self.root.after(
                        0,
                        lambda: (
                            var_status.set(f"Error al actualizar desde GitHub: {exc}"),
                            lbl.configure(fg="#FF5C6C"),
                        ),
                    )
                return

            def done_ui() -> None:
                if ok_count == len(files):
                    var_status.set("¡Programa PC actualizado! Reinicia la aplicación para aplicar cambios.")
                    lbl.configure(fg=COL_GREEN_HI)
                    self._enqueue_log("OK", "Programa PC actualizado correctamente desde GitHub.")
                else:
                    var_status.set("No se pudieron descargar todos los archivos desde GitHub.")
                    lbl.configure(fg=COL_AMBER_TX)

            if self.root.winfo_exists():
                self.root.after(0, done_ui)

        threading.Thread(target=worker, daemon=True).start()

    def _download_wuhb_from_github(self, var_status: tk.StringVar, lbl: tk.Label) -> None:
        save_path = filedialog.asksaveasfilename(
            title="Guardar WiiU_Transfer.wuhb (Copia este archivo en SD:/wiiu/apps/)",
            initialdir=str(self.base_dir),
            initialfile="WiiU_Transfer.wuhb",
            defaultextension=".wuhb",
            filetypes=[("Aplicación Wii U (.wuhb)", "*.wuhb"), ("Todos los archivos", "*.*")],
        )
        if not save_path:
            return

        var_status.set("Descargando WiiU_Transfer.wuhb desde GitHub...")
        lbl.configure(fg=COL_CYAN_HI)

        def worker() -> None:
            try:
                req = urllib.request.Request(
                    f"{self._remote_wuhb_url}?t={int(time.time())}",
                    headers={"User-Agent": f"WiiU-Transfer-Tools-PC/{APP_VERSION}"},
                )
                with urllib.request.urlopen(req, timeout=25) as resp:
                    blob = resp.read()
                Path(save_path).write_bytes(blob)
                if self.root.winfo_exists():
                    self.root.after(
                        0,
                        lambda: (
                            var_status.set(f"WiiU_Transfer.wuhb guardado ({format_bytes(len(blob))})"),
                            lbl.configure(fg=COL_GREEN_HI),
                            self._enqueue_log("OK", f"App Wii U descargada en: {save_path}"),
                        ),
                    )
            except Exception as exc:
                if self.root.winfo_exists():
                    self.root.after(
                        0,
                        lambda: (
                            var_status.set(f"No se pudo descargar WiiU_Transfer.wuhb: {exc}"),
                            lbl.configure(fg="#FF5C6C"),
                        ),
                    )

        threading.Thread(target=worker, daemon=True).start()

    def on_close(self) -> None:
        self.server.stop()
        if self.root.winfo_exists():
            self.root.destroy()


def main() -> None:
    root = tk.Tk()
    WiiUTransferApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
