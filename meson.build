project(
  'retrofsr',
  'cpp',
  version: '0.5.0',
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
  # Pre-1.0 ABI is unstable (Options grows fields across minors); track soversion to
  # the minor so stale binaries fail to load instead of misreading Options.
  version: meson.project_version(),
  soversion: '0.4',
  install: true,
)

retrofsr_dep = declare_dependency(
  include_directories: retrofsr_inc,
  link_with: retrofsr_lib,
)

install_headers('include/retrofsr/retrofsr.hpp', subdir: 'retrofsr')
install_data('LICENSE', install_dir: get_option('datadir') / 'doc' / 'retrofsr')

pkg = import('pkgconfig')
pkg.generate(
  retrofsr_lib,
  name: 'retrofsr',
  description: 'Small C++23 CPU spatial upscaler for emulators',
  version: meson.project_version(),
)

# CLI tests are plain python scripts; skip them gracefully where python3 is absent.
python3 = find_program('python3', required: false)

if get_option('examples')
  scale_ppm_exe = executable(
    'scale_ppm',
    'examples/scale_ppm.cpp',
    dependencies: retrofsr_dep,
    install: false,
  )
  if get_option('tests') and python3.found()
    test('scale-ppm-cli', python3, args: [files('tests/tools/test_scale_ppm.py'), scale_ppm_exe])
  endif
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
  test(
    'basic',
    test_exe,
    env: ['RETROFSR_TEST_DATA=' + meson.project_source_root() / 'tests' / 'data'],
  )

  executable(
    'retrofsr_golden_gen',
    'tests/gen_golden_data.cpp',
    dependencies: retrofsr_dep,
    install: false,
  )
endif

if get_option('tools')
  retrofsr_compare_exe = executable(
    'retrofsr_compare',
    'tools/retrofsr_compare.cpp',
    install: false,
  )
  if get_option('tests') and python3.found()
    test('compare-cli', python3, args: [files('tests/tools/test_compare_cli.py'), retrofsr_compare_exe])
  endif
endif

if get_option('benchmarks')
  executable(
    'retrofsr_bench',
    'benchmarks/bench.cpp',
    dependencies: retrofsr_dep,
    install: false,
  )
endif
