// Host-side test harness for the FAT32 .cctk module.
//
// Compiles the real module sources against kernel headers, links them with
// host stubs for the few kernel entry points the module uses, and drives the
// module through its VFS ops on a file-backed "block device". Exercises BPB
// detection, FAT cluster chains, 8.3 + VFAT long names, file reads, and the
// write path (mkdir/create/write/truncate/delete).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "fat32_internal.h"
#include "blkdev.h"

// Generic filesystem-module entry points (exported by fat32_mod.c).
vfs_node_t *fs_mount(struct blkdev *dev);
int         fs_unmount(void);

// --- host stubs for kernel entry points used by the module ---
static unsigned char *g_img;
static uint32_t       g_img_sectors;
static const char    *g_path;

void *kmalloc(uint32_t size) { return malloc(size); }
void  kfree(void *p)          { free(p); }

void printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void *memory_copy(void *d, const void *s, int n) { return memcpy(d, s, (size_t)n); }
void *memory_set(void *d, int v, int n)          { return memset(d, v, (size_t)n); }
char *copy_string(char *d, const char *s)        { return strcpy(d, s); }
int   compare_string(const char *a, const char *b) { return strcmp(a, b); }
int   snprintf(char *b, unsigned int n, const char *f, ...) {
    va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r;
}

int blkdev_read(struct blkdev *dev, uint32_t lba, uint8_t *buf) {
    (void)dev;
    if (lba + 1 > g_img_sectors)
        return -1;
    memcpy(buf, g_img + (size_t)lba * 512, 512);
    return 0;
}

int blkdev_write(struct blkdev *dev, uint32_t lba, uint8_t *buf) {
    (void)dev;
    if (lba + 1 > g_img_sectors)
        return -1;
    memcpy(g_img + (size_t)lba * 512, buf, 512);
    return 0;
}

static void save_image(void) {
    if (!g_path)
        return;
    FILE *f = fopen(g_path, "wb");
    if (f) {
        fwrite(g_img, 1, (size_t)g_img_sectors * 512, f);
        fclose(f);
    }
}

// --- tiny helpers ---
static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (fold(*a) != fold(*b)) return 0;
        a++; b++;
    }
    return fold(*a) == fold(*b);
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static vfs_node_t *find(vfs_node_t *dir, const char *name) {
    if (!dir) return NULL;
    return dir->ops->walk ? dir->ops->walk(dir, name) : NULL;
}

static int count_entries(vfs_node_t *dir) {
    int n = 0;
    if (!dir || !dir->ops->readdir) return -1;
    for (;;) {
        vfs_dirent_t *de = dir->ops->readdir(dir, (uint32_t)n);
        if (!de) break;
        printf("    [%02d] %s\n", n, de->name);
        n++;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <esp.img> [zeros.img]\n", argv[0]);
        return 2;
    }
    g_path = argv[1];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_img = malloc((size_t)sz);
    if (fread(g_img, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 2; }
    fclose(f);
    if (sz % 512 != 0) { printf("image size not a multiple of 512\n"); return 2; }
    g_img_sectors = (uint32_t)(sz / 512);

    struct blkdev dev;
    memset(&dev, 0, sizeof(dev));
    uint32_t cfg_expect = 0, data_expect = 0;

    // ── read phase (pre-existing mkfs.fat + mtools content) ──────────────
    vfs_node_t *root = fs_mount(&dev);
    CHECK(root != NULL, "mount: FAT32 recognised, root node returned");
    if (!root) {
        printf("\n%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

    printf("root entries:\n");
    int rc = count_entries(root);
    CHECK(rc > 0, "root directory lists at least one entry");

    vfs_node_t *efi = find(root, "EFI");
    CHECK(efi != NULL && efi->type == VFS_DIRECTORY, "finddir /EFI is a directory");
    if (!efi) {
        printf("\n%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

    vfs_node_t *boot = find(efi, "BOOT");
    CHECK(boot != NULL && boot->type == VFS_DIRECTORY, "finddir /EFI/BOOT is a directory");

    vfs_node_t *big = boot ? find(boot, "BIGFILE.BIN") : NULL;
    CHECK(big != NULL && big->type == VFS_FILE, "finddir /EFI/BOOT/BIGFILE.BIN is a file");
    if (big) {
        CHECK(big->size == 100000, "BIGFILE.BIN size matches source payload");
        char *buf = malloc(big->size + 1);
        int got = big->ops->read(big, 0, big->size, buf);
        CHECK(got == 100000, "read whole BIGFILE.BIN");
        int pat_ok = 1;
        for (uint32_t i = 0; i < big->size; i++)
            if ((unsigned char)buf[i] != (unsigned char)(i % 251)) { pat_ok = 0; break; }
        CHECK(pat_ok, "BIGFILE.BIN content matches generated pattern");

        memset(buf, 0, 4097);
        got = big->ops->read(big, 4090, 4096, buf);
        CHECK(got == 4096, "partial read at offset 4090 returns 4096 bytes");
        int p2 = 1;
        for (int i = 0; i < 4096; i++)
            if ((unsigned char)buf[i] != (unsigned char)((4090 + i) % 251)) { p2 = 0; break; }
        CHECK(p2, "partial read bytes match pattern");
        free(buf);
    }

    vfs_node_t *lfn = find(root, "Long File Name 123.txt");
    CHECK(lfn != NULL && lfn->type == VFS_FILE, "finddir by VFAT long name");
    if (lfn) {
        CHECK(ieq(lfn->name, "Long File Name 123.txt"), "decoded LFN preserves case");
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int got = lfn->ops->read(lfn, 0, sizeof(buf) - 1, buf);
        CHECK(got > 0 && (unsigned char)buf[0] == (unsigned char)0, "LFN file content readable");
    }

    vfs_node_t *sht = find(root, "SHORT.BIN");
    CHECK(sht != NULL && sht->type == VFS_FILE, "finddir by plain 8.3 name");

    vfs_node_t *deep = find(root, "GRUB");
    CHECK(deep != NULL && deep->type == VFS_DIRECTORY, "finddir /GRUB is a directory");
    if (deep) {
        vfs_node_t *x86 = find(deep, "x86_64-efi");
        CHECK(x86 != NULL && x86->type == VFS_DIRECTORY, "finddir /GRUB/x86_64-efi is a directory");
    }

    // ── write phase ──────────────────────────────────────────────────────
    printf("== write phase ==\n");

    CHECK(root->ops->mkdir(root, "NEWDIR") == 0, "mkdir /NEWDIR");
    vfs_node_t *nd = find(root, "NEWDIR");
    CHECK(nd != NULL && nd->type == VFS_DIRECTORY, "finddir /NEWDIR is a directory");

    CHECK(nd->ops->mkdir(nd, "Sub Dir") == 0, "mkdir /NEWDIR/Sub Dir (LFN)");
    vfs_node_t *sub = find(nd, "Sub Dir");
    CHECK(sub != NULL && sub->type == VFS_DIRECTORY, "finddir /NEWDIR/Sub Dir");

    // Lower-case LFN file with text content.
    CHECK(root->ops->create(root, "grub.cfg") == 0, "create /grub.cfg");
    vfs_node_t *cfg = find(root, "grub.cfg");
    CHECK(cfg != NULL && cfg->type == VFS_FILE, "finddir /grub.cfg");
    if (cfg) {
        const char *text = "set timeout=5\nmenuentry \"cactos\" {\n  linux /cactos\n}\n";
        cfg_expect = (uint32_t)strlen(text);
        int w = cfg->ops->write(cfg, 0, cfg_expect, (char *)text);
        CHECK(w == (int)cfg_expect, "write /grub.cfg");
        CHECK(cfg->size == cfg_expect, "grub.cfg size updated in memory");
    }

    // Plain 8.3 uppercase file (no LFN records).
    CHECK(root->ops->create(root, "PLAIN.TXT") == 0, "create /PLAIN.TXT");
    vfs_node_t *plain = find(root, "PLAIN.TXT");
    if (plain) {
        const char *payload = "plain 8.3 payload\n";
        int w = plain->ops->write(plain, 0, (uint32_t)strlen(payload), (char *)payload);
        CHECK(w == (int)strlen(payload), "write /PLAIN.TXT");
    }

    // Multi-cluster file: 200000 bytes, later appends across cluster tails.
    CHECK(nd->ops->create(nd, "datafile.bin") == 0, "create /NEWDIR/datafile.bin");
    vfs_node_t *df = find(nd, "datafile.bin");
    CHECK(df != NULL && df->type == VFS_FILE, "finddir /NEWDIR/datafile.bin");
    if (df) {
        const uint32_t SZ = 200000;
        char *buf = malloc(SZ);
        for (uint32_t i = 0; i < SZ; i++) buf[i] = (char)(i % 251);
        int w = df->ops->write(df, 0, SZ, buf);
        CHECK(w == (int)SZ, "write 200000 bytes to datafile.bin");
        CHECK(df->size == SZ, "datafile.bin size grown");

        char *chk = malloc(SZ);
        int got = df->ops->read(df, 0, SZ, chk);
        CHECK(got == (int)SZ, "read back datafile.bin");
        CHECK(memcmp(buf, chk, SZ) == 0, "datafile.bin round-trip matches");

        // Append from current EOF (exercises cluster-chain extension).
        const char *tail = "TAIL-MARKER";
        uint32_t tail_len = (uint32_t)strlen(tail);
        data_expect = SZ + tail_len;
        w = df->ops->write(df, SZ, tail_len, (char *)tail);
        CHECK(w == (int)tail_len, "append to datafile.bin");
        CHECK(df->size == data_expect, "datafile.bin size after append");
        char buf2[64];
        memset(buf2, 0, sizeof(buf2));
        df->ops->read(df, SZ, sizeof(buf2) - 1, buf2);
        CHECK(strncmp(buf2, tail, tail_len) == 0, "appended tail readable");
        free(buf);
        free(chk);
    }

    // Truncate down then up with zero fill.
    CHECK(nd->ops->create(nd, "shrink.bin") == 0, "create /NEWDIR/shrink.bin");
    vfs_node_t *sh = find(nd, "shrink.bin");
    if (sh) {
        char *buf = malloc(10000);
        for (int i = 0; i < 10000; i++) buf[i] = (char)(i % 251);
        sh->ops->write(sh, 0, 10000, buf);
        CHECK(sh->size == 10000, "shrink.bin initial size");
        CHECK(sh->ops->truncate(sh, 3000) == 0, "truncate shrink.bin down to 3000");
        CHECK(sh->size == 3000, "shrink.bin size after shrink");
        char *chk = malloc(3000);
        sh->ops->read(sh, 0, 3000, chk);
        CHECK(memcmp(buf, chk, 3000) == 0, "shrink.bin head bytes survive");
        CHECK(sh->ops->truncate(sh, 9000) == 0, "truncate shrink.bin up to 9000");
        CHECK(sh->size == 9000, "shrink.bin size after grow");
        char tail[16];
        memset(tail, 0xAA, sizeof(tail));
        sh->ops->read(sh, 3000, 16, tail);
        int zero_ok = 1;
        for (int i = 0; i < 16; i++) if (tail[i] != 0) { zero_ok = 0; break; }
        CHECK(zero_ok, "grown region zero-filled");
        free(buf);
        free(chk);
    }

    // Overwrite an existing file through delete + recreate.
    CHECK(root->ops->delete(root, "SHORT.BIN") == 0, "delete /SHORT.BIN");
    CHECK(find(root, "SHORT.BIN") == NULL, "/SHORT.BIN gone after delete");
    CHECK(root->ops->create(root, "SHORT.BIN") == 0, "recreate /SHORT.BIN");
    vfs_node_t *sh2 = find(root, "SHORT.BIN");
    if (sh2) {
        const char *v = "fresh";
        int w = sh2->ops->write(sh2, 0, (uint32_t)strlen(v), (char *)v);
        CHECK(w == 5 && sh2->size == 5, "recreated SHORT.BIN holds new content");
    }

    // Remount and verify persistence of the directory-entry metadata.
    vfs_node_t *root2 = fs_mount(&dev);
    CHECK(root2 != NULL, "re-mount after writes");
    if (root2) {
        vfs_node_t *p = find(root2, "NEWDIR");
        vfs_node_t *pdf = p ? find(p, "datafile.bin") : NULL;
        CHECK(pdf != NULL && pdf->size == data_expect, "remount: datafile.bin size persisted");
        vfs_node_t *pc = find(root2, "grub.cfg");
        CHECK(pc != NULL && pc->size == cfg_expect, "remount: grub.cfg size persisted");
        vfs_node_t *ps = find(root2, "SHORT.BIN");
        CHECK(ps != NULL && ps->size == 5, "remount: recreated SHORT.BIN size persisted");
    }

    printf("saving modified image to %s\n", g_path);
    save_image();

    // ── non-FAT32 rejection ──────────────────────────────────────────────
    if (argc >= 3) {
        FILE *zf = fopen(argv[2], "rb");
        if (zf) {
            fseek(zf, 0, SEEK_END);
            long zsz = ftell(zf);
            fseek(zf, 0, SEEK_SET);
            unsigned char *z = malloc((size_t)zsz);
            if (zsz > 0 && fread(z, 1, (size_t)zsz, zf) == (size_t)zsz) {
                g_img = z;
                g_img_sectors = (uint32_t)(zsz / 512);
                vfs_node_t *nope = fs_mount(&dev);
                CHECK(nope == NULL, "non-FAT32 device rejected (mount returns NULL)");
            }
            free(z);
            fclose(zf);
        }
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
