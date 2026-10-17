# Version resources (VERSIONINFO) of DXUP's DLLs.
dxup_version_rc = {}
if is_windows
  windows = import('windows')
  version_parts = meson.project_version().split('.')
  foreach dll : ['dxgi.dll', 'd3d11.dll', 'ddraw.dll', 'd3d8.dll', 'd3d9.dll']
    rc = configure_file(
      input         : 'version.rc.in',
      output        : dll + '.rc',
      configuration : {
        'VERSION'        : meson.project_version(),
        'VERSION_COMMAS' : ','.join(version_parts + ['0']),
        'DLL_NAME'       : dll,
      },
    )
    dxup_version_rc += {dll : windows.compile_resources(rc)}
  endforeach
endif

# ---- dxgi.dll ----
dxgi_src = files([
  'dxgi_main.cpp',
  # dxgi_interfaces.h owns concrete D3D11 context/device wrappers. Their
  # emitted vtables call the backend-device resource helpers, so dxgi.dll must
  # carry that implementation as well instead of relying on another DLL's
  # private C++ symbols (which also makes MSVC LTCG builds link correctly).
  'd3d11_backend_device.cpp',
])

dxgi_deps = [util_dep, directx_backend_dep]

# Note: We do NOT link against system dxgi.lib since we ARE dxgi.dll.
# The real system DXGI is loaded dynamically via LoadLibrary.

if enable_d3d12_backend
  dxgi_deps += [d3d12_backend_dep]
endif

dxgi_dll_kwargs = {
  'include_directories' : ['.', '..'],
  'name_prefix'         : '',
  'install'             : true,
}

if is_windows
  dxgi_dll_kwargs += {'vs_module_defs' : 'dxgi.def'}
endif

dxgi_dll = shared_library('dxgi',
  dxgi_src, dxup_version_rc.get('dxgi.dll', []),
  dependencies : dxgi_deps,
  kwargs       : dxgi_dll_kwargs,
)

# ---- d3d11.dll ----
d3d11_src = files([
  'd3d11_main.cpp',
  'd3d11_backend_device.cpp',
  'd3d11_backend_context.cpp',
])

d3d11_deps = [util_dep, directx_backend_dep]

if enable_d3d12_backend
  d3d11_deps += [d3d12_backend_dep]
  if is_windows
    d3d11_deps += [upscale_dep]
  endif
endif

# Note: We do NOT link against system d3d11.lib since we ARE d3d11.dll.
# d3d12.lib and dxgi.lib come through d3d12_backend_dep.

d3d11_dll_kwargs = {
  'include_directories' : ['.', '..'],
  'name_prefix'         : '',
  'install'             : true,
}

if is_windows
  d3d11_dll_kwargs += {
    'vs_module_defs' : 'd3d11.def',
    'link_args'      : ['/MAP'],
  }
endif

d3d11_dll = shared_library('d3d11',
  d3d11_src, dxup_version_rc.get('d3d11.dll', []),
  dependencies : d3d11_deps,
  kwargs       : d3d11_dll_kwargs,
)
