# BUILD — qué compilar y cómo
#
# Formato: <binario_destino> <fuente1> [fuente2...] [flags_gcc]
# Las rutas son relativas a /sys/src/ dentro del disco ZEROS.
# Los flags opcionales van al final (se pasan directamente a TCC).

zeros         shell/shell.c shell/editor.c fs/vfs.c fs/vfs_zeros.c fs/vfs_host.c fs/zeros_mount.c -lreadline -lncurses
zeros_format  fs/zeros_format.c
zeros_populate fs/zeros_populate.c fs/zeros_mount.c
zeros_update  fs/zeros_update.c fs/zeros_mount.c
zeros_upgrade fs/zeros_upgrade.c fs/zeros_mount.c
