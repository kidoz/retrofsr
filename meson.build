project(
  'retrofsr',
  'cpp',
  version: '0.2.0',
  license: 'MIT',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'buildtype=release',
  ],
)

retrofsr_inc = include_directories('include')

retrofsr_lib = library(
  'retrofsr',
  'src/retrofsr.cpp',
  include_directories: retrofsr_inc,
  install: true,
)

retrofsr_dep = declare_dependency(
  include_directories: retrofsr_inc,
  link_with: retrofsr_lib,
)

install_headers('include/retrofsr/retrofsr.hpp', subdir: 'retrofsr')

pkg = import('pkgconfig')
pkg.generate(
  retrofsr_lib,
  name: 'retrofsr',
  description: 'Small C++23 CPU spatial upscaler for emulators',
  version: meson.project_version(),
)

if get_option('examples')
  executable(
    'scale_ppm',
    'examples/scale_ppm.cpp',
    dependencies: retrofsr_dep,
    install: false,
  )
endif

if get_option('tests')
  catch2_dep = dependency(
    'catch2-with-main',
    fallback: ['catch2', 'catch2_with_main_dep'],
    default_options: ['tests=false'],
  )
  test_exe = executable(
    'retrofsr_basic_test',
    ['tests/basic.cpp', 'tests/golden.cpp', 'tests/image_quality.cpp'],
    dependencies: [retrofsr_dep, catch2_dep],
    install: false,
  )
  test('basic', test_exe)

  executable(
    'retrofsr_golden_gen',
    'tests/gen_golden_data.cpp',
    dependencies: retrofsr_dep,
    install: false,
  )
endif

if get_option('tools')
  executable(
    'retrofsr_compare',
    'tools/retrofsr_compare.cpp',
    install: false,
  )
endif
