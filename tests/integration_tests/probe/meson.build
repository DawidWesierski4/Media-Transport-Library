# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

probe_sources = files('mtl_probe_main.cpp')

probe_cpp_args = ['-Wall', '-Wvla']
if get_option('buildtype') != 'debug'
  probe_cpp_args += ['-Werror']
endif

if not is_windows
  executable('mtl_probe', probe_sources,
    cpp_args : probe_cpp_args,
    include_directories : include_directories('.'),
    install : false,
  )
endif
