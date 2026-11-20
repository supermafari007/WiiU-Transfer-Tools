# Wii U Transfer Tools

<p align="center">
  <img src="Imagenes/3_WiiU_Transfer_Tools_logo.png" alt="Wii U Transfer Tools" width="420">
</p>

Desarrollado por **El Diuca**.

**Versión 1.0.0** — aplicación de Wii U y programa de PC.

**Wii U Transfer Tools** permite transferir e instalar paquetes WUP en consolas Wii U con Aroma, desde el programa de PC por red local o directamente desde un pendrive USB FAT32. Incluye **selección múltiple en cola** y una interfaz unificada para la consola y Windows.

Este proyecto es **software de código abierto bajo la licencia MIT**. Consulta [LICENSE](LICENSE) para conocer los términos de uso, modificación y redistribución.

## Así se ve Wii U Transfer Tools

| En la consola Wii U | Programa de PC para Windows |
|:---:|:---:|
| ![Wii U Transfer Tools en TV y GamePad](Imagenes/1_WiiU_Transfer_Tools_consola.png) | ![Servidor de Wii U Transfer Tools para PC](Imagenes/2_WiiU_Transfer_Tools_PC.png) |

## Estructura de carpetas

* **`PC/`** — Servidor gráfico para Windows con interfaz moderna (`WiiU_Transfer.bat`):
  * [wiiu_transfer_gui.py](PC/wiiu_transfer_gui.py): Ventana principal con selección de carpeta, listado de paquetes (`Paquete WUP` / `Carpeta`), IP local, velocidad en MB/s en vivo y registro de actividad.
  * [transfer_server.py](PC/transfer_server.py): Motor TCP binario (`NFSA`, puerto `7331`) + autodescubrimiento UDP (puerto `7332`). También soporta `--cli`.
  * [requirements.txt](PC/requirements.txt): dependencias de Python.
  * `packages/`: Carpeta predeterminada donde colocar tus paquetes.
* **`Wii U/`** —
  * `WiiU_Transfer.wuhb`: Aplicación compilada para **Aroma**, lista para copiar a `sd:/wiiu/apps/WiiU_Transfer.wuhb`.
  * [frontend/source/main.cpp](Wii%20U/frontend/source/main.cpp), [frontend/source/usb_fat_reader.hpp](Wii%20U/frontend/source/usb_fat_reader.hpp) y [frontend/source/ui_canvas.hpp](Wii%20U/frontend/source/ui_canvas.hpp): Interfaz visual dual-screen nativa 720p (TV 1280×720 + GamePad 854×480 táctil) en WUT puro (`OSScreen`, `VPAD`, `KPAD`, `MCP`, `FSA`) con **2 fuentes** (`Red PC` y `Pendrive USB FAT32`) y **2 modos de escritura**:
    1. **`NetFSA (Cero-SD / Mocha)`** *(predeterminado en Aroma)*: Cero espacio usado en la tarjeta SD; desbloquea `/dev/fsa` con el MochaPayload integrado de Aroma, copia por doble búfer multi-core (CPU1 lee Red/Pendrive mientras CPU2 escribe en paralelo a `usr/tmp` del USB/NAND), instala vía `MCP_InstallTitleAsync` y limpia el temporal automáticamente.
    2. **`Direct-Sync (Cache SD)`**: Usa temporal en la tarjeta SD con limpieza automática al terminar o cancelar (ideal si tu USB/NAND no tiene espacio extra para el temporal o no usas Mocha).
  * `iosu_module/`: Código base ARM (`wiiu_transfer_iosu`) para el hook experimental sobre `/dev/fsa` en IOSU.
  * `protocol/netfsa_protocol.h`: Cabecera binaria compartida.

## Descargar la versión 1.0.0

* **Wii U:** [WiiU_Transfer.wuhb](<Wii U/WiiU_Transfer.wuhb>), listo para copiar a la tarjeta SD.
* **PC:** descarga el repositorio y ejecuta `PC/WiiU_Transfer.bat`; el programa para Windows está disponible como código fuente y necesita Python.

## Guía rápida de uso

1. **En la consola Wii U:**
   * Copia `Wii U/WiiU_Transfer.wuhb` dentro de `sd:/wiiu/apps/WiiU_Transfer.wuhb`.
   * *(Opcional)* Si tu red bloquea el broadcast UDP, crea el archivo `sd:/wiiu/apps/WiiU_Transfer/server_ip.txt` con la IP de tu PC (ejemplo: `192.168.1.50`).
2. **En tu PC:**
   * Instala Python 3.10 o superior y las dependencias con `py -m pip install -r PC/requirements.txt`.
   * Coloca tus carpetas de paquetes dentro de `PC/packages/` (o selecciona cualquier carpeta desde la interfaz).
   * Haz doble clic en `PC/WiiU_Transfer.bat` y pulsa **Iniciar Servidor**.
   * Asegúrate de permitir a Python en el Firewall de Windows en redes privadas (puertos TCP `7331` y UDP `7332`).
3. **Instalar desde Red PC (uno o varios paquetes):**
   * Abre **Wii U Transfer Tools** en la Wii U, elige paquetes con `Arriba / Abajo` (o tocando el GamePad).
   * Para **selección múltiple**, marca varios paquetes con **`[-]` / `[ZL]`** (o tocando la casilla/tarjeta en el GamePad), o marca todos con **`[ZR]`** / botón **`Todos`**.
   * Selecciona **USB Externo** o **NAND Interna** con `Izquierda / Derecha`, y pulsa **`[A]`** para instalar toda la cola.
4. **Instalar desde Pendrive USB (FAT32) en caliente (sin tocar tu disco duro trasero):**
   * Formatea tu pendrive en **FAT32** y copia tus carpetas WUP en la raíz (o dentro de `/install` o `/packages`).
   * Abre **Wii U Transfer Tools** primero en tu Wii U, luego conecta el pendrive en un **puerto USB frontal**.
   * Pulsa **`[X]`** (o toca **`Pendrive USB`** en la barra superior) y **`[Y]`** para detectarlo.
   * Marca 1 o varios paquetes y pulsa **`[A]`** para instalar. **Retira el pendrive antes de salir al Menú de Wii U** para evitar el aviso de formateo del sistema.

## Cómo recompilar `WiiU_Transfer.wuhb` en Windows

Requiere devkitPro con WUT instalado en `C:\devkitPro\opt\devkitpro`.

```powershell
powershell -ExecutionPolicy Bypass -File "Wii U/frontend/build_wiiu.ps1"
```
