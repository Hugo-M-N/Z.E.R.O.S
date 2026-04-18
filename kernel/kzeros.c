/*
 * kzeros.c — Port del filesystem ZEROS para el kernel bare-metal
 *
 * Equivalente a zeros_mount.c pero sin dependencias POSIX:
 *   - I/O de disco: disk_read_sector / disk_write_sector (2 sectores = 1 bloque)
 *   - Memoria:      kmalloc / kfree
 *   - Salida:       console_print / console_print_uint / console_print_buf
 *   - Cadenas:      kstring.h (k_strcmp, k_strncpy, etc.)
 *   - Timestamps:   fijados a 0 (no hay RTC todavía)
 */

#include "kzeros.h"
#include "disk.h"
#include "heap.h"
#include "console.h"
#include "kstring.h"

/* ── Estructura interna ──────────────────────────────────*/

struct kzeros_mount {
    zeros_superblock  sb;
    unsigned char    *inode_bitmap;
    unsigned char    *block_bitmap;
    unsigned int      cwd_ino;
    char              cwd_path[1024];
};

/* ── I/O de bloques ──────────────────────────────────────
 * ZEROS_BLOCK_SIZE = 1024 bytes = 2 sectores ATA de 512 bytes */

static int read_block(unsigned int blk, void *data) {
    unsigned char *p = (unsigned char *)data;
    unsigned int lba = blk * 2;
    if (disk_read_sector(lba,     p)       < 0) return -1;
    if (disk_read_sector(lba + 1, p + 512) < 0) return -1;
    return 0;
}

static int write_block(unsigned int blk, const void *data) {
    const unsigned char *p = (const unsigned char *)data;
    unsigned int lba = blk * 2;
    if (disk_write_sector(lba,     p)       < 0) return -1;
    if (disk_write_sector(lba + 1, p + 512) < 0) return -1;
    return 0;
}

/* ── Inodos ──────────────────────────────────────────────*/

static int write_inode(kzeros_mount_t *mnt, unsigned int ino, const zeros_inode *inode) {
    unsigned int blk = mnt->sb.inode_table_start + ino / ZEROS_INODES_PER_BLOCK;
    unsigned int off = (ino % ZEROS_INODES_PER_BLOCK) * ZEROS_INODE_SIZE;
    unsigned char buf[ZEROS_BLOCK_SIZE];
    if (read_block(blk, buf) < 0) return -1;
    memcpy(buf + off, inode, ZEROS_INODE_SIZE);
    return write_block(blk, buf);
}

static int read_inode(kzeros_mount_t *mnt, unsigned int ino, zeros_inode *inode) {
    unsigned int blk = mnt->sb.inode_table_start + ino / ZEROS_INODES_PER_BLOCK;
    unsigned int off = (ino % ZEROS_INODES_PER_BLOCK) * ZEROS_INODE_SIZE;
    unsigned char buf[ZEROS_BLOCK_SIZE];
    if (read_block(blk, buf) < 0) return -1;
    memcpy(inode, buf + off, ZEROS_INODE_SIZE);
    return 0;
}

/* ── Bitmap ──────────────────────────────────────────────*/

static int  bm_test (const unsigned char *bm, unsigned int bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1;
}
static void bm_set  (unsigned char *bm, unsigned int bit) {
    bm[bit / 8] |=  (unsigned char)(1u << (bit % 8));
}
static void bm_clear(unsigned char *bm, unsigned int bit) {
    bm[bit / 8] &= (unsigned char)~(1u << (bit % 8));
}

static unsigned int alloc_inode(kzeros_mount_t *mnt) {
    for (unsigned int i = 0; i < mnt->sb.total_inodes; i++) {
        if (!bm_test(mnt->inode_bitmap, i)) {
            bm_set(mnt->inode_bitmap, i);
            mnt->sb.free_inodes--;
            return i;
        }
    }
    return (unsigned int)-1;
}

static unsigned int alloc_block(kzeros_mount_t *mnt) {
    for (unsigned int i = mnt->sb.data_start; i < mnt->sb.total_blocks; i++) {
        if (!bm_test(mnt->block_bitmap, i)) {
            bm_set(mnt->block_bitmap, i);
            mnt->sb.free_blocks--;
            return i;
        }
    }
    return (unsigned int)-1;
}

static void free_inode(kzeros_mount_t *mnt, unsigned int ino) {
    bm_clear(mnt->inode_bitmap, ino);
    mnt->sb.free_inodes++;
}

static void free_block(kzeros_mount_t *mnt, unsigned int blk) {
    bm_clear(mnt->block_bitmap, blk);
    mnt->sb.free_blocks++;
}

/* ── Directorios ─────────────────────────────────────────*/

static unsigned int dir_lookup(kzeros_mount_t *mnt, unsigned int dir_ino, const char *name) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return (unsigned int)-1;
    unsigned char buf[ZEROS_BLOCK_SIZE];
    for (unsigned int e = 0; e < dir.extent_count; e++) {
        for (unsigned int b = 0; b < dir.extents[e].length; b++) {
            if (read_block(dir.extents[e].start_block + b, buf) < 0)
                return (unsigned int)-1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (k_strcmp(entries[i].name, name) == 0)
                    return entries[i].inode;
            }
        }
    }
    return (unsigned int)-1;
}

static int dir_add_entry(kzeros_mount_t *mnt, unsigned int dir_ino,
                          const char *name, unsigned int child_ino, unsigned int type) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return -1;
    unsigned char buf[ZEROS_BLOCK_SIZE];

    for (unsigned int e = 0; e < dir.extent_count; e++) {
        for (unsigned int b = 0; b < dir.extents[e].length; b++) {
            unsigned int blk = dir.extents[e].start_block + b;
            if (read_block(blk, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode != ZEROS_DIRENT_FREE) continue;
                entries[i].inode = child_ino;
                entries[i].type  = type;
                k_strncpy(entries[i].name, name, ZEROS_NAME_MAX);
                entries[i].name[ZEROS_NAME_MAX] = '\0';
                if (write_block(blk, buf) < 0) return -1;
                dir.size += sizeof(zeros_dirent);
                return write_inode(mnt, dir_ino, &dir);
            }
        }
    }

    if (dir.extent_count >= ZEROS_MAX_EXTENTS) return -1;
    unsigned int new_blk = alloc_block(mnt);
    if (new_blk == (unsigned int)-1) return -1;

    memset(buf, 0xFF, ZEROS_BLOCK_SIZE);
    zeros_dirent *entries = (zeros_dirent *)buf;
    entries[0].inode = child_ino;
    entries[0].type  = type;
    k_strncpy(entries[0].name, name, ZEROS_NAME_MAX);
    entries[0].name[ZEROS_NAME_MAX] = '\0';
    if (write_block(new_blk, buf) < 0) return -1;

    dir.extents[dir.extent_count].start_block = new_blk;
    dir.extents[dir.extent_count].length      = 1;
    dir.extent_count++;
    dir.size += sizeof(zeros_dirent);
    return write_inode(mnt, dir_ino, &dir);
}

static int dir_remove_entry(kzeros_mount_t *mnt, unsigned int dir_ino, const char *name) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return -1;
    unsigned char buf[ZEROS_BLOCK_SIZE];
    for (unsigned int e = 0; e < dir.extent_count; e++) {
        for (unsigned int b = 0; b < dir.extents[e].length; b++) {
            unsigned int blk = dir.extents[e].start_block + b;
            if (read_block(blk, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (k_strcmp(entries[i].name, name) != 0) continue;
                entries[i].inode = ZEROS_DIRENT_FREE;
                return write_block(blk, buf);
            }
        }
    }
    return -1;
}

static int dir_is_empty(kzeros_mount_t *mnt, unsigned int dir_ino) {
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return 0;
    unsigned char buf[ZEROS_BLOCK_SIZE];
    for (unsigned int e = 0; e < dir.extent_count; e++) {
        for (unsigned int b = 0; b < dir.extents[e].length; b++) {
            if (read_block(dir.extents[e].start_block + b, buf) < 0) return 0;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                if (k_strcmp(entries[i].name, ".") == 0) continue;
                if (k_strcmp(entries[i].name, "..") == 0) continue;
                return 0;
            }
        }
    }
    return 1;
}

/* ── Rutas ───────────────────────────────────────────────*/

static unsigned int resolve_path(kzeros_mount_t *mnt, const char *path) {
    if (!path || !path[0]) return mnt->cwd_ino;
    if (k_strcmp(path, "/") == 0) return ZEROS_ROOT_INODE;

    unsigned int cur = (path[0] == '/') ? ZEROS_ROOT_INODE : mnt->cwd_ino;

    char copy[1024];
    k_strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *tok = k_strtok(copy, "/");
    while (tok) {
        unsigned int next = dir_lookup(mnt, cur, tok);
        if (next == (unsigned int)-1) return (unsigned int)-1;
        cur = next;
        tok = k_strtok(0, "/");
    }
    return cur;
}

static unsigned int resolve_parent(kzeros_mount_t *mnt, const char *path, char *name_out) {
    const char *slash = k_strrchr(path, '/');
    if (!slash) {
        k_strncpy(name_out, path, ZEROS_NAME_MAX);
        name_out[ZEROS_NAME_MAX] = '\0';
        return mnt->cwd_ino;
    }
    if (slash == path) {
        k_strncpy(name_out, slash + 1, ZEROS_NAME_MAX);
        name_out[ZEROS_NAME_MAX] = '\0';
        return ZEROS_ROOT_INODE;
    }
    char parent_path[1024];
    unsigned int len = (unsigned int)(slash - path);
    k_strncpy(parent_path, path, len);
    parent_path[len] = '\0';
    k_strncpy(name_out, slash + 1, ZEROS_NAME_MAX);
    name_out[ZEROS_NAME_MAX] = '\0';
    return resolve_path(mnt, parent_path);
}

/* ── Metadata flush ──────────────────────────────────────*/

static int flush_metadata(kzeros_mount_t *mnt) {
    unsigned char sb_buf[ZEROS_BLOCK_SIZE];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &mnt->sb, sizeof(mnt->sb));
    if (write_block(0, sb_buf) < 0) return -1;
    for (unsigned int i = 0; i < mnt->sb.inode_bitmap_blocks; i++)
        if (write_block(mnt->sb.inode_bitmap_start + i,
                        mnt->inode_bitmap + i * ZEROS_BLOCK_SIZE) < 0) return -1;
    for (unsigned int i = 0; i < mnt->sb.block_bitmap_blocks; i++)
        if (write_block(mnt->sb.block_bitmap_start + i,
                        mnt->block_bitmap + i * ZEROS_BLOCK_SIZE) < 0) return -1;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 * API pública
 * ══════════════════════════════════════════════════════════ */

kzeros_mount_t *kzeros_open(void) {
    kzeros_mount_t *mnt = (kzeros_mount_t *)kmalloc(sizeof(kzeros_mount_t));
    if (!mnt) return 0;
    memset(mnt, 0, sizeof(*mnt));

    unsigned char sb_buf[ZEROS_BLOCK_SIZE];
    if (read_block(0, sb_buf) < 0) { kfree(mnt); return 0; }
    memcpy(&mnt->sb, sb_buf, sizeof(mnt->sb));

    if (mnt->sb.magic != ZEROS_MAGIC) {
        console_print("  [FS] Magic invalido\n");
        kfree(mnt); return 0;
    }

    unsigned int ibm_bytes = mnt->sb.inode_bitmap_blocks * ZEROS_BLOCK_SIZE;
    unsigned int bbm_bytes = mnt->sb.block_bitmap_blocks * ZEROS_BLOCK_SIZE;
    mnt->inode_bitmap = (unsigned char *)kmalloc(ibm_bytes);
    mnt->block_bitmap = (unsigned char *)kmalloc(bbm_bytes);
    if (!mnt->inode_bitmap || !mnt->block_bitmap) {
        kfree(mnt->inode_bitmap); kfree(mnt->block_bitmap);
        kfree(mnt); return 0;
    }

    for (unsigned int i = 0; i < mnt->sb.inode_bitmap_blocks; i++)
        read_block(mnt->sb.inode_bitmap_start + i,
                   mnt->inode_bitmap + i * ZEROS_BLOCK_SIZE);
    for (unsigned int i = 0; i < mnt->sb.block_bitmap_blocks; i++)
        read_block(mnt->sb.block_bitmap_start + i,
                   mnt->block_bitmap + i * ZEROS_BLOCK_SIZE);

    mnt->cwd_ino = ZEROS_ROOT_INODE;
    memcpy(mnt->cwd_path, "/", 2);
    return mnt;
}

void kzeros_close(kzeros_mount_t *mnt) {
    if (!mnt) return;
    flush_metadata(mnt);
    kfree(mnt->inode_bitmap);
    kfree(mnt->block_bitmap);
    kfree(mnt);
}

const char *kzeros_pwd(kzeros_mount_t *mnt) { return mnt->cwd_path; }

int kzeros_cd(kzeros_mount_t *mnt, const char *path) {
    const char *target = (path && path[0]) ? path : "/";
    unsigned int ino = resolve_path(mnt, target);
    if (ino == (unsigned int)-1) return -1;
    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (!ZEROS_IS_DIR(inode.mode)) return -1;
    mnt->cwd_ino = ino;
    if (target[0] == '/') {
        k_strncpy(mnt->cwd_path, target, sizeof(mnt->cwd_path) - 1);
        mnt->cwd_path[sizeof(mnt->cwd_path) - 1] = '\0';
    } else if (k_strcmp(target, "..") == 0) {
        char *slash = (char *)k_strrchr(mnt->cwd_path, '/');
        if (slash && slash != mnt->cwd_path) *slash = '\0';
        else memcpy(mnt->cwd_path, "/", 2);
    } else {
        unsigned int len = k_strlen(mnt->cwd_path);
        if (mnt->cwd_path[len - 1] != '/')
            k_strncat(mnt->cwd_path, "/", sizeof(mnt->cwd_path) - len - 1);
        k_strncat(mnt->cwd_path, target,
                  sizeof(mnt->cwd_path) - k_strlen(mnt->cwd_path) - 1);
    }
    unsigned int l = k_strlen(mnt->cwd_path);
    if (l > 1 && mnt->cwd_path[l - 1] == '/') mnt->cwd_path[l - 1] = '\0';
    return 0;
}

int kzeros_ls(kzeros_mount_t *mnt, const char *path) {
    const char *target = (path && path[0]) ? path : mnt->cwd_path;
    unsigned int dir_ino = resolve_path(mnt, target);
    if (dir_ino == (unsigned int)-1) return -1;
    zeros_inode dir;
    if (read_inode(mnt, dir_ino, &dir) < 0) return -1;
    if (!ZEROS_IS_DIR(dir.mode)) return -1;

    unsigned char buf[ZEROS_BLOCK_SIZE];
    for (unsigned int e = 0; e < dir.extent_count; e++) {
        for (unsigned int b = 0; b < dir.extents[e].length; b++) {
            if (read_block(dir.extents[e].start_block + b, buf) < 0) return -1;
            zeros_dirent *entries = (zeros_dirent *)buf;
            for (int i = 0; i < ZEROS_DIRENTS_PER_BLOCK; i++) {
                if (entries[i].inode == ZEROS_DIRENT_FREE) continue;
                zeros_inode child;
                if (read_inode(mnt, entries[i].inode, &child) < 0) continue;
                console_print("  ");
                console_print(ZEROS_IS_DIR(child.mode) ? "[dir] " : "[reg] ");
                console_print(entries[i].name);
                console_print("\n");
            }
        }
    }
    return 0;
}

int kzeros_cat(kzeros_mount_t *mnt, const char *path) {
    unsigned int ino = resolve_path(mnt, path);
    if (ino == (unsigned int)-1) return -1;
    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (ZEROS_IS_DIR(inode.mode)) return -1;

    unsigned int remaining = (unsigned int)inode.size;
    unsigned char buf[ZEROS_BLOCK_SIZE];
    for (unsigned int e = 0; e < inode.extent_count && remaining > 0; e++) {
        for (unsigned int b = 0; b < inode.extents[e].length && remaining > 0; b++) {
            if (read_block(inode.extents[e].start_block + b, buf) < 0) return -1;
            unsigned int chunk = remaining < ZEROS_BLOCK_SIZE ? remaining : ZEROS_BLOCK_SIZE;
            console_print_buf((const char *)buf, chunk);
            remaining -= chunk;
        }
    }
    return 0;
}

int kzeros_mkdir(kzeros_mount_t *mnt, const char *path) {
    char name[ZEROS_NAME_MAX + 1];
    unsigned int parent_ino = resolve_parent(mnt, path, name);
    if (parent_ino == (unsigned int)-1) return -1;
    if (dir_lookup(mnt, parent_ino, name) != (unsigned int)-1) return -1;

    unsigned int ino = alloc_inode(mnt);
    if (ino == (unsigned int)-1) return -1;
    unsigned int blk = alloc_block(mnt);
    if (blk == (unsigned int)-1) return -1;

    zeros_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = ZEROS_FT_DIR | ZEROS_S_IRUSR | ZEROS_S_IWUSR | ZEROS_S_IXUSR
                              | ZEROS_S_IRGRP | ZEROS_S_IXGRP
                              | ZEROS_S_IROTH | ZEROS_S_IXOTH;
    inode.link_count   = 2;
    inode.extent_count = 1;
    inode.extents[0].start_block = blk;
    inode.extents[0].length      = 1;

    unsigned char buf[ZEROS_BLOCK_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    if (write_block(blk, buf) < 0) return -1;
    if (write_inode(mnt, ino, &inode) < 0) return -1;

    if (dir_add_entry(mnt, ino, ".",  ino,        ZEROS_FT_DIR) < 0) return -1;
    if (dir_add_entry(mnt, ino, "..", parent_ino, ZEROS_FT_DIR) < 0) return -1;
    if (dir_add_entry(mnt, parent_ino, name, ino, ZEROS_FT_DIR) < 0) return -1;
    return flush_metadata(mnt);
}

int kzeros_touch(kzeros_mount_t *mnt, const char *path) {
    if (resolve_path(mnt, path) != (unsigned int)-1) return 0;

    char name[ZEROS_NAME_MAX + 1];
    unsigned int parent_ino = resolve_parent(mnt, path, name);
    if (parent_ino == (unsigned int)-1) return -1;

    unsigned int ino = alloc_inode(mnt);
    if (ino == (unsigned int)-1) return -1;

    zeros_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = ZEROS_FT_REG | ZEROS_S_IRUSR | ZEROS_S_IWUSR
                              | ZEROS_S_IRGRP | ZEROS_S_IROTH;
    inode.link_count = 1;

    if (write_inode(mnt, ino, &inode) < 0) return -1;
    if (dir_add_entry(mnt, parent_ino, name, ino, ZEROS_FT_REG) < 0) return -1;
    return flush_metadata(mnt);
}

int kzeros_write_file(kzeros_mount_t *mnt, const char *path,
                      const void *data, unsigned int len) {
    unsigned int ino = resolve_path(mnt, path);
    zeros_inode inode;

    if (ino == (unsigned int)-1) {
        if (kzeros_touch(mnt, path) < 0) return -1;
        ino = resolve_path(mnt, path);
        if (ino == (unsigned int)-1) return -1;
    }
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (ZEROS_IS_DIR(inode.mode)) return -1;

    for (unsigned int e = 0; e < inode.extent_count; e++)
        for (unsigned int b = 0; b < inode.extents[e].length; b++)
            free_block(mnt, inode.extents[e].start_block + b);
    inode.extent_count = 0;
    inode.size = 0;

    const unsigned char *src = (const unsigned char *)data;
    unsigned int written = 0;
    unsigned char buf[ZEROS_BLOCK_SIZE];

    while (written < len) {
        if (inode.extent_count >= ZEROS_MAX_EXTENTS) return -1;
        unsigned int blk = alloc_block(mnt);
        if (blk == (unsigned int)-1) return -1;

        unsigned int chunk = len - written;
        if (chunk > ZEROS_BLOCK_SIZE) chunk = ZEROS_BLOCK_SIZE;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, src + written, chunk);
        if (write_block(blk, buf) < 0) return -1;

        if (inode.extent_count > 0) {
            zeros_extent *prev = &inode.extents[inode.extent_count - 1];
            if (prev->start_block + prev->length == blk) {
                prev->length++;
                written += chunk;
                inode.size += chunk;
                continue;
            }
        }
        inode.extents[inode.extent_count].start_block = blk;
        inode.extents[inode.extent_count].length      = 1;
        inode.extent_count++;
        written += chunk;
        inode.size += chunk;
    }

    if (write_inode(mnt, ino, &inode) < 0) return -1;
    return flush_metadata(mnt);
}

int kzeros_rm(kzeros_mount_t *mnt, const char *path) {
    unsigned int ino = resolve_path(mnt, path);
    if (ino == (unsigned int)-1) return -1;
    if (ino == ZEROS_ROOT_INODE || ino == mnt->cwd_ino) return -1;

    zeros_inode inode;
    if (read_inode(mnt, ino, &inode) < 0) return -1;
    if (ZEROS_IS_DIR(inode.mode) && !dir_is_empty(mnt, ino)) return -1;

    for (unsigned int e = 0; e < inode.extent_count; e++)
        for (unsigned int b = 0; b < inode.extents[e].length; b++)
            free_block(mnt, inode.extents[e].start_block + b);
    free_inode(mnt, ino);

    char name[ZEROS_NAME_MAX + 1];
    unsigned int parent_ino = resolve_parent(mnt, path, name);
    if (parent_ino != (unsigned int)-1)
        dir_remove_entry(mnt, parent_ino, name);

    return flush_metadata(mnt);
}

int kzeros_getattr(kzeros_mount_t *mnt, const char *path, zeros_inode *out) {
    unsigned int ino = resolve_path(mnt, path);
    if (ino == (unsigned int)-1) return -1;
    return read_inode(mnt, ino, out);
}
