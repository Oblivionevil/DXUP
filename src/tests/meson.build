test_d3d11_dep = cpp.find_library('d3d11', required : true)
test_dxgi_dep = cpp.find_library('dxgi', required : true)
test_user32_dep = cpp.find_library('user32', required : true)
test_d3d10_dep = cpp.find_library('d3d10', required : true)

executable('dx11_test_app',
  ['dx11_test_app.cpp'],
  dependencies : [test_d3d11_dep, test_dxgi_dep, test_user32_dep, test_d3d10_dep],
  install : false,
)
executable('shader_bench',
  ['shader_bench.cpp'],
  dependencies : [test_d3d11_dep, d3dcompiler_dep],
  install : false,
)
if enable_ddraw
  executable('dx7_test_app',
    ['dx7_test_app.cpp'],
    dependencies : [cpp.find_library('ddraw'), cpp.find_library('dxguid'), test_user32_dep],
    install : false,
  )
  executable('d3d8_test_app',
    ['d3d8_test_app.cpp'],
    dependencies : [test_user32_dep],
    install : false,
  )
  executable('d3d9_test_app',
    ['d3d9_test_app.cpp'],
    dependencies : [d3dcompiler_dep, test_user32_dep],
    install : false,
  )
  executable('d3d9_shader_test',
    ['d3d9_shader_test.cpp'],
    dependencies : [legacy_dep, d3dcompiler_dep, cpp.find_library('d3d9'), test_d3d11_dep, test_user32_dep],
    include_directories : include_directories('../legacy'),
    install : false,
  )
endif
