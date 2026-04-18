#pragma once

#include "zeros_fs.h"

/* Port del filesystem ZEROS para el kernel bare-metal.
 * Misma API que zeros_mount.h pero sin POSIX:
 *   - sin fd (el disco se accede via disk.h)
 *   - sin malloc (usa kmalloc/kfree)
 *   - sin printf (usa console_print)
 *   - sin time (timestamps = 0)
 *   - sin readline (no zeros_path_complete)
 */

typedef struct kzeros_mount kzeros_mount_t;

kzeros_mount_t *kzeros_open (void);
void            kzeros_close(kzeros_mount_t *mnt);

int         kzeros_cd (kzeros_mount_t *mnt, const char *path);
const char *kzeros_pwd(kzeros_mount_t *mnt);

int kzeros_ls  (kzeros_mount_t *mnt, const char *path);
int kzeros_cat (kzeros_mount_t *mnt, const char *path);

int kzeros_mkdir     (kzeros_mount_t *mnt, const char *path);
int kzeros_touch     (kzeros_mount_t *mnt, const char *path);
int kzeros_write_file(kzeros_mount_t *mnt, const char *path,
                      const void *data, unsigned int len);
int kzeros_rm        (kzeros_mount_t *mnt, const char *path);

int kzeros_getattr(kzeros_mount_t *mnt, const char *path, zeros_inode *out);
