# Wii U Transfer Tools

<p align="center">
  <img src="Imagenes/3_WiiU_Transfer_Tools_logo.png" alt="Wii U Transfer Tools" width="420">
</p>

Desarrollado por **El Diuca**.

**Versión 2.0.0** — Aplicación para consola Wii U (`.wuhb`), Aplicación para Android 9+ (`.apk`) y Programa para PC Windows con Instalador Gráfico.

**Wii U Transfer Tools** permite transferir e instalar juegos de Wii U en consolas con Aroma utilizando **lenguaje 100% simple** y sin pasos técnicos, ya sea desde una **Memoria USB (`exFAT`, `FAT32`, `NTFS` o `FAT`)**, desde tu **Celular Android** (por cable USB o por Wi-Fi) o desde tu **PC con Windows** (por Wi-Fi o adaptador de red LAN USB).

Este proyecto es **software de código abierto bajo la licencia MIT**. Consulta [LICENSE](LICENSE) para conocer los términos de uso, modificación y redistribución.

## Así se ve Wii U Transfer Tools (Ver. 2.0.0)

| En la consola Wii U (`Modo USB` / `Modo Red`) | Programa de PC + Instalador para Windows |
|:---:|:---:|
| ![Wii U Transfer Tools en TV y GamePad](Imagenes/1_WiiU_Transfer_Tools_consola.png) | ![Programa de Wii U Transfer Tools para PC](Imagenes/2_WiiU_Transfer_Tools_PC.png) |

<p align="center">
  <img src="Imagenes/6_WiiU_Transfer_Tools_Android_MTP.png" alt="Wii U Transfer Tools para Android 9+" width="820">
</p>

## Novedades de la Versión 2.0.0

* **Soporte Multi-Formato para Memoria USB (`exFAT`, `FAT32`, `NTFS` y `FAT`)**:
  * Ya no necesitas formatear tu Memoria USB a FAT32 ni tienes límite de 4 GB por archivo: el motor de lectura USB en caliente lee directamente particiones **exFAT**, **NTFS**, **FAT32** y **FAT (FAT12/FAT16)** tanto en esquemas **MBR** como **GPT** en modo de solo lectura seguro.
* **Selector Simplificado de 2 Modos (`Modo USB` y `Modo Red`)**:
  * **`Modo USB`**: Detecta automáticamente si conectaste una **Memoria USB** (`exFAT` / `FAT32` / `NTFS` / `FAT`) o un **Celular Android** por cable USB en el puerto frontal de la consola.
  * **`Modo Red`**: Optimizado al máximo tanto para **Wi-Fi** como para **Adaptador USB de Red (LAN Ethernet)** con pool `somemopt` de 3 MB + `SO_RUSRBUF` (ventana TCP de 512 KB, `TCP_MAXSEG = 1460`, `SO_NOSLOWSTART`, `TCP_NODELAY` y `TCP_NOACKDELAY`).
* **Lenguaje 100% Simple y Familiar en las 3 Plataformas**:
  * Opciones claras como **`Disco USB`** y **`Memoria Consola`**, método de copia **`Directo (Sin usar tarjeta SD)`** o **`Usando tarjeta SD temporal`**, y botón **`Musica: SI / NO`** en pantalla (o pulsando `[R-Stick]`).
* **App Android compatible desde Android 9 (Pie, API 28) hacia arriba**:
  * Pestañas **`Modo USB (Cable)`** y **`Modo Red (Wi-Fi)`**, acceso directo a **`Ajustes USB`** del teléfono, descompresor `.ZIP` integrado, botón **`i Info`** con enlace a GitHub y descarga directa de actualizaciones (`.apk` y `.wuhb`).
* **Programa para PC con Instalador Gráfico para Windows y Actualizador GitHub**:
  * Nuevo instalador gráfico (`PC/Instalar_WiiU_Transfer_Tools.bat`) que crea icono `.ICO` en el Escritorio y Menú Inicio (abriendo sin ventana negra de consola), configura automáticamente el Firewall de Windows y verifica nuevas versiones en [version.json](version.json).

## Estructura del proyecto

* **`Wii U/`** —
  * [WiiU_Transfer.wuhb](<Wii U/WiiU_Transfer.wuhb>): Aplicación compilada para **Aroma**, lista para copiar a `sd:/wiiu/apps/WiiU_Transfer.wuhb` (incluye música de pantalla de inicio y tema de fondo en TV y GamePad con botón de silencio).
  * [frontend/source/main.cpp](Wii%20U/frontend/source/main.cpp), [frontend/source/usb_fat_reader.hpp](Wii%20U/frontend/source/usb_fat_reader.hpp), [frontend/source/usb_mtp_reader.hpp](Wii%20U/frontend/source/usb_mtp_reader.hpp), [frontend/source/audio_player.hpp](Wii%20U/frontend/source/audio_player.hpp) y [frontend/source/ui_canvas.hpp](Wii%20U/frontend/source/ui_canvas.hpp).
* **`Android/`** —
  * [WiiU_Transfer_Tools_Android.apk](Android/WiiU_Transfer_Tools_Android.apk): Aplicación nativa para **Android 9.0 (API 28) y superiores**.
  * Código fuente en [Android/app/src/main/java/com/eldiuca/wiiutransfer/](Android/app/src/main/java/com/eldiuca/wiiutransfer/) y script de compilación [Android/build_android.ps1](Android/build_android.ps1).
* **`PC/`** —
  * [Instalar_WiiU_Transfer_Tools.bat](PC/Instalar_WiiU_Transfer_Tools.bat) / [Instalar_WiiU_Transfer_Tools.py](PC/Instalar_WiiU_Transfer_Tools.py): Instalador gráfico oficial para Windows.
  * [wiiu_transfer_gui.py](PC/wiiu_transfer_gui.py) y [transfer_server.py](PC/transfer_server.py): Interfaz para PC con barra de progreso en vivo por juego, velocidad en MB/s, ventana **Acerca de / GitHub** y auto-actualizador.
* **`version.json`** — Manifiesto de versión consultado automáticamente por la App Android y el programa de PC para avisar y descargar nuevas actualizaciones desde GitHub.

## Guía rápida de uso

1. **En tu consola Wii U:**
   * Copia `Wii U/WiiU_Transfer.wuhb` dentro de `sd:/wiiu/apps/WiiU_Transfer.wuhb`.
2. **Para instalar desde una Memoria USB (`exFAT`, `FAT32`, `NTFS` o `FAT`):**
   * Copia las carpetas de tus juegos en la raíz de tu Memoria USB (o dentro de `/install`, `/packages` o `/WiiU_Transfer`).
   * Abre **Wii U Transfer Tools** en la consola, conecta tu Memoria USB en el **puerto USB frontal** y en **`Modo USB`** pulsa **`[Y]`**.
   * Marca uno o varios juegos, elige **`Disco USB`** o **`Memoria Consola`** y pulsa **`[A]`** para instalar. **Retira la memoria USB antes de salir al Menú Wii U**.
3. **Para instalar desde tu Celular Android (Android 9 o superior):**
   * Instala `Android/WiiU_Transfer_Tools_Android.apk` en tu celular y coloca tus juegos en `/WiiU_Transfer` o `/Download`.
   * **Por Cable USB**: En la app elige **`Modo USB (Cable)`**, pulsa **`Preparar Juegos para USB`**, conecta el celular al USB frontal de la Wii U en modo *Transferencia de archivos* y en la consola elige **`Modo USB`** y pulsa **`[Y]`**.
   * **Por Red Wi-Fi**: En la app elige **`Modo Red (Wi-Fi)`**, pulsa **`Compartir Juegos por Red`** y en la consola cambia a **`Modo Red`** con **`[X]`** y pulsa **`[Y]`**.
4. **Para instalar desde tu PC con Windows:**
   * Ejecuta `PC/Instalar_WiiU_Transfer_Tools.bat` (o abre directo `PC/Iniciar_WiiU_Transfer_GUI.bat`).
   * Pon tus carpetas de juegos en `PC/packages/`, pulsa **`Compartir Juegos`** y en tu Wii U cambia a **`Modo Red`** con **`[X]`** y pulsa **`[Y]`**.
