#include "fat32_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// Little-endian field readers (module runs on i386; kept explicit on purpose).
uint16_t fat32_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t fat32_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fat_pow2(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

// Pure BPB classification: is this boot sector a FAT32 volume?
int fat32_is_fat32(const struct fat32_bpb *bp) {
    if (!bp)
        return 0;
    if (fat32_le16((const uint8_t *)&bp->byts_per_sec) < 512)
        return 0;
    if (fat32_le16((const uint8_t *)&bp->byts_per_sec) > 4096)
        return 0;
    if (!fat_pow2(fat32_le16((const uint8_t *)&bp->byts_per_sec)))
        return 0;

    // FAT32 discriminators: 16-bit FAT size and root entry count are zero.
    if (fat32_le16((const uint8_t *)&bp->fatsz16) != 0)
        return 0;
    if (fat32_le16((const uint8_t *)&bp->root_ent_cnt) != 0)
        return 0;
    if (fat32_le32((const uint8_t *)&bp->fatsz32) == 0)
        return 0;

    uint8_t spc = bp->sec_per_clus;
    if (spc == 0 || !fat_pow2(spc))
        return 0;
    if (fat32_le16((const uint8_t *)&bp->byts_per_sec) * spc > FAT32_MAX_CLUST_SZ)
        return 0;
    if (bp->num_fats == 0 || bp->num_fats > 4)
        return 0;
    if (fat32_le16((const uint8_t *)&bp->rsvd_sec_cnt) == 0)
        return 0;

    uint32_t total = fat32_le16((const uint8_t *)&bp->tot_sec16);
    if (total == 0)
        total = fat32_le32((const uint8_t *)&bp->tot_sec32);
    if (total == 0)
        return 0;

    uint32_t data = (uint32_t)fat32_le16((const uint8_t *)&bp->rsvd_sec_cnt) +
                    (uint32_t)bp->num_fats * fat32_le32((const uint8_t *)&bp->fatsz32);
    if (data >= total)
        return 0;
    if ((total - data) / spc == 0)
        return 0;

    // Optional textual tag; numeric checks above are the real gate.
    int i;
    for (i = 0; i < 8; i++)
        if (bp->fs_type[i] != "FAT32   "[i])
            break;
    if (i < 8) {
        uint8_t sig = bp->boot_sig;
        uint16_t bs = fat32_le16((const uint8_t *)&bp->boot_sign);
        if (sig != 0x28 && sig != 0x29)
            return 0;
        if (bs != FAT32_BOOT_SIGN)
            return 0;
    }
    return 1;
}

// Derive and validate all geometry fields from a parsed BPB.
int fat32_ctx_init(struct fat32_ctx *ctx, struct blkdev *dev,
                   const struct fat32_bpb *bp) {
    if (!ctx || !dev || !bp)
        return -1;

    memory_set(ctx, 0, sizeof(*ctx));
    ctx->blk = dev;
    memory_copy(&ctx->bpb, bp, sizeof(*bp));

    uint32_t bps  = fat32_le16((const uint8_t *)&bp->byts_per_sec);
    uint32_t spc  = bp->sec_per_clus;
    uint32_t rsvd = fat32_le16((const uint8_t *)&bp->rsvd_sec_cnt);
    uint32_t fats = bp->num_fats;
    uint32_t fsz  = fat32_le32((const uint8_t *)&bp->fatsz32);
    uint32_t root = fat32_le32((const uint8_t *)&bp->root_clus);

    uint32_t total = fat32_le16((const uint8_t *)&bp->tot_sec16);
    if (total == 0)
        total = fat32_le32((const uint8_t *)&bp->tot_sec32);

    ctx->bps          = bps;
    ctx->spc          = spc;
    ctx->log_phys     = bps / 512;
    ctx->cluster_bytes = bps * spc;
    ctx->clus_sec512  = spc * ctx->log_phys;
    ctx->rsvd_sec     = rsvd;
    ctx->fats         = fats;
    ctx->fat_sz       = fsz;
    ctx->fat_start512 = rsvd * ctx->log_phys;

    uint32_t data_log = rsvd + fats * fsz;
    ctx->data_start512   = data_log * ctx->log_phys;
    ctx->data_sectors512 = (total - data_log) * ctx->log_phys;
    ctx->cluster_count   = (total - data_log) / spc;
    ctx->root_cluster    = root;
    ctx->fsinfo_sec      = fat32_le16((const uint8_t *)&bp->fsinfo_sec);

    // Root directory must be a real cluster inside the data region.
    if (root < 2 || root > ctx->cluster_count + 1)
        return -1;

    // BPB volume label, trimmed of trailing spaces.
    int i = 0, n = 0;
    for (i = 0; i < 11 && bp->vol_lab[i]; i++)
        if (bp->vol_lab[i] != ' ')
            n = i + 1;
    for (i = 0; i < n; i++)
        ctx->vol_label[i] = bp->vol_lab[i];
    ctx->vol_label[n] = '\0';
    if (n == 0)
        copy_string(ctx->vol_label, "NO NAME");

    return 0;
}

// Read `count` consecutive 512-byte device sectors.
int fat32_read_sectors(struct fat32_ctx *ctx, uint32_t lba512,
                       uint32_t count, uint8_t *out) {
    if (!ctx || !out)
        return -1;
    for (uint32_t i = 0; i < count; i++)
        if (blkdev_read(ctx->blk, lba512 + i, out + i * 512) != 0)
            return -1;
    return 0;
}

int fat32_valid_cluster(struct fat32_ctx *ctx, uint32_t cl) {
    if (!ctx)
        return 0;
    return cl >= 2 && cl <= ctx->cluster_count + 1 &&
           cl <= FAT32_MAX_CLUSTER;
}

uint32_t fat32_cluster_lba(struct fat32_ctx *ctx, uint32_t cl) {
    if (!fat32_valid_cluster(ctx, cl))
        return 0;
    return ctx->data_start512 + (cl - 2) * ctx->clus_sec512;
}

// FAT[cl] value: next cluster in the chain, 0x0FFFFFF7 (bad),
// or >= 0x0FFFFFF8 for end-of-chain.
uint32_t fat32_next_cluster(struct fat32_ctx *ctx, uint32_t cl) {
    if (!ctx || !fat32_valid_cluster(ctx, cl))
        return 0;

    uint32_t byte_off = cl * 4;
    uint32_t log_sec  = byte_off / ctx->bps;
    uint32_t in_sec   = byte_off % ctx->bps;
    uint32_t phys     = ctx->fat_start512 + log_sec * ctx->log_phys;

    uint8_t *buf = (uint8_t *)kmalloc(ctx->bps);
    if (!buf)
        return 0;
    uint32_t val = 0;
    if (fat32_read_sectors(ctx, phys, ctx->log_phys, buf) == 0)
        val = fat32_le32(buf + in_sec);
    kfree(buf);
    return val;
}

// Optional FSInfo: free-cluster count and next-free hint.
int fat32_read_fsinfo(struct fat32_ctx *ctx, uint16_t fsinfo_sec) {
    if (!ctx || fsinfo_sec == 0)
        return -1;

    uint8_t *buf = (uint8_t *)kmalloc(ctx->bps);
    if (!buf)
        return -1;
    memory_set(buf, 0, ctx->bps);

    int rc = -1;
    uint32_t phys = (uint32_t)fsinfo_sec * ctx->log_phys;
    if (fat32_read_sectors(ctx, phys, ctx->log_phys, buf) == 0) {
        struct fat32_fsinfo *fi = (struct fat32_fsinfo *)buf;
        if (fat32_le32((const uint8_t *)&fi->lead_sig)  == FAT32_FSI_LEAD &&
            fat32_le32((const uint8_t *)&fi->struc_sig) == FAT32_FSI_STRUC &&
            fat32_le32((const uint8_t *)&fi->free_count) != 0xFFFFFFFFu) {
            ctx->fsi_valid = 1;
            ctx->fsi_free  = fat32_le32((const uint8_t *)&fi->free_count);
            ctx->fsi_next  = fat32_le32((const uint8_t *)&fi->next_free);
            rc = 0;
        }
    }
    kfree(buf);
    return rc;
}

// --- directory entry decoding ---

// One 13-code-unit chunk of a VFAT long name.
struct fat_lfn_piece {
    uint16_t u[13];
};

static uint8_t fat_short_checksum(const uint8_t *name) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + name[i]);
    return s;
}

static void fat_append_utf8(char *out, int *pos, uint16_t c) {
    if (c < 0x80) {
        out[(*pos)++] = (char)c;
    } else if (c < 0x800) {
        out[(*pos)++] = (char)(0xC0 | (c >> 6));
        out[(*pos)++] = (char)(0x80 | (c & 0x3F));
    } else {
        out[(*pos)++] = (char)(0xE0 | (c >> 12));
        out[(*pos)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (c & 0x3F));
    }
}

// Reassemble a long name. VFAT stores the chunks in reverse order: the
// physically first LFN entry carries the end of the name.
static void fat_build_lfn(const struct fat_lfn_piece *pieces, int np, char *out) {
    int pos = 0;
    for (int i = np - 1; i >= 0 && pos < FAT32_MAX_NAME; i--) {
        for (int j = 0; j < 13; j++) {
            uint16_t c = pieces[i].u[j];
            if (c == 0xFFFF)
                continue;             // padding after the terminator
            if (c == 0)
                goto done;
            if (pos + 3 >= FAT32_MAX_NAME)
                goto done;
            fat_append_utf8(out, &pos, c);
        }
    }
done:
    out[pos] = '\0';
}

// Decode an 8.3 short name (11 raw bytes) into "base[.ext]".
static void fat_decode_short(const uint8_t *raw, uint8_t nt_res, char *out) {
    int pos = 0;
    int lower_base = (nt_res & 0x08) != 0;
    int lower_ext  = (nt_res & 0x10) != 0;

    for (int i = 0; i < 8 && pos < 8; i++) {
        uint8_t c = raw[i];
        if (i == 0 && c == 0x05)
            c = 0xE5;                 // 0x05 is stored instead of 0xE5
        if (c == ' ')
            break;
        if (lower_base && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        out[pos++] = (char)c;
    }

    char ext[4];
    int ep = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t c = raw[8 + i];
        if (c == ' ')
            break;
        if (lower_ext && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        ext[ep++] = (char)c;
    }
    if (ep > 0) {
        out[pos++] = '.';
        for (int i = 0; i < ep; i++)
            out[pos++] = ext[i];
    }
    out[pos] = '\0';
}

static int fat_name_is_dot(const char *n) {
    if (n[0] != '.')
        return 0;
    if (n[1] == '\0')
        return 1;
    return n[1] == '.' && n[2] == '\0';
}

// Walk every entry of the directory whose chain starts at `dir_cluster`.
// ".", "..", volume labels and freed entries are filtered here; real entries
// are reported through the callback.
void fat32_dir_iter(struct fat32_ctx *ctx, uint32_t dir_cluster,
                    fat32_dir_cb_t cb, void *ud) {
    if (!ctx || !cb || !fat32_valid_cluster(ctx, dir_cluster))
        return;

    struct fat_lfn_piece pieces[20];
    int np = 0;
    uint8_t lfn_ck = 0;
    uint32_t cl = dir_cluster;
    uint32_t guard = 0;
    uint8_t buf[512];

    while (fat32_valid_cluster(ctx, cl) && guard++ < 0x100000) {
        uint32_t base = fat32_cluster_lba(ctx, cl);

        for (uint32_t s = 0; s < ctx->clus_sec512; s++) {
            if (blkdev_read(ctx->blk, base + s, buf) != 0)
                return;

            for (uint32_t off = 0; off + 32 <= 512; off += 32) {
                const uint8_t *e = buf + off;
                uint8_t c0 = e[0];

                if (c0 == 0x00)                // rest of the directory is empty
                    return;
                if (c0 == 0xE5) {              // freed entry
                    np = 0;
                    continue;
                }

                uint8_t attr = e[11];

                if (attr == FAT_ATTR_LFN) {
                    if (np < 20 && c0 != 0x00 && c0 != 0xE5) {
                        struct fat_lfn_piece *p = &pieces[np];
                        for (int j = 0; j < 5; j++)
                            p->u[j] = fat32_le16(e + 1 + j * 2);
                        for (int j = 0; j < 6; j++)
                            p->u[5 + j] = fat32_le16(e + 14 + j * 2);
                        for (int j = 0; j < 2; j++)
                            p->u[11 + j] = fat32_le16(e + 28 + j * 2);
                        if (np == 0)
                            lfn_ck = e[13];
                        np++;
                    }
                    continue;
                }

                // A real (short) entry terminates any pending long name;
                // the LFN checksum must match this entry's 11-byte name.
                char name[256];
                if (np > 0 && lfn_ck == fat_short_checksum(e)) {
                    fat_build_lfn(pieces, np, name);
                } else {
                    fat_decode_short(e, e[12], name);
                }
                np = 0;

                if (fat_name_is_dot(name))
                    continue;

                if (attr == FAT_ATTR_VOLUME) {
                    // Volume label entry lives at the root directory.
                    if (dir_cluster == ctx->root_cluster) {
                        int k, len = 0;
                        for (k = 0; k < 11; k++)
                            if (e[k] != ' ')
                                len = k + 1;
                        for (k = 0; k < len && k < 11; k++)
                            ctx->vol_label[k] = (char)e[k];
                        ctx->vol_label[k] = '\0';
                    }
                    continue;
                }

                struct fat32_dirent de;
                memory_set(&de, 0, sizeof(de));
                memory_copy(de.name, name, sizeof(de.name) - 1);
                de.cluster = ((uint32_t)fat32_le16(e + 20) << 16) |
                              fat32_le16(e + 26);
                de.size    = fat32_le32(e + 28);
                de.attr    = attr;
                de.is_dir  = (attr & FAT_ATTR_DIR) != 0;
                de.loc_clus = cl;
                de.loc_off  = s * 512 + off;
                cb(&de, ud);
            }
        }

        uint32_t nxt = fat32_next_cluster(ctx, cl);
        if (nxt == 0 || nxt >= FAT32_EOC_MIN)
            break;
        cl = nxt;
    }
}

// Total size of a directory chain in bytes (used only for reporting).
uint32_t fat32_dir_chain_size(struct fat32_ctx *ctx, uint32_t cl) {
    uint32_t size = 0;
    uint32_t guard = 0;
    while (fat32_valid_cluster(ctx, cl) && guard++ < 0x100000) {
        size += ctx->cluster_bytes;
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        if (nxt == 0 || nxt >= FAT32_EOC_MIN)
            break;
        cl = nxt;
    }
    return size;
}
