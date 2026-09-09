# 🌵 FAT32-for-Cact

<p align="center">
  <img src="https://img.shields.io/badge/version-0.2.0-orange.svg?style=for-the-badge" alt="Version: 0.2.0">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/format-cctk-green.svg?style=for-the-badge" alt="Output: fat32.cctk">
  <img src="https://img.shields.io/badge/type-filesystem-blue.svg?style=for-the-badge" alt="Filesystem">
</p>

<p align="center">
  <strong>English.</strong> Out-of-tree <strong>FAT32</strong> filesystem → <strong><code>fat32.cctk</code></strong> for the Cact filesystem-module loader (<code>fs_mod</code>).<br>
  <strong>Русский.</strong> Вынесенная из ядра файловая система <strong>FAT32</strong> → <strong><code>fat32.cctk</code></strong>.<br>
  Монтирует EFI System Partition (ESP) через <code>fs_mount(dev)</code>; не-FAT32 устройства отвергаются, чтобы авто-детект пробовал другие модули.
</p>

---

## 🎯 Purpose (Фаза 1: выбор/подготовка EFI/ESP)

| | |
|---|---|
| **ESP mount** | Открывает FAT32-раздел и отдаёт его корень как `vfs_node_t` |
| **Detection** | Строгая проверка BPB (`FAT32`, `root_ent_cnt==0`, `fatsz16==0`, `0x55AA`, FSInfo) — `NULL` на любой не-FAT32 шапке |
| **Directories** | Обход цепочек кластеров, **VFAT LFN** (UTF-16→UTF-8) + 8.3 fallback, case-insensitive `walk` |
| **Files read** | `read` по файлу через FAT-цепочку (частичные чтения, много-кластерные файлы) |
| **Files write** | `create`, `write` (grow/append, sparse-gap zero-fill), `truncate` (up & down), `delete` |
| **Directories write** | `mkdir` (с `"."`/`".."` и LFN-именем), `rmdir` пустых |

Типичный сценарий установщика: перебрать партиции → `fs_mod_mount_type(dev, "fat32")` →
проверить содержимое (например, `/EFI`), затем создать каталоги (`/EFI/BOOT`), удалить
старый загрузчик при необходимости и скопировать `kernel.bin`/`BOOT*.EFI` на ESP
обычными `write`-операциями VFS.

---

## 🔨 Building

**Standalone**

```sh
make install   # auto-detects ../CactKernel-x86_32 and ../LocalRepoCactOS
make clean
```

**Full workspace** — вместе со всеми драйверами (CactOS `DRIVERS` уже включает `FAT32`):

```sh
make -C CactOS-x86_32 iso
```

Override paths if needed: `make KERN_ROOT=/custom/path LOCAL_REPO=/custom/path install`.

---

## 📦 What it produces

| Output | Where it goes | Purpose |
|--------|---------------|---------|
| **`fat32.cctk`** | derived here | Relocatable ELF (ET_REL) loaded by the kernel's `fs_mod` loader |
| installed **`lib/fat32.cctk`** | `$(LOCAL_REPO)/lib/` | Packed into **cctkfs.img** and loaded at boot |

---

## 🔌 Module interface

The kernel calls two exported symbols:

```c
vfs_node_t *fs_mount(struct blkdev *dev);   // mount device, return root or NULL
int         fs_unmount(void);               // teardown (currently a no-op)
```

Directory and file nodes expose the standard `vfs_ops_t`:
`readdir/walk/listdir/create/mkdir/delete/rmdir` on directories and
`read/write/truncate` on files. Undefined kernel symbols (`kmalloc`,
`blkdev_read`, `blkdev_write`, `memory_copy`, …) are resolved at load time via
`ksym_resolve()`.

Implementation notes (no inode table on FAT): file metadata lives in the owning
directory entry; each file node keeps a small meta struct that points at that
entry so `write`/`truncate` can persist size and first-cluster updates. Cluster
allocation scans the FAT (starting from the FSInfo hint), mirrors every FAT
copy, and refreshes the FSInfo free count after each mutating operation.

---

## 🧪 Host test (no kernel needed)

Compiles the real module sources against kernel headers, links them with stubs,
and drives the module on a file-backed ESP image created by `mkfs.fat` + mtools:

```sh
make test
./test/run_test.sh
```

The suite covers: BPB detection, LFN names, 8.3 fallback, cluster-chain file
reads (whole + partial across a cluster boundary), **and the write path**:
mkdir (incl. LFN dirs), create/write (multi-cluster, append), truncate down/up
with zero fill, delete + recreate, re-mount persistence, and rejection of
non-FAT32 media. Afterwards the image is re-read with **mtools** to prove the
module-written FAT32 is interoperable with a third-party FAT implementation.
