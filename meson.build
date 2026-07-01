project(
  'retrofsr',
  'cpp',
  version: '0.3.0',
  license: 'MIT',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'buildtype=release',
  ],
)

retrofsr_inc = include_directories('include')

# Portable SIMD evaluation outcome: clang/gcc auto-vectorize the Color4 kernels via SLP at
# the default -O3 already, and forcing whole-loop vectorization is rejected by the cost
# model. The simd option therefore enables vectorization-friendly math flags (no errno or
# trap bookkeeping around libm calls) instead of hand-written intrinsic paths; the default
# build is the scalar-fallback configuration of the exact same kernels.
cpp_compiler = meson.get_compiler('cpp')
retrofsr_args = []
if get_option('simd')
  retrofsr_args += cpp_compiler.get_supported_arguments('-fno-math-errno', '-fno-trapping-math')
endif

retrofsr_lib = library(
  'retrofsr',
  'src/retrofsr.cpp',
  include_directories: retrofsr_inc,
  cpp_args: retrofsr_args,
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

if get_option('benchmarks')
  executable(
    'retrofsr_bench',
    'benchmarks/bench.cpp',
    dependencies: retrofsr_dep,
    install: false,
  )
endif
