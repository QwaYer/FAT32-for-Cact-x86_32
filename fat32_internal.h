#ifndef FAT32_INTERNAL_H
#define FAT32_INTERNAL_H

#include "fat32.h"
#include "blkdev.h"

// Per-mount FAT32 context (lives for the lifetime of the mount).
struct fat32_ctx {
    struct blkdev     *blk;          // owning blkdev (whole disk or partition)
    struct fat32_bpb   bpb;

    uint32_t bps;                    // bytes per logical sector
    uint32_t spc;                    // logical sectors per cluster
    uint32_t log_phys;               // logical sectors per 512-byte device sector
    uint32_t cluster_bytes;
    uint32_t clus_sec512;            // physical 512-byte sectors per cluster
    uint32_t rsvd_sec;               // reserved logical sectors (boot area)
    uint32_t fats;                   // number of FAT copies
    uint32_t fat_sz;                 // logical sectors per FAT copy
    uint32_t fat_start512;           // physical LBA of the first FAT
    uint32_t data_start512;          // physical LBA of the data region
    uint32_t data_sectors512;        // physical sectors in the data region
    uint32_t cluster_count;          // number of clusters in the data region
    uint32_t root_cluster;

    char     vol_label[12];          // BPB label, overridden by root dir entry

    uint32_t fsinfo_sec;             // logical sector of FSInfo (0 = absent)
    int      fsi_valid;
    uint32_t fsi_free;               // free cluster count from FSInfo
    uint32_t fsi_next;               // next free cluster hint from FSInfo
};

// File-node private data: locates the owning directory entry so writes and
// size changes can be persisted (FAT has no inode table to rewrite).
struct fat32_file_meta {
    struct fat32_ctx *ctx;
    uint32_t dir_clus;               // directory cluster holding the entry
    uint32_t entry_off;              // byte offset of the short entry in it
};

// Decoded directory entry handed to iteration callbacks.
struct fat32_dirent {
    char     name[256];
    uint32_t cluster;                // first cluster (0 for an empty file)
    uint32_t size;
    uint8_t  attr;
    int      is_dir;
    uint32_t loc_clus;               // directory cluster that holds the entry
    uint32_t loc_off;                // byte offset of the short entry there
};

typedef void (*fat32_dir_cb_t)(const struct fat32_dirent *de, void *ud);

// --- block / FAT / cluster helpers (fat32_fat.c) ---
uint16_t fat32_le16(const uint8_t *p);   // little-endian field readers
uint32_t fat32_le32(const uint8_t *p);
int      fat32_is_fat32(const struct fat32_bpb *bp);   // pure BPB check
int      fat32_ctx_init(struct fat32_ctx *ctx, struct blkdev *dev,
                        const struct fat32_bpb *bp);   // fill + validate ctx
int      fat32_read_sectors(struct fat32_ctx *ctx, uint32_t lba512,
                            uint32_t count, uint8_t *out);
int      fat32_valid_cluster(struct fat32_ctx *ctx, uint32_t cl);
uint32_t fat32_next_cluster(struct fat32_ctx *ctx, uint32_t cl);
uint32_t fat32_cluster_lba(struct fat32_ctx *ctx, uint32_t cl);
int      fat32_read_fsinfo(struct fat32_ctx *ctx, uint16_t fsinfo_sec);
void     fat32_dir_iter(struct fat32_ctx *ctx, uint32_t dir_cluster,
                        fat32_dir_cb_t cb, void *ud);
uint32_t fat32_dir_chain_size(struct fat32_ctx *ctx, uint32_t dir_cluster);

// --- allocator / FAT mutation (fat32_alloc.c) ---
uint32_t fat32_alloc_cluster(struct fat32_ctx *ctx);
void     fat32_free_chain(struct fat32_ctx *ctx, uint32_t first_cl);
void     fat32_set_fat(struct fat32_ctx *ctx, uint32_t cluster, uint32_t val);
uint32_t fat32_ensure_chain(struct fat32_ctx *ctx, uint32_t first_cl,
                            uint32_t need_bytes);
void     fat32_zero_cluster(struct fat32_ctx *ctx, uint32_t cl);
void     fat32_sync_fsinfo(struct fat32_ctx *ctx);
void     fat32_patch_entry(struct fat32_ctx *ctx, uint32_t dir_clus,
                           uint32_t entry_off, uint32_t cluster, uint32_t size);

// --- directory mutation (fat32_dir.c) ---
int fat32_dir_add(struct fat32_ctx *ctx, uint32_t dir_clus, const char *name,
                  uint8_t attr, uint32_t first_cl, uint32_t size,
                  uint32_t *loc_clus, uint32_t *loc_off);
int fat32_dir_clear(struct fat32_ctx *ctx, uint32_t loc_clus, uint32_t loc_off);
int fat32_dir_has_entries(struct fat32_ctx *ctx, uint32_t dir_clus);

// --- VFS read layer (fat32_vfs.c) ---
vfs_node_t *fat32_root_node(struct fat32_ctx *ctx);
vfs_node_t *fat32_finddir(vfs_node_t *dir, const char *name);
void        fat32_listdir(vfs_node_t *dir);
int         fat32_read_file(vfs_node_t *file, uint32_t offset,
                            uint32_t size, char *buf);

// --- VFS write layer (fat32_write.c) ---
int fat32_create(vfs_node_t *dir, const char *name);
int fat32_mkdir(vfs_node_t *dir, const char *name);
int fat32_delete(vfs_node_t *dir, const char *name);
int fat32_rmdir(vfs_node_t *dir, const char *name);
int fat32_write_file(vfs_node_t *node, uint32_t offset, uint32_t size,
                     char *buf);
int fat32_truncate_file(vfs_node_t *node, uint32_t length);

#endif
