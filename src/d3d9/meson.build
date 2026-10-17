# ---- Direct3D 9 objects (d3d9.dll, and d3d8.dll on top of them) ----
d3d9_core_lib = static_library('dxup_d3d9_core',
  ['d3d9_interface.cpp', 'd3d9_device.cpp', 'd3d9_resources.cpp'],
  dependencies        : [util_dep, legacy_dep],
  include_directories : ['.', '..'],
)

d3d9_core_dep = declare_dependency(
  link_with           : [d3d9_core_lib],
  dependencies        : [legacy_dep],
  include_directories : ['.'],
)

# ---- d3d9.dll: Direct3D 9 and 9Ex ----
d3d9_dll = shared_library('d3d9',
  ['d3d9_main.cpp', dxup_version_rc.get('d3d9.dll', [])],
  dependencies        : [util_dep, d3d9_core_dep, cpp.find_library('dxguid')],
  include_directories : ['.', '..'],
  name_prefix         : '',
  vs_module_defs      : 'd3d9.def',
  install             : true,
)
