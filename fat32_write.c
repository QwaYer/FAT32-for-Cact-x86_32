#include "fat32_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// File/directory mutation ops wired into the VFS operation tables. FAT keeps
// file metadata in the directory entry, so every change to size or first
// cluster goes through fat32_patch_entry() on the owning entry located by
// struct fat32_file_meta.

static void fat32_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// Free a VFS node allocated by fat32_finddir (dirs share the ctx, files own
// a heap meta struct).
static void fat_node_free(vfs_node_t *n, struct fat32_ctx *ctx) {
    if (!n)
        return;
    if (n->type != VFS_DIRECTORY && n->priv && n->priv != ctx)
        kfree(n->priv);
    kfree(n);
}

// Store `len` bytes at `offset` of the chain rooted at `cl`. With src == 0
// the range is filled with zeros (sparse gap / truncate-up).
static uint32_t fat_store(struct fat32_ctx *ctx, uint32_t cl, uint32_t offset,
                          const char *src, uint32_t len) {
    if (len == 0)
        return 0;
    if (!fat32_valid_cluster(ctx, cl))
        return 0;

    uint32_t cloff = 0;
    while (cloff + ctx->cluster_bytes <= offset) {
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        if (!fat32_valid_cluster(ctx, nxt))
            return 0;
        cloff += ctx->cluster_bytes;
        cl = nxt;
    }

    static uint8_t zero512[512];
    uint8_t sec[512];
    uint32_t done = 0;
    uint32_t fpos = offset;

    while (done < len) {
        uint32_t rel = fpos - cloff;
        if (rel >= ctx->cluster_bytes) {
            uint32_t nxt = fat32_next_cluster(ctx, cl);
            if (!fat32_valid_cluster(ctx, nxt))
                break;
            cloff += ctx->cluster_bytes;
            cl = nxt;
            continue;
        }
        uint32_t sec_idx = rel / 512;
        uint32_t boff    = rel % 512;
        uint32_t take    = 512 - boff;
        if (take > len - done)
            take = len - done;
        if (take > ctx->cluster_bytes - rel)
            take = ctx->cluster_bytes - rel;

        uint32_t lba = fat32_cluster_lba(ctx, cl) + sec_idx;
        if (src) {
            if (boff == 0 && take == 512) {
                if (blkdev_write(ctx->blk, lba, (uint8_t *)(src + done)) != 0)
                    break;
            } else {
                if (blkdev_read(ctx->blk, lba, sec) != 0)
                    break;
                memory_copy(sec + boff, src + done, take);
                if (blkdev_write(ctx->blk, lba, sec) != 0)
                    break;
            }
        } else {
            if (boff == 0 && take == 512) {
                if (blkdev_write(ctx->blk, lba, zero512) != 0)
                    break;
            } else {
                if (blkdev_read(ctx->blk, lba, sec) != 0)
                    break;
                memory_set(sec + boff, 0, take);
                if (blkdev_write(ctx->blk, lba, sec) != 0)
                    break;
            }
        }
        done += take;
        fpos += take;
    }
    return done;
}

// ── create / mkdir ──────────────────────────────────────────────────────

int fat32_create(vfs_node_t *dir, const char *name) {
    if (!dir || !name)
        return -1;
    struct fat32_ctx *ctx = (struct fat32_ctx *)dir->priv;
    if (!ctx || !fat32_valid_cluster(ctx, dir->inode))
        return -1;

    vfs_node_t *ex = fat32_finddir(dir, name);
    if (ex) {
        fat_node_free(ex, ctx);
        return -1;
    }

    uint32_t lc, lo;
    int rc = fat32_dir_add(ctx, dir->inode, name, FAT_ATTR_ARCH, 0, 0,
                           &lc, &lo);
    if (rc == 0)
        fat32_sync_fsinfo(ctx);
    return rc;
}

int fat32_mkdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name)
        return -1;
    struct fat32_ctx *ctx = (struct fat32_ctx *)dir->priv;
    if (!ctx || !fat32_valid_cluster(ctx, dir->inode))
        return -1;

    vfs_node_t *ex = fat32_finddir(dir, name);
    if (ex) {
        fat_node_free(ex, ctx);
        return -1;
    }

    uint32_t cl = fat32_alloc_cluster(ctx);
    if (cl == 0)
        return -1;
    fat32_zero_cluster(ctx, cl);

    // "." and ".." in the first sector of the new directory.
    uint8_t sec[512];
    memory_set(sec, 0, sizeof(sec));
    uint8_t *dot  = sec;
    uint8_t *ddot = sec + 32;
    dot[0]  = '.';
    ddot[0] = '.';
    ddot[1] = '.';
    dot[11]  = FAT_ATTR_DIR;
    ddot[11] = FAT_ATTR_DIR;
    fat32_put16(dot  + 20, (uint16_t)(cl >> 16));
    fat32_put16(dot  + 26, (uint16_t)cl);
    fat32_put16(ddot + 20, (uint16_t)(dir->inode >> 16));
    fat32_put16(ddot + 26, (uint16_t)dir->inode);
    blkdev_write(ctx->blk, fat32_cluster_lba(ctx, cl), sec);

    uint32_t lc, lo;
    int rc = fat32_dir_add(ctx, dir->inode, name, FAT_ATTR_DIR, cl, 0,
                           &lc, &lo);
    if (rc != 0) {
        fat32_free_chain(ctx, cl);
        return -1;
    }
    fat32_sync_fsinfo(ctx);
    return 0;
}

// ── delete / rmdir ──────────────────────────────────────────────────────

int fat32_delete(vfs_node_t *dir, const char *name) {
    if (!dir || !name)
        return -1;
    struct fat32_ctx *ctx = (struct fat32_ctx *)dir->priv;
    if (!ctx)
        return -1;

    vfs_node_t *n = fat32_finddir(dir, name);
    if (!n)
        return -1;
    if (n->type == VFS_DIRECTORY) {
        fat_node_free(n, ctx);
        return -1;                   // use rmdir for directories
    }
    struct fat32_file_meta *m = (struct fat32_file_meta *)n->priv;
    if (m) {
        if (fat32_valid_cluster(ctx, n->inode))
            fat32_free_chain(ctx, n->inode);
        fat32_dir_clear(ctx, m->dir_clus, m->entry_off);
    }
    fat_node_free(n, ctx);
    fat32_sync_fsinfo(ctx);
    return 0;
}

int fat32_rmdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name)
        return -1;
    struct fat32_ctx *ctx = (struct fat32_ctx *)dir->priv;
    if (!ctx)
        return -1;

    vfs_node_t *n = fat32_finddir(dir, name);
    if (!n)
        return -1;
    if (n->type != VFS_DIRECTORY) {
        fat_node_free(n, ctx);
        return -1;
    }
    if (fat32_dir_has_entries(ctx, n->inode)) {
        fat_node_free(n, ctx);
        return -1;                   // not empty
    }

    if (fat32_valid_cluster(ctx, n->inode))
        fat32_free_chain(ctx, n->inode);
    fat_node_free(n, ctx);
    fat32_sync_fsinfo(ctx);
    return 0;
}

// ── file write / truncate ───────────────────────────────────────────────

int fat32_write_file(vfs_node_t *node, uint32_t offset, uint32_t size,
                     char *buf) {
    if (!node || !buf)
        return -1;
    if (size == 0)
        return 0;
    if (node->type != VFS_FILE)
        return -1;
    struct fat32_file_meta *m = (struct fat32_file_meta *)node->priv;
    if (!m)
        return -1;
    struct fat32_ctx *ctx = m->ctx;
    if (!ctx)
        return -1;

    uint32_t need_end = offset + size;
    if (need_end < offset)           // overflow
        return -1;
    uint32_t old_sz = node->size;

    // Zero the gap between the old EOF and a higher write offset so no stale
    // bytes survive inside an already-allocated tail cluster.
    if (offset > old_sz)
        fat_store(ctx, node->inode, old_sz, 0, offset - old_sz);

    uint32_t first = node->inode;
    if (need_end > 0) {
        first = fat32_ensure_chain(ctx, first, need_end);
        if (first == 0 && node->inode == 0)
            return -1;               // could not allocate the first cluster
    }
    if (first != node->inode)
        node->inode = first;

    uint32_t done = fat_store(ctx, first, offset, buf, size);

    // Actual capacity of the (possibly grown) chain — the file cannot extend
    // past it when the volume ran out of free clusters.
    uint32_t ncl = 0;
    uint32_t cur = first;
    while (fat32_valid_cluster(ctx, cur)) {
        ncl++;
        uint32_t nxt = fat32_next_cluster(ctx, cur);
        if (nxt == 0 || nxt >= FAT32_EOC_MIN)
            break;
        cur = nxt;
    }
    uint32_t chain_bytes = ncl * ctx->cluster_bytes;

    uint32_t new_sz = old_sz;
    if (chain_bytes >= need_end)
        new_sz = need_end;
    else if (chain_bytes > old_sz)
        new_sz = chain_bytes;        // truncated by disk-full

    node->size = new_sz;
    fat32_patch_entry(ctx, m->dir_clus, m->entry_off, node->inode, new_sz);
    fat32_sync_fsinfo(ctx);

    if (done == 0 && size > 0)
        return -1;
    return (int)done;
}

int fat32_truncate_file(vfs_node_t *node, uint32_t length) {
    if (!node || node->type != VFS_FILE)
        return -1;
    struct fat32_file_meta *m = (struct fat32_file_meta *)node->priv;
    if (!m)
        return -1;
    struct fat32_ctx *ctx = m->ctx;
    if (!ctx)
        return -1;

    uint32_t old_sz = node->size;
    if (length == old_sz)
        return 0;

    if (length > old_sz) {
        uint32_t first = node->inode;
        first = fat32_ensure_chain(ctx, first, length);
        if (node->inode == 0 && first == 0)
            return -1;
        node->inode = first;
        fat_store(ctx, first, old_sz, 0, length - old_sz);
        node->size = length;
        fat32_patch_entry(ctx, m->dir_clus, m->entry_off, first, length);
        fat32_sync_fsinfo(ctx);
        return 0;
    }

    // Shrink: keep ceil(length/cluster) clusters, free the tail chain.
    if (length == 0) {
        if (fat32_valid_cluster(ctx, node->inode))
            fat32_free_chain(ctx, node->inode);
        node->inode = 0;
    } else {
        uint32_t need = (length + ctx->cluster_bytes - 1) / ctx->cluster_bytes;
        uint32_t cl = node->inode;
        uint32_t idx = 1;
        while (idx < need) {
            uint32_t nxt = fat32_next_cluster(ctx, cl);
            if (!fat32_valid_cluster(ctx, nxt))
                return -1;
            cl = nxt;
            idx++;
        }
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        fat32_set_fat(ctx, cl, 0x0FFFFFFF);
        if (fat32_valid_cluster(ctx, nxt))
            fat32_free_chain(ctx, nxt);
    }
    node->size = length;
    fat32_patch_entry(ctx, m->dir_clus, m->entry_off, node->inode, length);
    fat32_sync_fsinfo(ctx);
    return 0;
}
