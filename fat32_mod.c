/*
 * Loadable FAT32 filesystem for the Cact kmod loader.
 *
 * Compiled as a relocatable ELF (ET_REL) and shipped as `fat32.cctk` inside
 * cctkfs.img. The kernel filesystem-module loader (fs_mod) loads this image,
 * relocates it, and resolves the exported `fs_mount` / `fs_unmount` symbols.
 * mntfs then uses `fs_mount(dev)` to mount a FAT32 EFI System Partition
 * (ESP); non-FAT32 devices are rejected so autodetect can probe other
 * modules. Mounts are read/write: the installer can create directories,
 * create/delete files, and copy bootloader payloads onto the ESP.
 */

#include "fat32_internal.h"
#include "fat32.h"
#include "blkdev.h"

/* Exported generic filesystem-module entry: mount block device `dev`. */
vfs_node_t *fs_mount(struct blkdev *dev) {
    return fat32_mount_disk(dev);
}

/* Exported generic filesystem-module entry: teardown. */
int fs_unmount(void) {
    return 0;
}
