util_src = files([
  'util_config.cpp',
  'util_types.cpp',
])

util_lib = static_library('dxup_util',
  util_src,
  include_directories : ['.'],
  pic                 : true,
)

util_dep = declare_dependency(
  link_with           : [util_lib],
  include_directories : ['.'],
)
