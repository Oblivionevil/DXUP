assert(enable_d3d12_backend, 'the legacy core needs the D3D12 backend (legacy_direct.h)')

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

# The source of the fixed-function pixel shader, for the variants compiled
# while a game runs (Device::selectFfpPixelVariant).
legacy_shaders += custom_target('ffp_source',
  input   : 'ffp.hlsl',
  output  : 'ffp_source.h',
  command : [find_program('python3', 'python'), files('embed_source.py'), '@INPUT@', '@OUTPUT@', 'g_ffp_source'],
)

legacy_lib = static_library('dxup_legacy',
  ['legacy_selftest.cpp', 'd3d9_shader.cpp', 'd3d9_dxbc.cpp', legacy_shaders],
  include_directories : ['.', '..'],
  dependencies        : [util_dep],
)

# Every draw runs through it: no stack cookies, as d3d12_draw_path_lib. The
# shader translators keep the compiler's check.
legacy_draw_path_lib = static_library('dxup_legacy_draw',
  ['legacy_device.cpp', legacy_shaders],
  include_directories : ['.', '..'],
  dependencies        : [util_dep],
  cpp_args            : meson.get_compiler('cpp').get_argument_syntax() == 'msvc' ? ['/GS-'] : [],
)

# The legacy core straight on the D3D12 backend (d3d9.backend): every draw
# runs through it as well. Outside whole-program optimization: profile-guided
# builds compile what their training does not reach for size, and d3d9.backend
# is off in the training so far.
legacy_direct_lib = static_library('dxup_legacy_direct',
  ['legacy_direct.cpp', '../d3d11/d3d11_backend_device.cpp'],
  include_directories : ['.', '..', '../d3d11'],
  dependencies        : [util_dep, directx_backend_dep, d3d12_backend_dep],
  cpp_args            : meson.get_compiler('cpp').get_argument_syntax() == 'msvc' ? ['/GS-', '/GL-'] : [],
)

legacy_dep = declare_dependency(
  link_with           : [legacy_lib, legacy_draw_path_lib, legacy_direct_lib],
  dependencies        : [d3d12_backend_dep, upscale_dep],
  include_directories : ['.'],
)
