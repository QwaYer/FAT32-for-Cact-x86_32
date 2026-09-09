#include "fat32_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// On-disk directory mutation: 8.3 name generation, VFAT long-name encoding,
// free-slot allocation inside a directory chain, entry add/remove.

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

static uint8_t fat_short_checksum(const uint8_t *name) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + name[i]);
    return s;
}

static int fat_isvalid83(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@':
    case '~': case '`': case '!': case '(': case ')': case '{':
    case '}': case '^': case '#': case '&':
        return 1;
    default:
        return 0;
    }
}

// Build the canonical uppercase 8.3 representation of `name`.
// Fills raw[11] (space padded) and canon ("BASE[.EXT]").
static void fat_gen83(const char *name, uint8_t raw[11], char *canon) {
    char base[16], ext[4];
    int  bl = 0, el = 0;

    // Split at the last dot that has at least one trailing character.
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.' && p[1] != '\0' && p != name)
            dot = p;

    int i;
    for (i = 0; i < 16 && name[i] && (!dot || &name[i] < dot); i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        base[bl++] = fat_isvalid83((uint8_t)c) ? c : '_';
    }
    if (dot) {
        for (i = 0; i < 3 && dot[i + 1]; i++) {
            char c = dot[i + 1];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            ext[el++] = fat_isvalid83((uint8_t)c) ? c : '_';
        }
    }

    for (i = 0; i < 11; i++)
        raw[i] = ' ';
    for (i = 0; i < bl && i < 8; i++)
        raw[i] = (uint8_t)base[i];
    for (i = 0; i < el && i < 3; i++)
        raw[8 + i] = (uint8_t)ext[i];

    int cl = 0;
    for (i = 0; i < bl && i < 8; i++)
        canon[cl++] = base[i];
    if (el > 0) {
        canon[cl++] = '.';
        for (i = 0; i < el && i < 3; i++)
            canon[cl++] = ext[i];
    }
    canon[cl] = '\0';
}

// UTF-8 -> UTF-16 code units (BMP only). Returns unit count.
static int fat_utf8_to16(const char *s, uint16_t *u, int cap) {
    int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < cap) {
        uint8_t c = *p;
        if (c < 0x80) {
            u[n++] = c;
            p++;
        } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            u[n++] = (uint16_t)(((c & 0x1F) << 6) | (p[1] & 0x3F));
            p += 2;
        } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            u[n++] = (uint16_t)(((c & 0x0F) << 12) |
                                ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
            p += 3;
        } else {
            u[n++] = '?';
            p++;
        }
    }
    return n;
}

static void fat_lfn_record(uint8_t *rec, uint8_t seq, uint8_t ck,
                           const uint16_t *u) {
    memory_set(rec, 0, 32);
    rec[0]  = seq;
    rec[11] = FAT_ATTR_LFN;
    rec[13] = ck;
    for (int i = 0; i < 5; i++)
        fat_put16(rec + 1 + i * 2, u[i]);
    for (int i = 0; i < 6; i++)
        fat_put16(rec + 14 + i * 2, u[5 + i]);
    for (int i = 0; i < 2; i++)
        fat_put16(rec + 28 + i * 2, u[11 + i]);
}

static void fat_short_record(uint8_t *rec, const uint8_t raw[11],
                             uint8_t attr, uint32_t cluster, uint32_t size) {
    memory_set(rec, 0, 32);
    memory_copy(rec, raw, 11);
    rec[11] = attr;
    fat_put16(rec + 20, (uint16_t)(cluster >> 16));
    fat_put16(rec + 26, (uint16_t)cluster);
    fat_put32(rec + 28, size);
}

// Find m contiguous free slots inside an existing directory chain, or grow
// the chain with a fresh (zeroed) cluster when no run exists.
static int fat_dir_find_run(struct fat32_ctx *ctx, uint32_t dir_clus, int m,
                            uint32_t *loc_clus, uint32_t *loc_off) {
    uint32_t cl = dir_clus, prev = 0;
    uint32_t guard = 0;
    uint8_t  buf[512];

    while (fat32_valid_cluster(ctx, cl) && guard++ < 0x100000) {
        uint32_t base = fat32_cluster_lba(ctx, cl);
        for (uint32_t s = 0; s < ctx->clus_sec512; s++) {
            if (blkdev_read(ctx->blk, base + s, buf) != 0)
                return -1;

            int first_zero = 16;
            for (int slot = 0; slot < 16; slot++)
                if (buf[slot * 32] == 0x00) {
                    first_zero = slot;
                    break;
                }
            int limit = first_zero;
            if (limit > 16 - m)
                limit = 16 - m;
            for (int start = 0; start <= limit; start++) {
                int ok = 1;
                for (int j = 0; j < m; j++) {
                    uint8_t b0 = buf[(start + j) * 32];
                    if (b0 != 0x00 && b0 != 0xE5) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) {
                    *loc_clus = cl;
                    *loc_off  = s * 512 + (uint32_t)start * 32;
                    return 0;
                }
            }
        }
        prev = cl;
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        if (nxt == 0 || nxt >= FAT32_EOC_MIN)
            break;
        cl = nxt;
    }

    // Append a fresh cluster to the directory chain.
    uint32_t nc = fat32_alloc_cluster(ctx);
    if (nc == 0)
        return -1;
    fat32_zero_cluster(ctx, nc);
    if (prev)
        fat32_set_fat(ctx, prev, nc);
    *loc_clus = nc;
    *loc_off  = 0;
    return 0;
}

// Add a directory entry for `name`. `attr` is FAT_ATTR_DIR or FAT_ATTR_ARCH.
// On success loc_clus/loc_off point at the short entry.
int fat32_dir_add(struct fat32_ctx *ctx, uint32_t dir_clus, const char *name,
                  uint8_t attr, uint32_t first_cl, uint32_t size,
                  uint32_t *loc_clus, uint32_t *loc_off) {
    if (!ctx || !name || !loc_clus || !loc_off)
        return -1;
    int len = 0;
    while (name[len])
        len++;
    if (len == 0 || len >= 256)
        return -1;

    uint8_t raw[11];
    char    canon[16];
    fat_gen83(name, raw, canon);
    int need_lfn = compare_string(canon, name) != 0;

    uint16_t units[256];
    int ulen = 0;
    int n = 0;                       // number of LFN entries
    if (need_lfn) {
        ulen = fat_utf8_to16(name, units, 255);
        n = (ulen + 12) / 13;
        if (n > 15)                  // short entry + n LFN must fit a sector
            return -1;
    }
    int m = n + 1;

    uint8_t *recs = (uint8_t *)kmalloc((uint32_t)m * 32);
    if (!recs)
        return -1;

    uint8_t ck = fat_short_checksum(raw);

    // Physical order: the END chunk (seq 1) goes first, the BEGIN chunk
    // (seq 0x40|n) sits directly before the short entry.
    for (int j = 0; j < n; j++) {
        int c = n - 1 - j;          // name chunk index for this record
        uint16_t u[13];
        uint8_t seq;
        for (int k = 0; k < 13; k++) {
            int idx = c * 13 + k;
            if (idx < ulen)
                u[k] = units[idx];
            else if (idx == ulen)
                u[k] = 0;           // end-of-name terminator
            else
                u[k] = 0xFFFF;      // padding
        }
        seq = (c == 0) ? (uint8_t)(0x40 | n) : (uint8_t)(n - c);
        fat_lfn_record(recs + j * 32, seq, ck, u);
    }
    fat_short_record(recs + (m - 1) * 32, raw, attr, first_cl, size);

    uint32_t clus, off;
    if (fat_dir_find_run(ctx, dir_clus, m, &clus, &off) != 0) {
        kfree(recs);
        return -1;
    }

    // Records never straddle a 512-byte boundary (m <= 16).
    uint32_t sec_idx = off / 512;
    uint32_t in_sec  = off % 512;
    uint8_t  buf[512];
    if (blkdev_read(ctx->blk, fat32_cluster_lba(ctx, clus) + sec_idx, buf) != 0) {
        kfree(recs);
        return -1;
    }
    memory_copy(buf + in_sec, recs, (uint32_t)m * 32);
    if (blkdev_write(ctx->blk, fat32_cluster_lba(ctx, clus) + sec_idx, buf) != 0) {
        kfree(recs);
        return -1;
    }
    kfree(recs);

    *loc_clus = clus;
    *loc_off  = off + (uint32_t)(m - 1) * 32;
    return 0;
}

// Mark a short entry and its (contiguous, checksum-matched) LFN chain deleted.
int fat32_dir_clear(struct fat32_ctx *ctx, uint32_t loc_clus,
                    uint32_t loc_off) {
    if (!ctx || !fat32_valid_cluster(ctx, loc_clus))
        return -1;
    if (loc_off + 32 > ctx->cluster_bytes)
        return -1;

    uint8_t *clbuf = (uint8_t *)kmalloc(ctx->cluster_bytes);
    if (!clbuf)
        return -1;

    uint32_t base = fat32_cluster_lba(ctx, loc_clus);
    for (uint32_t s = 0; s < ctx->clus_sec512; s++)
        if (blkdev_read(ctx->blk, base + s, clbuf + s * 512) != 0) {
            kfree(clbuf);
            return -1;
        }

    uint8_t ck = fat_short_checksum(clbuf + loc_off);
    uint32_t off = loc_off;
    for (;;) {
        clbuf[off] = 0xE5;           // deleted marker
        if (off < 32)
            break;
        uint32_t p = off - 32;
        const uint8_t *pe = clbuf + p;
        if (pe[11] != FAT_ATTR_LFN || pe[13] != ck)
            break;
        off = p;
    }

    for (uint32_t s = 0; s < ctx->clus_sec512; s++)
        blkdev_write(ctx->blk, base + s, clbuf + s * 512);
    kfree(clbuf);
    return 0;
}

// 1 if the directory contains any real entry besides "." and "..".
int fat32_dir_has_entries(struct fat32_ctx *ctx, uint32_t dir_clus) {
    if (!ctx || !fat32_valid_cluster(ctx, dir_clus))
        return 1;

    uint32_t cl = dir_clus;
    uint32_t guard = 0;
    uint8_t  buf[512];

    while (fat32_valid_cluster(ctx, cl) && guard++ < 0x100000) {
        uint32_t base = fat32_cluster_lba(ctx, cl);
        for (uint32_t s = 0; s < ctx->clus_sec512; s++) {
            if (blkdev_read(ctx->blk, base + s, buf) != 0)
                return 1;
            for (uint32_t off = 0; off + 32 <= 512; off += 32) {
                const uint8_t *e = buf + off;
                if (e[0] == 0x00)
                    return 0;       // rest of the directory is empty
                if (e[0] == 0xE5)
                    continue;
                if (e[11] == FAT_ATTR_LFN)
                    return 1;       // an LFN always pairs with a real entry
                if (e[0] == 0x2E)
                    continue;       // . and ..
                if (e[11] == FAT_ATTR_VOLUME)
                    continue;
                return 1;
            }
        }
        uint32_t nxt = fat32_next_cluster(ctx, cl);
        if (nxt == 0 || nxt >= FAT32_EOC_MIN)
            break;
        cl = nxt;
    }
    return 0;
}
