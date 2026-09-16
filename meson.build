# ==============================================================================
# fat32 module — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dkern_root=../CactKernel-x86_32 -Dlocal_repo=../LocalRepoCactOS-x86_32
#   ninja -C build-meson             # build-meson/fat32.cctk
#   ninja -C build-meson stage       # copy into <local_repo>/lib
#   ninja -C build-meson test_fat32  # host-side test binary (not built by default)
#
# The module is several translation units linked with `ld -r` into a single
# relocatable object (ET_REL), which the kernel's module loader maps and
# relocates.  The host test compiles the same sources with the build machine's
# compiler and links against the host libc.
# ==============================================================================

project('fat32-mod', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=1',        # -Wall, as in the Makefile
    'buildtype=plain',
  ])

fs = import('fs')
bash = find_program('bash')
cc = meson.get_compiler('c')
ld = find_program('ld')

src_root = meson.project_source_root()

# Path options are relative to this repo's root unless absolute.
_kern = get_option('kern_root')
kern_root = _kern.startswith('/') ? _kern : src_root / _kern
_local = get_option('local_repo')
local_repo = _local.startswith('/') ? _local : src_root / _local
install_dir = local_repo / 'lib'

# Kernel headers this module (and its host test) compiles against.
mod_includes = [
  kern_root / 'Cact/kernel/core',
  kern_root / 'Cact/kernel/memory',
  kern_root / 'Cact/kernel/cpudev',
  kern_root / 'Cact/drivers/pci',
  kern_root / 'Cact/drivers/block/blkdev',
  kern_root / 'Cact/drivers/block/pagecache',
  kern_root / 'Cact/fs/vfs',
]

mod_c_args = [
  '-ffreestanding',
  '-fno-pie',
  '-fno-stack-protector',
  '-nostdlib',
  # Ring 0: the kernel has not enabled SSE for itself, so clang must not lower
  # struct copies to SSE2 (the same trap the kernel build hits).
  '-mno-mmx',
  '-mno-sse',
  '-mno-sse2',
  '-O2',
]
foreach d : mod_includes
  mod_c_args += '-I' + d
endforeach
mod_c_args += '-I' + src_root

# Same order as the Makefile's F32_SRCS.
fat32_sources = [
  'fat32_fat.c',
  'fat32_alloc.c',
  'fat32_dir.c',
  'fat32_vfs.c',
  'fat32_write.c',
  'fat32_mod.c',
]

fat32_objs = []
foreach s : fat32_sources
  fat32_objs += custom_target('obj-' + s.underscorify(),
    input: s,
    output: s + '.o',
    command: cc.cmd_array() + ['-m32'] + mod_c_args +
             ['-MD', '-MF', '@DEPFILE@', '-c', '@INPUT@', '-o', '@OUTPUT@'],
    depfile: s + '.o.d')
endforeach

fat32_cctk = custom_target('fat32.cctk',
  input: fat32_objs,
  output: 'fat32.cctk',
  command: [ld, '-m', 'elf_i386', '-r', '-o', '@OUTPUT@'] + fat32_objs,
  build_by_default: true)

# Meson reserves the name "install" for itself, hence "stage".
run_target('stage',
  command: [bash, '-c', 'set -e; mkdir -p "$1"; cp -f "$2" "$1/fat32.cctk"',
            'bash', install_dir, fat32_cctk])

# ------------------------------------------------------------------------------
# Host-side test (the Makefile's `test` target)
#
# Compiled by the build machine's own compiler against the host libc, so it is a
# native target — the module sources are compiled a second time here.
# ------------------------------------------------------------------------------

test_c_args = [
  '-m32',
  '-fno-pie',
  '-fno-builtin',
  '-O0',
  '-g',
]
foreach d : mod_includes
  test_c_args += '-I' + d
endforeach
test_c_args += '-I' + src_root

executable('test_fat32',
  fat32_sources + files('test/test_fat32.c'),
  native: true,
  c_args: test_c_args,
  link_args: ['-m32', '-no-pie'],
  build_by_default: false)
