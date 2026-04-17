# Makefile — zeros filesystem tools

CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11

all: zeros_format zeros_fsck zeros_fuse zeros_update zeros_upgrade

zeros_format: zeros_format.c zeros_fs.h
	$(CC) $(CFLAGS) -o zeros_format zeros_format.c

zeros_fsck: zeros_fsck.c zeros_fs.h
	$(CC) $(CFLAGS) -o zeros_fsck zeros_fsck.c

zeros_fuse: zeros_fuse.c zeros_mount.c zeros_fs.h zeros_mount.h
	$(CC) $(CFLAGS) -o zeros_fuse zeros_fuse.c zeros_mount.c \
	    `pkg-config --cflags --libs fuse3`

zeros_update: zeros_update.c zeros_mount.c zeros_fs.h zeros_mount.h
	$(CC) $(CFLAGS) -o zeros_update zeros_update.c zeros_mount.c

zeros_upgrade: zeros_upgrade.c zeros_mount.c zeros_fs.h zeros_mount.h
	$(CC) $(CFLAGS) -o zeros_upgrade zeros_upgrade.c zeros_mount.c

clean:
	rm -f zeros_format zeros_fsck zeros_fuse zeros_update zeros_upgrade *.img

.PHONY: all clean
