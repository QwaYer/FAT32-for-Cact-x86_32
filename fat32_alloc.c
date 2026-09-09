#include "fat32_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// Write little-endian values into on-disk buffers.
static void fat_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fat_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Update FAT[cluster] in every FAT copy (read-modify-write of the
// containing logical sector).
void fat32_set_fat(struct fat32_ctx *ctx, uint32_t cluster, uint32_t val) {
    if (!ctx || !fat32_valid_cluster(ctx, cluster))
        return;

    uint32_t sec_in_fat = (cluster * 4) / ctx->bps;
    uint32_t in_sec     = (cluster * 4) % ctx->bps;

    uint8_t *buf = (uint8_t *)kmalloc(ctx->bps);
    if (!buf)
        return;

    for (uint32_t k = 0; k < ctx->fats; k++) {
        uint32_t log     = ctx->rsvd_sec + k * ctx->fat_sz + sec_in_fat;
        uint32_t phys    = log * ctx->log_phys;
        if (fat32_read_sectors(ctx, phys, ctx->log_phys, buf) != 0)
            continue;
        fat_put32(buf + in_sec, val);
        for (uint32_t s = 0; s < ctx->log_phys; s++)
            if (blkdev_write(ctx->blk, phys + s, buf + s * 512) != 0)
                break;
    }
    kfree(buf);
}

// Zero the whole cluster (freshly allocated data area).
void fat32_zero_cluster(struct fat32_ctx *ctx, uint32_t cl) {
    if (!ctx || !fat32_valid_cluster(ctx, cl))
        return;
    uint8_t zero[512];
    memory_set(zero, 0, sizeof(zero));
    uint32_t base = fat32_cluster_lba(ctx, cl);
    for (uint32_t s = 0; s < ctx->clus_sec512; s++)
        blkdev_write(ctx->blk, base + s, zero);
}

// Scan FAT entries lo..hi (inclusive) for the first free (0) cluster.
static uint32_t fat_scan_free(struct fat32_ctx *ctx, uint32_t lo, uint32_t hi) {
    if (lo < 2)
        lo = 2;
    uint32_t maxcl = ctx->cluster_count + 1;
    if (hi > maxcl)
        hi = maxcl;
    if (lo > hi)
        return 0;

    uint32_t eps = ctx->bps / 4;               // FAT entries per sector
    uint32_t l0  = (lo * 4) / ctx->bps;
    uint32_t l1  = (hi * 4) / ctx->bps;

    for (uint32_t l = l0; l <= l1; l++) {
        uint8_t *buf = (uint8_t *)kmalloc(ctx->bps);
        if (!buf)
            return 0;
        uint32_t phys = ctx->fat_start512 + l * ctx->log_phys;
        int rc = fat32_read_sectors(ctx, phys, ctx->log_phys, buf);
        if (rc != 0) {
            kfree(buf);
            return 0;
        }
        uint32_t base = l * eps;
        uint32_t c0 = base > lo ? base : lo;
        uint32_t c1 = base + eps - 1;
        if (c1 > hi)
            c1 = hi;
        uint32_t found = 0;
        for (uint32_t c = c0; c <= c1; c++) {
            if (fat32_le32(buf + (c - base) * 4) == 0) {
                found = c;
                break;
            }
        }
        kfree(buf);
        if (found)
            return found;
    }
    return 0;
}

// Allocate one cluster and mark it end-of-chain in every FAT copy.
uint32_t fat32_alloc_cluster(struct fat32_ctx *ctx) {
    if (!ctx)
        return 0;
    uint32_t maxcl = ctx->cluster_count + 1;
    if (maxcl < 2)
        return 0;

    uint32_t start = 2;
    if (ctx->fsi_valid && ctx->fsi_next >= 2 && ctx->fsi_next <= maxcl)
        start = ctx->fsi_next;

    uint32_t c = fat_scan_free(ctx, start, maxcl);
    if (c == 0 && start > 2)
        c = fat_scan_free(ctx, 2, start - 1);
    if (c == 0) {
        printk("[fat32] ERROR: volume full, no free clusters\n");
        return 0;
    }

    fat32_set_fat(ctx, c, 0x0FFFFFFF);
    if (ctx->fsi_valid && ctx->fsi_free)
        ctx->fsi_free--;
    ctx->fsi_next = (c + 1 <= maxcl) ? c + 1 : 2;
    return c;
}

// Release every cluster of a chain back to the free pool.
void fat32_free_chain(struct fat32_ctx *ctx, uint32_t first_cl) {
    if (!ctx || !fat32_valid_cluster(ctx, first_cl))
        return;

    uint32_t cl   = first_cl;
    uint32_t freed = 0;
    uint32_t guard = 0;
    while (fat32_valid_cluster(ctx, cl) && guard++ < 0x100000) {
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        fat32_set_fat(ctx, cl, 0);
        freed++;
        if (nxt == 0 || nxt >= FAT32_EOC_MIN || nxt == FAT32_BAD_CLUSTER)
            break;
        cl = nxt;
    }
    if (freed && ctx->fsi_valid) {
        ctx->fsi_free += freed;
        if (ctx->fsi_free > ctx->cluster_count)
            ctx->fsi_free = ctx->cluster_count;
    }
}

// Grow the chain rooted at `first_cl` so it covers `need_bytes`, zeroing
// every newly allocated cluster. Returns the (possibly new) first cluster,
// or 0 on allocation failure when the chain did not exist yet.
uint32_t fat32_ensure_chain(struct fat32_ctx *ctx, uint32_t first_cl,
                            uint32_t need_bytes) {
    if (!ctx)
        return 0;
    if (need_bytes == 0)
        return first_cl;

    uint32_t need = (need_bytes + ctx->cluster_bytes - 1) / ctx->cluster_bytes;

    uint32_t cl = first_cl;
    uint32_t prev = 0;
    uint32_t count = 0;

    if (cl == 0) {
        cl = fat32_alloc_cluster(ctx);
        if (cl == 0)
            return 0;
        fat32_zero_cluster(ctx, cl);
        prev = cl;
        count = 1;
        first_cl = cl;
    } else {
        uint32_t cur = cl;
        while (fat32_valid_cluster(ctx, cur)) {
            prev = cur;
            count++;
            uint32_t nxt = fat32_next_cluster(ctx, cur);
            if (nxt == 0 || nxt >= FAT32_EOC_MIN)
                break;
            cur = nxt;
        }
    }

    while (count < need) {
        uint32_t nc = fat32_alloc_cluster(ctx);
        if (nc == 0)
            break;                      // out of space
        fat32_zero_cluster(ctx, nc);
        fat32_set_fat(ctx, prev, nc);   // prev -> nc (nc already EOC)
        prev = nc;
        count++;
    }
    return first_cl;
}

// Persist the cached free-count / next-free hint back to the FSInfo sector.
void fat32_sync_fsinfo(struct fat32_ctx *ctx) {
    if (!ctx || !ctx->fsi_valid || ctx->fsinfo_sec == 0)
        return;

    uint8_t *buf = (uint8_t *)kmalloc(ctx->bps);
    if (!buf)
        return;
    memory_set(buf, 0, ctx->bps);

    uint32_t phys = ctx->fsinfo_sec * ctx->log_phys;
    if (fat32_read_sectors(ctx, phys, ctx->log_phys, buf) != 0) {
        kfree(buf);
        return;
    }
    struct fat32_fsinfo *fi = (struct fat32_fsinfo *)buf;
    if (fat32_le32((const uint8_t *)&fi->lead_sig)  == FAT32_FSI_LEAD &&
        fat32_le32((const uint8_t *)&fi->struc_sig) == FAT32_FSI_STRUC) {
        fat_put32((uint8_t *)&fi->free_count, ctx->fsi_free);
        fat_put32((uint8_t *)&fi->next_free,  ctx->fsi_next);
        for (uint32_t s = 0; s < ctx->log_phys; s++)
            if (blkdev_write(ctx->blk, phys + s, buf + s * 512) != 0)
                break;
    }
    kfree(buf);
}

// Update the size/first-cluster fields of a file's directory entry.
void fat32_patch_entry(struct fat32_ctx *ctx, uint32_t dir_clus,
                       uint32_t entry_off, uint32_t cluster, uint32_t size) {
    if (!ctx || !fat32_valid_cluster(ctx, dir_clus))
        return;

    uint32_t sec_idx = entry_off / 512;
    uint32_t in_sec  = entry_off % 512;
    uint8_t  buf[512];
    uint32_t lba = fat32_cluster_lba(ctx, dir_clus) + sec_idx;
    if (blkdev_read(ctx->blk, lba, buf) != 0)
        return;

    uint8_t *e = buf + (in_sec - in_sec % 32);
    fat_put16(e + 20, (uint16_t)(cluster >> 16));
    fat_put16(e + 26, (uint16_t)cluster);
    fat_put32(e + 28, size);
    blkdev_write(ctx->blk, lba, buf);
}
