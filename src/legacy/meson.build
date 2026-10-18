# The legacy core of Direct3D 7 to 9 (see legacy_device.h). Its shaders are
# compiled with fxc while building: no compiler at run time.
fxc = find_program('fxc', required : true)

legacy_shaders = []
foreach shader : [
    ['ffp_vs', 'vs_5_0', 'vs_main'],
    ['ffp_ps', 'ps_5_0', 'ps_main'],
    ['quad_vs', 'vs_5_0', 'quad_vs'],
    ['clear_ps', 'ps_5_0', 'clear_ps'],
    ['blit_ps', 'ps_5_0', 'blit_ps'],
    ['gamma_ps', 'ps_5_0', 'gamma_ps'],
  ]
  legacy_shaders += custom_target(shader[0],
    input   : 'ffp.hlsl',
    output  : shader[0] + '.h',
    command : [fxc, '/nologo', '/O3', '/T', shader[1], '/E', shader[2], '/Vn', 'g_' + shader[0], '/Fh', '@OUTPUT@', '@INPUT@'],
  )
endforeach

legacy_lib = static_library('dxup_legacy',
  ['legacy_device.cpp', 'd3d9_shader.cpp', 'd3d9_dxbc.cpp', legacy_shaders],
  include_directories : ['.', '..'],
  dependencies        : [util_dep],
)

legacy_dep = declare_dependency(
  link_with           : [legacy_lib],
  include_directories : ['.'],
)
