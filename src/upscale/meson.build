# Temporal upscalers: linked into d3d11.dll only, whose backend context runs
# them (dxgi.dll links the backend too and has no use for them).
fs = import('fs')

upscale_args = []
upscale_inc = []
upscale_link_args = []

if host_machine.cpu_family() == 'x86_64'
  dlss_sdk = get_option('dlss_sdk')
  if dlss_sdk != ''
    # NGX's static library comes per C runtime: release (/MT) or debug (/MTd).
    dlss_lib = dlss_sdk / 'lib' / (get_option('buildtype') == 'debug' ? 'nvsdk_ngx_s_dbg.lib' : 'nvsdk_ngx_s.lib')
    if fs.exists(dlss_lib)
      upscale_args += ['-DDXUP_HAVE_DLSS']
      upscale_inc += include_directories(dlss_sdk / 'include')
      upscale_link_args += [dlss_lib]
    else
      warning('dlss_sdk: ' + dlss_lib + ' not found, DLSS left out')
    endif
  endif
  ffx_sdk = get_option('ffx_sdk')
  if ffx_sdk != ''
    upscale_args += ['-DDXUP_HAVE_FFX']
    upscale_inc += include_directories(ffx_sdk / 'api' / 'include', ffx_sdk / 'upscalers' / 'include')
  endif
  xess_sdk = get_option('xess_sdk')
  if xess_sdk != ''
    upscale_args += ['-DDXUP_HAVE_XESS']
    upscale_inc += include_directories(xess_sdk / 'inc')
  endif
endif

upscale_lib = static_library('dxup_upscale',
  files([
    'upscaler.cpp',
    'upscaler_dlss.cpp',
    'upscaler_fsr.cpp',
    'upscaler_xess.cpp',
  ]),
  cpp_args            : upscale_args,
  dependencies        : [util_dep],
  include_directories : upscale_inc + [include_directories('..')],
  pic                 : true,
)

upscale_dep = declare_dependency(
  link_with  : [upscale_lib],
  link_args  : upscale_link_args,
  compile_args : upscale_args,
)
