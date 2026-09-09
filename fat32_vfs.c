#include "fat32_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// ASCII-only case folding helper for case-insensitive name lookups.
static char fat_fold(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c + 32);
    return c;
}

static int fat_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        uint8_t ua = (uint8_t)fat_fold(*a);
        uint8_t ub = (uint8_t)fat_fold(*b);
        if (ua != ub)
            return (int)ua - (int)ub;
        a++;
        b++;
    }
    return (int)(uint8_t)fat_fold(*a) - (int)(uint8_t)fat_fold(*b);
}

static vfs_dirent_t *fat32_readdir(vfs_node_t *node, uint32_t index);

// ── VFS operation tables ──

static vfs_ops_t fat32_dir_ops = {
    .readdir = fat32_readdir,
    .walk    = fat32_finddir,
    .listdir = fat32_listdir,
    .create  = fat32_create,
    .mkdir   = fat32_mkdir,
    .delete  = fat32_delete,
    .rmdir   = fat32_rmdir,
};

static vfs_ops_t fat32_file_ops = {
    .read     = fat32_read_file,
    .write    = fat32_write_file,
    .truncate = fat32_truncate_file,
};

// Populate a VFS node from a decoded directory entry. Returns 0 or -1.
static int fat32_fill_node(struct fat32_ctx *ctx, vfs_node_t *n,
                           const struct fat32_dirent *de) {
    memory_set(n, 0, sizeof(*n));
    n->inode = de->cluster;
    n->uid   = 0;
    n->gid   = 0;

    int len = 0;
    while (de->name[len] && len < (int)sizeof(n->name) - 1)
        len++;
    memory_copy(n->name, de->name, len);
    n->name[len] = '\0';

    if (de->is_dir) {
        n->type = VFS_DIRECTORY;
        n->ops  = &fat32_dir_ops;
        n->priv = ctx;
        n->mode = 0777;
    } else {
        struct fat32_file_meta *fm =
            (struct fat32_file_meta *)kmalloc(sizeof(*fm));
        if (!fm)
            return -1;
        fm->ctx       = ctx;
        fm->dir_clus  = de->loc_clus;
        fm->entry_off = de->loc_off;

        n->type = VFS_FILE;
        n->ops  = &fat32_file_ops;
        n->priv = fm;
        n->mode = 0666;
        n->size = de->size;
    }
    return 0;
}

// ── readdir ──

typedef struct {
    uint32_t want;
    uint32_t cur;
    vfs_dirent_t de;
    int found;
} fat32_rdctx_t;

static void fat32_readdir_cb(const struct fat32_dirent *de, void *ud) {
    fat32_rdctx_t *rc = (fat32_rdctx_t *)ud;
    if (rc->found)
        return;
    if (rc->cur++ != rc->want)
        return;
    int len = 0;
    while (de->name[len] && len < (int)sizeof(rc->de.name) - 1)
        len++;
    memory_copy(rc->de.name, de->name, len);
    rc->de.name[len] = '\0';
    rc->de.inode = de->cluster;
    rc->found    = 1;
}

static vfs_dirent_t *fat32_readdir(vfs_node_t *node, uint32_t index) {
    static fat32_rdctx_t rc;
    memory_set(&rc, 0, sizeof(rc));
    struct fat32_ctx *ctx = (struct fat32_ctx *)node->priv;
    rc.want = index;
    fat32_dir_iter(ctx, node->inode, fat32_readdir_cb, &rc);
    return rc.found ? &rc.de : 0;
}

// ── walk / finddir ──

typedef struct {
    struct fat32_ctx *ctx;
    const char       *name;
    vfs_node_t       *result;
} fat32_findctx_t;

static void fat32_find_cb(const struct fat32_dirent *de, void *ud) {
    fat32_findctx_t *fc = (fat32_findctx_t *)ud;
    if (fc->result)
        return;
    if (fat_stricmp(de->name, fc->name) != 0)
        return;
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(*n));
    if (!n)
        return;
    if (fat32_fill_node(fc->ctx, n, de) != 0) {
        kfree(n);
        return;
    }
    fc->result = n;
}

vfs_node_t *fat32_finddir(vfs_node_t *dir, const char *name) {
    if (!dir || !name)
        return 0;
    struct fat32_ctx *ctx = (struct fat32_ctx *)dir->priv;
    fat32_findctx_t fc;
    fc.ctx    = ctx;
    fc.name   = name;
    fc.result = 0;
    fat32_dir_iter(ctx, dir->inode, fat32_find_cb, &fc);
    return fc.result;
}

// ── listdir ──

static void fat32_list_cb(const struct fat32_dirent *de, void *ud) {
    (void)ud;
    printk(de->name);
    if (de->is_dir)
        printk("/");
    printk("\n");
}

void fat32_listdir(vfs_node_t *dir) {
    if (!dir)
        return;
    struct fat32_ctx *ctx = (struct fat32_ctx *)dir->priv;
    fat32_dir_iter(ctx, dir->inode, fat32_list_cb, 0);
}

// ── file read ──

int fat32_read_file(vfs_node_t *node, uint32_t offset, uint32_t size,
                    char *buf) {
    if (!node || !buf)
        return -1;
    struct fat32_file_meta *fm = (struct fat32_file_meta *)node->priv;
    if (!fm || !fm->ctx)
        return -1;
    struct fat32_ctx *ctx = fm->ctx;
    uint32_t fsz = node->size;
    if (offset >= fsz || size == 0)
        return 0;
    if (size > fsz - offset)
        size = fsz - offset;

    uint32_t cl = node->inode;
    if (!fat32_valid_cluster(ctx, cl))
        return 0;

    // Position at the cluster containing `offset`.
    uint32_t cloff = 0;
    while (cloff + ctx->cluster_bytes <= offset) {
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        if (nxt == 0 || nxt >= FAT32_EOC_MIN)
            return 0;               // file chain shorter than the request
        cloff += ctx->cluster_bytes;
        cl = nxt;
    }

    uint32_t done = 0;
    uint32_t foff = offset;
    uint8_t sec[512];

    while (done < size) {
        uint32_t rel = foff - cloff;
        if (rel >= ctx->cluster_bytes) {
            uint32_t nxt = fat32_next_cluster(ctx, cl);
            if (nxt == 0 || nxt >= FAT32_EOC_MIN)
                break;              // truncated chain
            cloff += ctx->cluster_bytes;
            cl = nxt;
            continue;
        }

        uint32_t sec_idx = rel / 512;
        uint32_t boff    = rel % 512;
        if (blkdev_read(ctx->blk, fat32_cluster_lba(ctx, cl) + sec_idx, sec) != 0)
            break;

        uint32_t take = 512 - boff;
        if (take > size - done)
            take = size - done;
        if (take > ctx->cluster_bytes - rel)
            take = ctx->cluster_bytes - rel;
        memory_copy(buf + done, sec + boff, take);
        done += take;
        foff += take;
    }
    return (int)done;
}

// Build the root node of a mounted FAT32 volume.
vfs_node_t *fat32_root_node(struct fat32_ctx *ctx) {
    if (!ctx || !fat32_valid_cluster(ctx, ctx->root_cluster))
        return 0;
    vfs_node_t *root = (vfs_node_t *)kmalloc(sizeof(*root));
    if (!root)
        return 0;
    memory_set(root, 0, sizeof(*root));
    copy_string(root->name, "/");
    root->type  = VFS_DIRECTORY;
    root->ops   = &fat32_dir_ops;
    root->priv  = ctx;
    root->inode = ctx->root_cluster;
    root->mode  = 0777;
    root->size  = fat32_dir_chain_size(ctx, ctx->root_cluster);
    return root;
}

// Mount a FAT32 volume from a block device (whole disk or partition).
vfs_node_t *fat32_mount_disk(struct blkdev *dev) {
    if (!dev)
        return 0;

    uint8_t boot[512];
    if (blkdev_read(dev, 0, boot) != 0)
        return 0;

    struct fat32_bpb bpb;
    memory_set(&bpb, 0, sizeof(bpb));
    memory_copy(&bpb, boot, sizeof(bpb));

    if (!fat32_is_fat32(&bpb))
        return 0;                   // not FAT32 — let fs_mod try other modules

    struct fat32_ctx *ctx = (struct fat32_ctx *)kmalloc(sizeof(*ctx));
    if (!ctx)
        return 0;
    if (fat32_ctx_init(ctx, dev, &bpb) != 0) {
        kfree(ctx);
        return 0;
    }

    fat32_read_fsinfo(ctx, fat32_le16((const uint8_t *)&bpb.fsinfo_sec));

    vfs_node_t *root = fat32_root_node(ctx);
    if (!root) {
        kfree(ctx);
        return 0;
    }

    printk("[fat32] mounted ESP volume '");
    printk(ctx->vol_label);
    printk("' (root cluster ");
    {
        char nb[16];
        snprintf(nb, sizeof(nb), "%lu", (unsigned long)ctx->root_cluster);
        printk(nb);
    }
    printk(")\n");
    return root;
}
