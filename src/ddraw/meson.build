# ---- ddraw.dll: DirectDraw 7 and Direct3D 7 ----
ddraw_dll = shared_library('ddraw',
  ['ddraw_main.cpp', 'ddraw_objects.cpp', dxup_version_rc.get('ddraw.dll', [])],
  dependencies        : [util_dep, legacy_dep, cpp.find_library('dxguid')],
  include_directories : ['.', '..'],
  name_prefix         : '',
  vs_module_defs      : 'ddraw.def',
  install             : true,
)
