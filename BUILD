# BUILD — qué compilar y cómo
#
# Formato: <binario_destino> <fuente1> [fuente2...] [flags_gcc]
# Las rutas son relativas a /sys/src/ dentro del disco ZEROS.
# Los flags opcionales van al final (se pasan directamente a TCC).
#
# NOTA: 'zeros' (la shell) NO está aquí porque depende de libreadline y
# libncurses compiladas contra glibc, que son incompatibles con musl.
# La shell solo puede actualizarse reconstruyendo el initramfs desde el host
# con build.sh. El resto de herramientas solo usan POSIX puro y compilan con TCC.

zeros_format   fs/zeros_format.c
zeros_populate fs/zeros_populate.c fs/zeros_mount.c
update   fs/zeros_update.c fs/zeros_mount.c
upgrade  fs/zeros_upgrade.c fs/zeros_mount.c
