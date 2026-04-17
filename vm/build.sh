#!/bin/bash
# build.sh — Construye el initramfs de Z.E.R.O.S
#
# Salida:
#   vm/kernel.img      — kernel de Linux
#   vm/initramfs.img   — sistema base en RAM
#   vm/disk.img        — disco ZEROS persistente (solo se crea la primera vez)
#   vm/zeros.iso       — ISO arrancable para VirtualBox

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$SCRIPT_DIR/.."

BUILD_DIR="/tmp/zeros_vm_build"
INITRAMFS="$BUILD_DIR/initramfs"

OUT_INITRAMFS="$SCRIPT_DIR/initramfs.img"
OUT_KERNEL="$SCRIPT_DIR/kernel.img"
OUT_ISO="$SCRIPT_DIR/zeros.iso"

echo "==> Compilando init (estático)..."
gcc -Wall -Wextra -std=c11 -static \
    -o "$BUILD_DIR/init" \
    "$SCRIPT_DIR/init.c" 2>/dev/null || {
    mkdir -p "$BUILD_DIR"
    gcc -Wall -Wextra -std=c11 -static \
        -o "$BUILD_DIR/init" \
        "$SCRIPT_DIR/init.c"
}

echo "==> Compilando shell (estática)..."
cd "$PROJECT/shell"
gcc -Wall -Wextra -std=c11 -I../fs \
    -static \
    -o "$BUILD_DIR/zeros" \
    shell.c editor.c \
    ../fs/vfs.c ../fs/vfs_zeros.c ../fs/vfs_host.c \
    ../fs/zeros_mount.c \
    -lreadline -lncurses -ltinfo

echo "==> Compilando zeros_format (estático)..."
cd "$PROJECT/fs"
gcc -Wall -Wextra -std=c11 -static \
    -o "$BUILD_DIR/zeros_format" \
    zeros_format.c

echo "==> Compilando zeros_populate (estático)..."
cd "$PROJECT/fs"
gcc -Wall -Wextra -std=c11 -static \
    -o "$BUILD_DIR/zeros_populate" \
    zeros_populate.c zeros_mount.c

echo "==> Compilando update (estático)..."
cd "$PROJECT/fs"
gcc -Wall -Wextra -std=c11 -static \
    -o "$BUILD_DIR/update" \
    zeros_update.c zeros_mount.c

echo "==> Compilando upgrade (estático)..."
cd "$PROJECT/fs"
gcc -Wall -Wextra -std=c11 -static \
    -o "$BUILD_DIR/upgrade" \
    zeros_upgrade.c zeros_mount.c

echo "==> Compilando zeros_shell_update (estático)..."
cd "$PROJECT/fs"
gcc -Wall -Wextra -std=c11 -static \
    -o "$BUILD_DIR/zeros_shell_update" \
    zeros_shell_update.c

echo "==> Copiando binario de la shell al repo para distribución..."
mkdir -p "$PROJECT/bin"
cp "$BUILD_DIR/zeros" "$PROJECT/bin/zeros"
echo "    $PROJECT/bin/zeros actualizado — haz commit+push para distribuirlo."

echo "==> Preparando disco ZEROS..."
cd "$PROJECT/fs"
if [ ! -f "$SCRIPT_DIR/disk.img" ]; then
    echo "    Primera vez — formateando disco nuevo (8 MB)..."
    ./zeros_format "$SCRIPT_DIR/disk.img" 8M
else
    echo "    Disco existente conservado (los datos persisten)."
fi

echo "==> Buscando BusyBox estático..."
BUSYBOX_BIN=$(command -v busybox-static 2>/dev/null || command -v busybox 2>/dev/null || true)
if [ -z "$BUSYBOX_BIN" ]; then
    echo "    No encontrado — instala con: sudo apt-get install -y busybox-static"
    exit 1
fi
if ! file "$BUSYBOX_BIN" | grep -q "statically linked"; then
    echo "    El busybox del sistema no es estático — instala busybox-static"
    exit 1
fi
echo "    Usando: $BUSYBOX_BIN"

echo "==> Construyendo initramfs en $INITRAMFS..."
rm -rf "$INITRAMFS"
mkdir -p "$INITRAMFS"/{bin,dev,proc,sys,tmp,usr/include}

cp "$BUILD_DIR/init"            "$INITRAMFS/init"
cp "$BUILD_DIR/zeros"           "$INITRAMFS/bin/zeros"
cp "$BUILD_DIR/zeros_format"    "$INITRAMFS/bin/zeros_format"
cp "$BUILD_DIR/zeros_populate"  "$INITRAMFS/bin/zeros_populate"
cp "$BUILD_DIR/update"             "$INITRAMFS/bin/update"
cp "$BUILD_DIR/upgrade"            "$INITRAMFS/bin/upgrade"
cp "$BUILD_DIR/zeros_shell_update" "$INITRAMFS/bin/zeros_shell_update"
cp "$BUSYBOX_BIN"                  "$INITRAMFS/bin/busybox"

chmod +x "$INITRAMFS/init"
chmod +x "$INITRAMFS/bin/zeros"
chmod +x "$INITRAMFS/bin/zeros_format"
chmod +x "$INITRAMFS/bin/zeros_populate"
chmod +x "$INITRAMFS/bin/update"
chmod +x "$INITRAMFS/bin/upgrade"
chmod +x "$INITRAMFS/bin/zeros_shell_update"
chmod +x "$INITRAMFS/bin/busybox"

echo "==> Incluyendo fuentes del sistema en initramfs..."
# Las fuentes se guardan en /usr/src/zeros/ (NO en /sys/src/ porque
# init monta sysfs sobre /sys nada más arrancar, tapando ese árbol).
# init.c las copia al disco ZEROS tras el primer formato, bajo /sys/src/.
mkdir -p "$INITRAMFS/usr/src/zeros/shell" \
         "$INITRAMFS/usr/src/zeros/fs" \
         "$INITRAMFS/usr/src/zeros/vm"

cp "$PROJECT/shell/shell.c"        "$INITRAMFS/usr/src/zeros/shell/"
cp "$PROJECT/shell/editor.c"       "$INITRAMFS/usr/src/zeros/shell/"
cp "$PROJECT/shell/editor.h"       "$INITRAMFS/usr/src/zeros/shell/"
cp "$PROJECT/fs/zeros_fs.h"        "$INITRAMFS/usr/src/zeros/fs/"
cp "$PROJECT/fs/zeros_mount.h"     "$INITRAMFS/usr/src/zeros/fs/"
cp "$PROJECT/fs/zeros_mount.c"     "$INITRAMFS/usr/src/zeros/fs/"
cp "$PROJECT/fs/zeros_format.c"    "$INITRAMFS/usr/src/zeros/fs/"
cp "$PROJECT/fs/zeros_populate.c"  "$INITRAMFS/usr/src/zeros/fs/"
cp "$PROJECT/vm/init.c"            "$INITRAMFS/usr/src/zeros/vm/"
cp "$PROJECT/vm/build.sh"          "$INITRAMFS/usr/src/zeros/vm/"
echo "    Fuentes incluidas."

echo "==> Creando symlinks de BusyBox..."
for tool in sh ash make find grep sed awk cut sort uniq \
            head tail wc diff cp mv ln chmod echo printf \
            sleep true false test uname dmesg loadkmap \
            ifconfig route ping wget fbset; do
    if "$BUSYBOX_BIN" --list 2>/dev/null | grep -qx "$tool"; then
        ln -sf /bin/busybox "$INITRAMFS/bin/$tool"
    fi
done

echo "==> Copiando módulos de almacenamiento del kernel..."
# El kernel de Ubuntu compila ahci, virtio_blk, etc. como módulos.
# Los copiamos al initramfs para poder cargarlos con insmod en arranque.
KVER=$(ls /lib/modules/ | sort -V | tail -1)
KMOD="/lib/modules/$KVER"
mkdir -p "$INITRAMFS/lib/modules"

# Módulos necesarios: SATA (VirtualBox), IDE (VirtualBox), VirtIO (QEMU)
for name in scsi_mod libata libahci ahci ata_piix sd_mod virtio virtio_blk e1000; do
    # Busca el .ko con cualquier extensión de compresión
    found=$(find "$KMOD" \
        \( -name "${name}.ko" -o -name "${name}.ko.gz" -o -name "${name}.ko.zst" \) \
        2>/dev/null | head -1)
    if [ -z "$found" ]; then
        echo "    (no encontrado: $name — puede estar compilado en el kernel)"
        continue
    fi
    # Descomprimir según extensión y copiar como .ko plano
    case "$found" in
        *.ko.gz)  zcat "$found" > "$INITRAMFS/lib/modules/${name}.ko" ;;
        *.ko.zst) zstd -d -q "$found" -o "$INITRAMFS/lib/modules/${name}.ko" ;;
        *)        cp "$found" "$INITRAMFS/lib/modules/${name}.ko" ;;
    esac
    echo "    $name ($(du -sh "$INITRAMFS/lib/modules/${name}.ko" | cut -f1))"
done

echo "==> Añadiendo curl (descargas HTTPS)..."
CURL_BIN=$(command -v curl 2>/dev/null || true)
if [ -z "$CURL_BIN" ]; then
    echo "  ERROR: curl no encontrado. Ejecuta: sudo apt-get install -y curl"
    exit 1
fi
cp "$CURL_BIN" "$INITRAMFS/bin/curl"
chmod +x "$INITRAMFS/bin/curl"
ldd "$CURL_BIN" | grep -o '/[^ ]*' | while read -r lib; do
    [ -f "$lib" ] || continue
    dest_dir="$INITRAMFS$(dirname "$lib")"
    mkdir -p "$dest_dir"
    cp -n "$lib" "$dest_dir/"
done
echo "    curl listo: $(du -sh "$INITRAMFS/bin/curl" | cut -f1)"

echo "==> Añadiendo readline y ncurses (para recompilar la shell con TCC)..."
# Headers — TCC los necesita para compilar shell.c
[ -d /usr/include/readline ] && cp -r /usr/include/readline "$INITRAMFS/usr/include/"
for h in ncurses.h curses.h term.h termcap.h; do
    [ -f "/usr/include/$h" ] && cp "/usr/include/$h" "$INITRAMFS/usr/include/"
done
[ -d /usr/include/ncursesw ] && cp -r /usr/include/ncursesw "$INITRAMFS/usr/include/"

# Librerías estáticas — TCC compila con -static, necesita .a no .so
SYSLIB="$INITRAMFS/usr/lib/x86_64-linux-gnu"
mkdir -p "$SYSLIB"
missing_static=""
for libname in readline ncurses tinfo; do
    found=""
    for dir in /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib; do
        [ -f "$dir/lib${libname}.a" ] && { found="$dir/lib${libname}.a"; break; }
    done
    if [ -n "$found" ]; then
        cp -n "$found" "$SYSLIB/"
        echo "    lib${libname}.a: $(du -sh "$found" | cut -f1)"
    else
        missing_static="$missing_static lib${libname}-dev"
    fi
done
if [ -n "$missing_static" ]; then
    echo "    Aviso: faltan libs estáticas. Instala con:"
    echo "    sudo apt-get install -y${missing_static}"
fi
echo "    readline y ncurses listos."

echo "==> Copiando certificados CA (HTTPS)..."
mkdir -p "$INITRAMFS/etc/ssl/certs"
CA_BUNDLE=""
for ca in /etc/ssl/certs/ca-certificates.crt \
          /etc/pki/tls/certs/ca-bundle.crt; do
    [ -f "$ca" ] && { CA_BUNDLE="$ca"; break; }
done
if [ -n "$CA_BUNDLE" ]; then
    cp "$CA_BUNDLE" "$INITRAMFS/etc/ssl/certs/ca-certificates.crt"
    echo "    CA bundle: $(du -sh "$INITRAMFS/etc/ssl/certs/ca-certificates.crt" | cut -f1)"
else
    echo "    Aviso: no se encontró bundle CA — HTTPS puede fallar"
fi

echo "==> Creando script DHCP (udhcpc)..."
mkdir -p "$INITRAMFS/usr/share/udhcpc"
cat > "$INITRAMFS/usr/share/udhcpc/default.script" << 'EOF'
#!/bin/sh
case "$1" in
    bound|renew)
        ifconfig $interface $ip netmask $subnet
        [ -n "$router" ] && route add default gw $router
        [ -n "$dns"    ] && echo "nameserver $dns" > /etc/resolv.conf
        ;;
    deconfig)
        ifconfig $interface 0.0.0.0
        ;;
esac
exit 0
EOF
chmod +x "$INITRAMFS/usr/share/udhcpc/default.script"
echo "    Script DHCP listo."

echo "==> Generando mapa de teclado español..."
mkdir -p "$INITRAMFS/etc"
ES_KMAP=$(find /usr/share/keymaps /usr/share/kbd/keymaps \
               -name "es.kmap.gz" -o -name "es.map.gz" 2>/dev/null | head -1)
if [ -n "$ES_KMAP" ]; then
    zcat "$ES_KMAP" | loadkeys --bkeymap > "$INITRAMFS/etc/es.bkmap" 2>/dev/null \
        && echo "    Keymap español generado." \
        || echo "    Aviso: no se pudo generar el keymap."
else
    echo "    Aviso: keymap español no encontrado."
fi

echo "==> Añadiendo TCC (compilador de C)..."
TCC_BIN=$(command -v tcc 2>/dev/null || true)
if [ -z "$TCC_BIN" ]; then
    echo "  ERROR: tcc no encontrado. Ejecuta: sudo apt-get install -y tcc"
    exit 1
fi
cp "$TCC_BIN" "$INITRAMFS/bin/tcc"
chmod +x "$INITRAMFS/bin/tcc"

echo "    Copiando librerías dinámicas de tcc..."
ldd "$TCC_BIN" | grep -o '/[^ ]*' | while read -r lib; do
    [ -f "$lib" ] || continue
    dest_dir="$INITRAMFS$(dirname "$lib")"
    mkdir -p "$dest_dir"
    cp -n "$lib" "$dest_dir/"
done

TCC_INC=$(find /usr/lib/tcc /usr/local/lib/tcc -maxdepth 1 -name "include" -type d 2>/dev/null | head -1)
[ -n "$TCC_INC" ] && { mkdir -p "$INITRAMFS/usr/lib/tcc"; cp -r "$TCC_INC" "$INITRAMFS/usr/lib/tcc/"; }

echo "    TCC listo: $(du -sh "$INITRAMFS/bin/tcc" | cut -f1)"

echo "==> Incluyendo musl libc (librería estándar C)..."
MUSL_INC=$(find /usr/include -maxdepth 1 -name "*-linux-musl" -type d 2>/dev/null | head -1)
MUSL_LIB=$(find /usr/lib     -maxdepth 1 -name "*-linux-musl" -type d 2>/dev/null | head -1)
if [ -z "$MUSL_INC" ] || [ -z "$MUSL_LIB" ]; then
    echo "  ERROR: musl-dev no encontrado. Ejecuta: sudo apt-get install -y musl-dev"
    exit 1
fi
mkdir -p "$INITRAMFS/usr/lib/musl/include"
cp -r "$MUSL_INC/." "$INITRAMFS/usr/lib/musl/include/"
# TCC calcula su include dir como ../lib/tcc/include relativo a su binario.
# Como está en /bin/tcc busca /lib/tcc/include, no /usr/lib/tcc/include.
# Copiar musl headers a /usr/include (path estándar que TCC siempre busca)
# garantiza que stdio.h se encuentre independientemente de la ubicación de TCC.
cp -r "$MUSL_INC/." "$INITRAMFS/usr/include/"

TCC_SYSLIB="$INITRAMFS/usr/lib/x86_64-linux-gnu"
mkdir -p "$TCC_SYSLIB/tcc"
for f in crt1.o crti.o crtn.o libc.a; do
    [ -f "$MUSL_LIB/$f" ] && cp "$MUSL_LIB/$f" "$TCC_SYSLIB/"
done
LIBTCC1=$(find /usr/lib -name "libtcc1.a" 2>/dev/null | head -1)
[ -n "$LIBTCC1" ] && cp "$LIBTCC1" "$TCC_SYSLIB/"
echo "    musl incluido: $(du -sh "$INITRAMFS/usr/lib/musl" | cut -f1)"

echo "==> Empaquetando initramfs.img..."
cd "$INITRAMFS"
TMP_IMG="$BUILD_DIR/initramfs_tmp.img"
find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$TMP_IMG"
rm -f "$OUT_INITRAMFS" 2>/dev/null || true
dd if="$TMP_IMG" of="$OUT_INITRAMFS" bs=1M 2>/dev/null
rm -f "$TMP_IMG"

echo "==> Copiando kernel..."
KERNEL=$(ls /boot/vmlinuz-* | sort -V | tail -1)
sudo cp "$KERNEL" "$OUT_KERNEL"
sudo chmod 644 "$OUT_KERNEL"

# ── ISO arrancable para VirtualBox ───────────────────────────
# Usa syslinux/isolinux como bootloader para arrancar kernel + initramfs.
# VirtualBox no tiene equivalente al -kernel de QEMU, necesita un bootloader.
echo "==> Generando ISO para VirtualBox..."
if ! command -v xorriso >/dev/null || ! dpkg -l isolinux syslinux-common 2>/dev/null | grep -q "^ii"; then
    echo "    Falta xorriso/isolinux. Ejecuta:"
    echo "    sudo apt-get install -y xorriso isolinux syslinux-common"
    echo "    Saltando generación de ISO."
else
    ISO_DIR="$BUILD_DIR/iso"
    rm -rf "$ISO_DIR"
    mkdir -p "$ISO_DIR/boot/syslinux"

    # Kernel e initramfs
    cp "$OUT_KERNEL"    "$ISO_DIR/boot/vmlinuz"
    cp "$OUT_INITRAMFS" "$ISO_DIR/boot/initramfs.img"

    # Bootloader syslinux (rutas fijas en Ubuntu)
    ISOLINUX_BIN="/usr/lib/ISOLINUX/isolinux.bin"
    SYSLINUX_DIR="/usr/lib/syslinux/modules/bios"

    cp "$ISOLINUX_BIN" "$ISO_DIR/boot/syslinux/"
    for mod in ldlinux.c32 libcom32.c32 libutil.c32 menu.c32; do
        [ -f "$SYSLINUX_DIR/$mod" ] && cp "$SYSLINUX_DIR/$mod" "$ISO_DIR/boot/syslinux/"
    done

    # Configuración del bootloader
    cat > "$ISO_DIR/boot/syslinux/syslinux.cfg" << 'EOF'
DEFAULT zeros
PROMPT 0
TIMEOUT 10

LABEL zeros
  MENU LABEL Z.E.R.O.S
  LINUX /boot/vmlinuz
  INITRD /boot/initramfs.img
  APPEND console=tty0 quiet vga=0x316 fbcon=scrollback:128k
EOF

    # Construir ISO
    TMP_ISO="$BUILD_DIR/zeros_tmp.iso"
    xorriso -as mkisofs \
        -o "$TMP_ISO" \
        -b boot/syslinux/isolinux.bin \
        -c boot/syslinux/boot.cat \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        "$ISO_DIR" 2>/dev/null

    rm -f "$OUT_ISO" 2>/dev/null || true
    dd if="$TMP_ISO" of="$OUT_ISO" bs=1M 2>/dev/null
    rm -f "$TMP_ISO"

    echo "    ISO generada: $(du -sh "$OUT_ISO" | cut -f1)"
fi

SIZE=$(du -sh "$OUT_INITRAMFS" | cut -f1)
echo ""
echo "==> Listo!"
echo "    kernel:    $(wslpath -w "$OUT_KERNEL")"
echo "    initramfs: $(wslpath -w "$OUT_INITRAMFS") ($SIZE)"
echo "    disco:     $(wslpath -w "$SCRIPT_DIR/disk.img")"
[ -f "$OUT_ISO" ] && echo "    ISO:       $(wslpath -w "$OUT_ISO")"
echo ""
echo "==> VirtualBox — configuración de la VM:"
echo "    1. Nueva VM: Linux 64-bit, 256 MB RAM, sin disco duro"
echo "    2. Almacenamiento → Controlador IDE → añadir zeros.iso como CD"
echo "    3. Almacenamiento → Controlador SATA → añadir disco existente → disk.img"
echo "       (Tipo: VirtIO, formato: RAW — usa 'VBoxManage convertfromraw' si pide VDI)"
echo "    4. Sistema → Orden de arranque: CD primero"
echo ""
