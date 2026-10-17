project('dxup', ['c', 'cpp'],
  version : '1.0.0',
  default_options : [
    'cpp_std=c++17',
    'warning_level=2',
    'b_ndebug=if-release',
    'b_vscrt=static_from_buildtype',
  ]
)

# Build options
enable_d3d12_backend = get_option('enable_d3d12_backend')
enable_d3d11         = get_option('enable_d3d11')
enable_d3d10         = get_option('enable_d3d10')
enable_dxgi          = get_option('enable_dxgi')
enable_ddraw         = get_option('enable_ddraw') and target_machine.system() == 'windows'

# Compiler setup
cpp = meson.get_compiler('cpp')

# Global arguments
global_cpp_args = []

if enable_d3d12_backend
  global_cpp_args += ['-DDXUP_D3D12_BACKEND']
endif

add_project_arguments(global_cpp_args, language : 'cpp')

# The DLLs load into foreign processes, whose folders often carry an older
# msvcp140.dll than the toolset's (Heroes of the Storm ships 14.16): the C++
# library and vcruntime are linked in, the Universal CRT (part of Windows)
# stays shared so all modules use one heap.
if cpp.get_argument_syntax() == 'msvc'
  if get_option('buildtype') == 'debug'
    add_project_link_arguments(['/NODEFAULTLIB:libucrtd.lib', 'ucrtd.lib'], language : ['c', 'cpp'])
  else
    add_project_link_arguments(['/NODEFAULTLIB:libucrt.lib', 'ucrt.lib'], language : ['c', 'cpp'])
  endif
  # The DLLs name their PDB without the build machine's folder (debuggers
  # find it by name and signature).
  add_project_link_arguments(['/PDBALTPATH:%_PDB%'], language : ['c', 'cpp'])
endif

# Platform detection
is_windows = target_machine.system() == 'windows'

# Dependencies
if is_windows
  d3d12_dep = cpp.find_library('d3d12', required : enable_d3d12_backend)
  dxgi_dep  = cpp.find_library('dxgi',  required : enable_d3d12_backend)
  d3d11_dep = cpp.find_library('d3d11', required : enable_d3d11)
  d3dcompiler_dep = cpp.find_library('d3dcompiler', required : enable_d3d12_backend)
else
  d3d12_dep = dependency('', required : false)
  dxgi_dep  = dependency('', required : false)
  d3d11_dep = dependency('', required : false)
  d3dcompiler_dep = dependency('', required : false)
endif

# Subdirectories
subdir('src')
