# Utility library (always built)
subdir('util')

# DirectX backend abstraction (always built)
subdir('directx')

# D3D12 backend
if enable_d3d12_backend
  subdir('d3d12')
  if is_windows
    subdir('upscale')
  endif
endif

# D3D11 wrapper
if enable_d3d11
  subdir('d3d11')
endif

# Direct3D 7 to 9 on the D3D11 front end
if enable_ddraw
  subdir('legacy')
  subdir('ddraw')
  subdir('d3d9')
  subdir('d3d8')
endif

# Test applications
if is_windows
  subdir('tests')
endif

# D3D10 wrapper
if enable_d3d10
  subdir('d3d10')
endif
