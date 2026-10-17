# ---- d3d8.dll: Direct3D 8 on the Direct3D 9 objects ----
d3d8_dll = shared_library('d3d8',
  ['d3d8_main.cpp', 'd3d8_objects.cpp', dxup_version_rc.get('d3d8.dll', [])],
  dependencies        : [util_dep, d3d9_core_dep, cpp.find_library('dxguid')],
  include_directories : ['.', '..'],
  name_prefix         : '',
  vs_module_defs      : 'd3d8.def',
  install             : true,
)
