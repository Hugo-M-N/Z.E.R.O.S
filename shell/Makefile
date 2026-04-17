# Makefile — Z.E.R.O.S Shell
#
# Flags de compilación:
#   -Wall -Wextra  : activa todos los warnings útiles
#   -g             : incluye símbolos de debug para gdb/valgrind
#   -std=c11       : estándar C moderno
#   -I../fs        : para encontrar zeros_mount.h y zeros_fs.h

CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11 -I../fs
LIBS    = -lreadline -lncurses
TARGET  = zeros
SRC     = shell.c editor.c \
          ../fs/vfs.c ../fs/vfs_zeros.c ../fs/vfs_host.c \
          ../fs/zeros_mount.c

all: $(TARGET)

$(TARGET): $(SRC) editor.h ../fs/vfs.h ../fs/zeros_mount.h ../fs/zeros_fs.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
