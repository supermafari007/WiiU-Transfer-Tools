#!/usr/bin/env python3
"""
Wii U Transfer - Interfaz Gráfica para PC (Tkinter + Pillow)
Diseño alineado con Wii U Transfer: esquinas redondeadas anti-aliased,
icono GamePad + ondas Wi-Fi supersampleado, tarjetas limpias y registro en vivo.
"""

import ctypes
import os
import queue
import subprocess
import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk
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
COL_GREEN_HI  = "#2FE082"
COL_RED       = "#D92B3A"
COL_RED_HV    = "#B51F2C"
COL_BTN_BG    = "#253141"
COL_BTN_HV    = "#314156"
COL_BTN_BD    = "#3A4D66"
COL_TEXT      = "#ECF3F9"
COL_MUTED     = "#8EA3B8"
COL_PILL_OFF  = "#072638"


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

        # Relleno interior + líneas de borde + 4 esquinas anti-aliased
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
            width // 2 + 6, height // 2, text="Detenido", fill="#FFFFFF", font=self._font
        )
        self.set_status("Detenido", COL_PILL_OFF, "#8EA3B8")

    def set_status(self, text: str, bg_hex: str, dot_hex: str) -> None:
        # Ajusta el ancho al texto (evita desbordes con escalado DPI de Windows)
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
        self.root.title("Wii U Transfer Tools — Servidor PC (por El Diuca)")
        self.root.geometry("1040x700")
        self.root.minsize(920, 630)
        self.root.configure(bg=COL_BG)

        # Barra de título oscura nativa en Windows 10/11
        if sys.platform.startswith("win"):
            try:
                self.root.update_idletasks()
                hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
                dark_val = ctypes.c_int(1)
                ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, 20, ctypes.byref(dark_val), ctypes.sizeof(dark_val)
                )
            except Exception:
                pass

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

        local_ip = get_local_ip()
        self.var_folder = tk.StringVar(value=str(self.packages_dir))
        self.var_ip = tk.StringVar(value=local_ip)
        self.var_clients = tk.StringVar(value="0 conectadas")
        self.var_sent = tk.StringVar(value="0 B")
        self.var_speed = tk.StringVar(value="0.00 MB/s")
        self.var_pkg_count = tk.StringVar(value="Paquetes disponibles (0)")
        self.var_tip = tk.StringVar(
            value=(
                "1. Abre Wii U Transfer Tools en consola\n"
                f"2. Conecta a {local_ip}:{TCP_PORT}\n"
                "3. Marca 1 o varios paquetes e instala"
            )
        )

        self._setup_styles()
        self._build_ui()
        self._refresh_packages()

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(120, self._poll_ui)

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
            rowheight=32,
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
        # 1. Header Oscuro con acento Cyan
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
            text="Transferencia directa por red hacia Wii U  •  Desarrollado por El Diuca",
            bg=COL_HEADER,
            fg="#A8D8EC",
            font=("Segoe UI", 9),
        ).pack(anchor="w", pady=(2, 0))

        self.status_pill = StatusPill(header, width=252, height=34)
        self.status_pill.pack(side="right", padx=20, pady=18)

        tk.Frame(self.root, bg=COL_CYAN, height=2).pack(fill="x", side="top")

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
            text="Carpeta:",
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
            ctrl_inner, "Cambiar carpeta...", self._choose_folder, width=148, height=34
        ).pack(side="left", padx=3)
        RoundedButton(
            ctrl_inner, "Recargar", self._refresh_packages, width=90, height=34
        ).pack(side="left", padx=3)
        RoundedButton(
            ctrl_inner, "Abrir carpeta", self._open_root_folder, width=114, height=34
        ).pack(side="left", padx=(3, 8))

        self.btn_toggle = RoundedButton(
            ctrl_inner,
            text="Iniciar Servidor",
            command=self.toggle_server,
            width=154,
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
        body.columnconfigure(0, weight=66)
        body.columnconfigure(1, weight=34)
        body.rowconfigure(0, weight=1)

        # Columna izquierda: Paquetes disponibles
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
            text="Doble clic para abrir carpeta",
            bg=COL_CARD,
            fg=COL_MUTED,
            font=("Segoe UI", 8),
        ).pack(side="right", pady=3)

        tree_wrap = tk.Frame(left_card.inner, bg=COL_CARD, highlightbackground=COL_BORDER, highlightthickness=1)
        tree_wrap.pack(fill="both", expand=True)

        cols = ("name", "files", "size", "kind")
        self.tree = ttk.Treeview(
            tree_wrap,
            columns=cols,
            show="headings",
            style="Transfer.Treeview",
            selectmode="browse",
        )
        self.tree.heading("name", text="Paquete", anchor="w")
        self.tree.heading("files", text="Archivos", anchor="center")
        self.tree.heading("size", text="Tamaño", anchor="e")
        self.tree.heading("kind", text="Tipo", anchor="center")

        self.tree.column("name", width=260, minwidth=160, stretch=True, anchor="w")
        self.tree.column("files", width=88, minwidth=78, stretch=False, anchor="center")
        self.tree.column("size", width=100, minwidth=85, stretch=False, anchor="e")
        self.tree.column("kind", width=120, minwidth=100, stretch=False, anchor="center")

        vsb = ttk.Scrollbar(
            tree_wrap, orient="vertical", command=self.tree.yview, style="Clean.Vertical.TScrollbar"
        )
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")
        self.tree.bind("<Double-1>", self._on_tree_double_click)

        # Columna derecha: Estado de Red
        right_card = RoundedCard(body, bg_outer=COL_BG, fill=COL_CARD, radius=12, pad=14)
        right_card.grid(row=0, column=1, sticky="nsew")

        tk.Label(
            right_card.inner,
            text="Estado de Red",
            bg=COL_CARD,
            fg=COL_TEXT,
            font=("Segoe UI", 12, "bold"),
        ).pack(anchor="w", pady=(0, 6))

        stats_box = tk.Frame(right_card.inner, bg=COL_CARD)
        stats_box.pack(fill="x")

        self._add_stat_row(stats_box, "IP Local del PC:", self.var_ip, COL_CYAN_HI)
        self._add_stat_row(
            stats_box,
            "Puertos:",
            tk.StringVar(value=f"TCP {TCP_PORT} / UDP {UDP_DISCOVERY_PORT}"),
            COL_TEXT,
        )
        self._add_stat_row(stats_box, "Consolas:", self.var_clients, COL_TEXT)
        self._add_stat_row(stats_box, "Datos enviados:", self.var_sent, COL_TEXT)
        self._add_stat_row(stats_box, "Velocidad:", self.var_speed, COL_GREEN_HI)

        tip_card = RoundedCard(
            right_card.inner,
            bg_outer=COL_CARD,
            fill=COL_TIP_BG,
            border=COL_TIP_BD,
            radius=10,
            pad=10,
            height=112,
        )
        tip_card.pack(fill="x", pady=(10, 0))
        tip_card.pack_propagate(False)

        tk.Label(
            tip_card.inner,
            text="Uso rápido en tu consola:",
            bg=COL_TIP_BG,
            fg=COL_TEXT,
            font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", pady=(0, 4))
        tk.Label(
            tip_card.inner,
            textvariable=self.var_tip,
            justify="left",
            bg=COL_TIP_BG,
            fg=COL_TEXT,
            font=("Segoe UI", 9),
        ).pack(anchor="nw")

        # 4. Registro de actividad
        log_card = RoundedCard(main, bg_outer=COL_BG, fill=COL_CARD, radius=12, pad=12, height=150)
        log_card.pack(fill="x")
        log_card.pack_propagate(False)

        tk.Label(
            log_card.inner,
            text="Registro de actividad",
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
            "Wii U Transfer Tools (por El Diuca) listo. Coloca tus carpetas en 'packages' e inicia el servidor.",
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
            title="Seleccionar carpeta de paquetes",
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

        pkgs = scan_packages(self.packages_dir)
        self.var_pkg_count.set(f"Paquetes disponibles ({len(pkgs)})")
        for pkg in pkgs:
            kind = "Paquete WUP" if pkg["has_tmd"] else "Carpeta"
            iid = self.tree.insert(
                "",
                "end",
                values=(
                    pkg["name"],
                    pkg["file_count"],
                    format_bytes(pkg["total_bytes"]),
                    kind,
                ),
            )
            self._pkg_paths[iid] = self.packages_dir / pkg["name"]

    def toggle_server(self) -> None:
        if self.server.is_running:
            self.server.stop()
            self.btn_toggle.configure_style(
                text="Iniciar Servidor", fill=COL_CYAN, hover=COL_CYAN_DK, fg="#FFFFFF"
            )
            self.status_pill.set_status("Detenido", COL_PILL_OFF, "#CBD5E0")
        else:
            try:
                self.server.start()
                ip = get_local_ip()
                self.var_ip.set(ip)
                self.var_tip.set(
                    "1. Abre Wii U Transfer Tools en consola\n"
                    f"2. Conecta a {ip}:{TCP_PORT}\n"
                    "3. Marca 1 o varios paquetes e instala"
                )
                self.btn_toggle.configure_style(
                    text="Detener Servidor", fill=COL_RED, hover=COL_RED_HV, fg="#FFFFFF"
                )
                self.status_pill.set_status(f"En línea • {ip}:{TCP_PORT}", COL_GREEN, "#A7F3D0")
            except OSError as exc:
                self._enqueue_log("ERR", f"No se pudo iniciar el servidor: {exc}")

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
        if elapsed >= 0.5:
            stats = self.server.get_stats(reset_window=True)
            clients = stats["active_clients"]
            if clients:
                self.var_clients.set(f"{len(clients)} ({clients[0]})")
            else:
                self.var_clients.set("0 conectadas")
            self.var_sent.set(format_bytes(stats["bytes_sent_total"]))
            mb_s = (stats["bytes_sent_window"] / (1024.0 * 1024.0)) / max(elapsed, 0.001)
            self.var_speed.set(f"{mb_s:.2f} MB/s")
            self._last_stat_time = now

        if self.root.winfo_exists():
            self.root.after(120, self._poll_ui)

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
