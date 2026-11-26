# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023-2026 Intel Corporation

project('MtlManager', ['c', 'cpp'],
  default_options : [
    'c_std=gnu11',
    'cpp_std=c++20',
    'buildtype=release',
    'warning_level=2',
  ],
  version : run_command(find_program('cat'), files('../VERSION'), check : true).stdout().strip(),
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

jsonc_dep = dependency('json-c', required : true)
threads_dep = dependency('threads', required : true)

c_args = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wformat', '-Wformat-security',
          '-Werror=format-security', '-fstack-protector-strong']
cpp_args = ['-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wformat', '-Wformat-security',
            '-Werror=format-security', '-fstack-protector-strong']
link_args = ['-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack']

if get_option('enable_asan')
  c_args += ['-fsanitize=address']
  cpp_args += ['-fsanitize=address']
  link_args += ['-fsanitize=address']
endif

inc_dirs = include_directories('.', '../lib/src')

mtlm2_core_sources = files(
  '../lib/src/mt_varlink.c',
  'mtlm2_peer.cpp',
  'mtlm2_shm.cpp',
  'mtlm2_journal.cpp',
  'mtlm2_netdev.cpp',
  'mtlm2_server.cpp',
)

# Keep building mtl.xdp.o when libxdp, libbpf, and clang are present.
libxdp_dep = dependency('libxdp', required : false)
libbpf_dep = dependency('libbpf', required : false)
clang_prog = find_program('clang', required : false)
bpf_deps = []
xdp_cpp_args = [
  '-DMTLM2_XDP_OBJ_INSTALL_PATH="' + (get_option('prefix') / get_option('datadir') / 'mtl/mtl.xdp.o') + '"',
]
bpf_cpp_args = []
xdp_deps = []
if libxdp_dep.found() and libbpf_dep.found()
  xdp_deps += [libxdp_dep, libbpf_dep]
  xdp_cpp_args += ['-DMTL_HAS_XDP_BACKEND']
endif
if libxdp_dep.found() and libbpf_dep.found() and clang_prog.found()
  bpf_target = custom_target('mtl_xdp_bpf',
    input : files('mtl.xdp.c'),
    output : 'mtl.xdp.o',
    command : [clang_prog, '-O2', '-g', '-Wall', '-target', 'bpf', '-c', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default : true,
    install : true,
    install_dir : get_option('datadir') / 'mtl',
  )
  bpf_deps += [bpf_target]
  bpf_cpp_args += ['-DMTLM2_BPF_OBJ_PATH="' + meson.current_build_dir() + '/mtl.xdp.o"']
endif

mtlm2_exe = executable('MtlManager',
  mtlm2_core_sources + files('mtlm2_main.cpp'),
  include_directories : inc_dirs,
  c_args : c_args,
  cpp_args : cpp_args + xdp_cpp_args,
  link_args : link_args,
  dependencies : [jsonc_dep, threads_dep] + xdp_deps,
  install : true,
)

if meson.version().version_compare('>=0.61')
  install_symlink('mtlm2d',
    pointing_to : 'MtlManager',
    install_dir : get_option('bindir'),
  )
endif

if get_option('enable_tests')
  gtest_dep = dependency('gtest', main : true, required : true)
  test_exe = executable('test_mtlm2_pr1a',
    mtlm2_core_sources + files('tests/test_mtlm2_pr1a.cpp'),
    include_directories : inc_dirs,
    c_args : c_args,
    cpp_args : cpp_args + xdp_cpp_args + [
      '-DMTLM2_PACKAGING_DIR="' + meson.current_source_dir() + '/packaging"',
      '-DMTLM2_DAEMON_BIN="' + meson.current_build_dir() + '/MtlManager"',
    ],
    link_args : link_args,
    dependencies : [jsonc_dep, threads_dep, gtest_dep] + xdp_deps,
    install : false,
  )
  test('mtlm2_pr1a', test_exe, depends : [mtlm2_exe], timeout : 60)

  test_pr1b_exe = executable('test_mtlm2_pr1b',
    mtlm2_core_sources + files('tests/test_mtlm2_pr1b.cpp'),
    include_directories : inc_dirs,
    c_args : c_args,
    cpp_args : cpp_args + xdp_cpp_args + bpf_cpp_args + [
      '-DMTLM2_DAEMON_BIN="' + meson.current_build_dir() + '/MtlManager"',
    ],
    link_args : link_args,
    dependencies : [jsonc_dep, threads_dep, gtest_dep] + xdp_deps,
    install : false,
  )
  test('mtlm2_pr1b', test_pr1b_exe, depends : [mtlm2_exe] + bpf_deps, timeout : 60)
endif
