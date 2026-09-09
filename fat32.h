#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "vfs.h"

struct blkdev;   // kernel block device (opaque handle passed to fs_mount)

// FAT32 on-disk constants
#define FAT32_BOOT_SIGN    0xAA55
#define FAT32_EOC_MIN      0x0FFFFFF8u      // first "end of chain" cluster value
#define FAT32_BAD_CLUSTER  0x0FFFFFF7u
#define FAT32_MAX_CLUSTER  0x0FFFFFF6u      // largest valid cluster number
#define FAT32_MAX_CLUST_SZ 32768u           // read limit: cluster bytes
#define FAT32_MAX_NAME     255

// Directory-entry attributes
#define FAT_ATTR_RO     0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME 0x08
#define FAT_ATTR_DIR    0x10
#define FAT_ATTR_ARCH   0x20
#define FAT_ATTR_LFN    0x0F

// FSInfo signatures (little-endian dwords)
#define FAT32_FSI_LEAD   0x41615252u
#define FAT32_FSI_STRUC  0x61417272u
#define FAT32_FSI_TRAIL  0xAA550000u

// BIOS parameter block (512-byte boot sector, packed like ext4 superblock)
struct fat32_bpb {
    uint8_t  bsjmp[3];
    char     bsoem[8];
    uint16_t byts_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec16;
    uint8_t  media;
    uint16_t fatsz16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec32;
    uint32_t fatsz32;
    uint16_t ext_flags;
    uint16_t fs_ver;
    uint32_t root_clus;
    uint16_t fsinfo_sec;
    uint16_t bk_boot_sec;
    uint8_t  bs_reserved[12];
    uint8_t  drv_num;
    uint8_t  bs_reserved1;
    uint8_t  boot_sig;
    uint32_t vol_id;
    char     vol_lab[11];
    char     fs_type[8];
    uint8_t  boot_code[420];
    uint16_t boot_sign;
} __attribute__((packed));

// FSInfo sector layout
struct fat32_fsinfo {
    uint32_t lead_sig;
    uint8_t  fsi_reserved1[480];
    uint32_t struc_sig;
    uint32_t free_count;
    uint32_t next_free;
    uint8_t  fsi_reserved2[12];
    uint32_t trail_sig;
} __attribute__((packed));

// 32-byte directory entry
struct fat32_dir_ent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed));

// Public module API (mirrors ext4.h)
vfs_node_t *fat32_mount_disk(struct blkdev *dev);

#endif
