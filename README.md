# Z.E.R.O.S

A minimal real operating system built from scratch in C.

Z.E.R.O.S is designed for embedded hardware. The goal is a system that understands itself — one where you can read, modify and recompile every component from within the running system.

> Current state: **v0.1** — functional shell with custom filesystem, self-compilation and self-update via GitHub.

---

## Features

- Custom filesystem (ZEROS) with superblock, bitmaps, inodes and extents
- VFS driver layer — the shell does not know what filesystem is underneath
- FUSE driver — mounts the ZEROS disk as a regular Linux directory
- Minimal shell with REPL, history, tab completion and built-in editor
- C compiler integration via TCC — compile programs from inside the system
- Self-update system: `zeros_update` pulls sources from this repo, `zeros_upgrade` recompiles them
- Bootable ISO for VirtualBox and QEMU

## Project structure

```
shell/          Shell and built-in text editor
fs/             Filesystem: format, mount, VFS driver, FUSE driver, update/upgrade tools
vm/             Init process (PID 1), VM build script
MANIFEST        List of source files managed by zeros_update
BUILD           Compilation rules used by zeros_upgrade
```

## Requirements

Building requires a Linux environment (WSL on Windows works).

```bash
sudo apt-get install -y gcc make libreadline-dev libfuse3-dev pkg-config \
                        busybox-static tcc musl-dev \
                        xorriso isolinux syslinux-common
```

## Build

```bash
# Filesystem tools
cd fs && make

# Shell (development build, runs on the host)
cd shell && make

# Full VM image (kernel + initramfs + bootable ISO)
cd vm && ./build.sh
```

## Run

**Development (WSL):**
```bash
fs/zeros_format disk.img 8M
shell/zeros disk.img
```

**Virtual machine (VirtualBox):**

1. Create a new VM: Linux 64-bit, 256 MB RAM, no hard disk
2. Storage → IDE controller → add `vm/zeros.iso` as CD
3. Storage → SATA controller → add `vm/disk.img` as existing disk (RAW format)
4. Boot order: CD first

On first boot the disk is automatically formatted and populated with sources and binaries.

## Self-update

From inside the running system:

```
ZEROS:/> zeros_update /dev/sda     — pull latest sources from GitHub
ZEROS:/> zeros_upgrade /dev/sda    — recompile and install to /bin
```

The repo URL is read from `/etc/zeros.conf`:
```
REPO_URL=https://raw.githubusercontent.com/Hugo-M-N/Z.E.R.O.S/main
```

## Shell commands

| Command | Description |
|---|---|
| `ls [path]` | List directory |
| `cd [path]` | Change directory |
| `cat <path>` | Read file |
| `write <path> <text>` | Write file |
| `mkdir <path>` | Create directory |
| `rm <path>` | Remove file or empty directory |
| `stat <path>` | File info |
| `edit <path>` | Open built-in text editor |
| `cc <file.c> [-o output]` | Compile C file with TCC |
| `help` | Show help |
| `shutdown` | Power off |

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

Copyright (C) 2026 Hugo Martín Nacarino

---
---

# Z.E.R.O.S

Un sistema operativo real y minimalista construido desde cero en C.

Z.E.R.O.S está diseñado para hardware embebido. El objetivo es un sistema que se entiende a sí mismo — en el que puedes leer, modificar y recompilar cada componente desde dentro del propio sistema en ejecución.

> Estado actual: **v0.1** — shell funcional con filesystem propio, autocompilación y actualización automática desde GitHub.

---

## Características

- Filesystem propio (ZEROS) con superbloque, bitmaps, inodos y extents
- Capa de driver VFS — la shell no sabe qué filesystem hay por debajo
- Driver FUSE — monta el disco ZEROS como un directorio Linux normal
- Shell minimalista con REPL, historial, autocompletado y editor de texto integrado
- Compilador C integrado mediante TCC — compila programas desde dentro del sistema
- Sistema de actualización automática: `zeros_update` descarga las fuentes de este repo, `zeros_upgrade` las recompila
- ISO arrancable para VirtualBox y QEMU

## Estructura del proyecto

```
shell/          Shell y editor de texto integrado
fs/             Filesystem: formato, montaje, driver VFS, driver FUSE, herramientas de actualización
vm/             Proceso init (PID 1), script de compilación de la VM
MANIFEST        Lista de archivos fuente gestionados por zeros_update
BUILD           Reglas de compilación usadas por zeros_upgrade
```

## Requisitos

Compilar requiere un entorno Linux (WSL en Windows funciona).

```bash
sudo apt-get install -y gcc make libreadline-dev libfuse3-dev pkg-config \
                        busybox-static tcc musl-dev \
                        xorriso isolinux syslinux-common
```

## Compilar

```bash
# Herramientas del filesystem
cd fs && make

# Shell (compilación de desarrollo, corre en el host)
cd shell && make

# Imagen completa de la VM (kernel + initramfs + ISO arrancable)
cd vm && ./build.sh
```

## Ejecutar

**Desarrollo (WSL):**
```bash
fs/zeros_format disk.img 8M
shell/zeros disk.img
```

**Máquina virtual (VirtualBox):**

1. Crear una VM nueva: Linux 64-bit, 256 MB RAM, sin disco duro
2. Almacenamiento → Controlador IDE → añadir `vm/zeros.iso` como CD
3. Almacenamiento → Controlador SATA → añadir `vm/disk.img` como disco existente (formato RAW)
4. Orden de arranque: CD primero

En el primer arranque el disco se formatea automáticamente y se puebla con fuentes y binarios.

## Autoactualización

Desde dentro del sistema en ejecución:

```
ZEROS:/> zeros_update /dev/sda     — descarga las fuentes más recientes de GitHub
ZEROS:/> zeros_upgrade /dev/sda    — recompila e instala en /bin
```

La URL del repositorio se lee de `/etc/zeros.conf`:
```
REPO_URL=https://raw.githubusercontent.com/Hugo-M-N/Z.E.R.O.S/main
```

## Comandos de la shell

| Comando | Descripción |
|---|---|
| `ls [ruta]` | Listar directorio |
| `cd [ruta]` | Cambiar directorio |
| `cat <ruta>` | Leer archivo |
| `write <ruta> <texto>` | Escribir archivo |
| `mkdir <ruta>` | Crear directorio |
| `rm <ruta>` | Eliminar archivo o directorio vacío |
| `stat <ruta>` | Información del archivo |
| `edit <ruta>` | Abrir el editor de texto integrado |
| `cc <archivo.c> [-o salida]` | Compilar archivo C con TCC |
| `help` | Mostrar ayuda |
| `shutdown` | Apagar el sistema |

## Licencia

GNU General Public License v3.0 — ver [LICENSE](LICENSE).

Copyright (C) 2026 Hugo Martín Nacarino
