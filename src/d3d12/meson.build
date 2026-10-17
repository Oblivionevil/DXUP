d3d12_backend_src = files([
  'd3d12_backend_device.cpp',
  'd3d12_backend_context.cpp',
  'd3d12_backend_upscale.cpp',
  'd3d12_backend_scale.cpp',
  'd3d12_cs_thread.cpp',
  'd3d12_pipeline_cache.cpp',
])

d3d12_backend_deps = [util_dep, directx_backend_dep]

if is_windows
  d3d12_backend_deps += [d3d12_dep, dxgi_dep, d3dcompiler_dep]
endif

d3d12_backend_lib = static_library('dxup_d3d12',
  d3d12_backend_src,
  dependencies        : d3d12_backend_deps,
  include_directories : ['.', '..'],
  pic                 : true,
)

d3d12_backend_dep = declare_dependency(
  link_with           : [d3d12_backend_lib],
  include_directories : ['.'],
)
