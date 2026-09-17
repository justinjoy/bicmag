project('BicMag', ['c', 'cpp'],
  version : '0.1.0',
  default_options : [
    'c_std=c17',
    'cpp_std=c++17',
    'warning_level=3',
  ])

glib_dep = dependency('glib-2.0', version : '>=2.56')
podofo_dep = dependency('libpodofo', version : '>=1.1.0')

inc = include_directories('.')

pdf_lib = static_library('bicmag-pdf',
  'bicmag/pdf.cpp',
  include_directories : inc,
  dependencies : [glib_dep, podofo_dep],
  install : false,
)

pdf_test = executable('test-bicmag-pdf',
  'tests/test-pdf.c',
  include_directories : inc,
  link_with : pdf_lib,
  dependencies : glib_dep,
  link_language : 'cpp',
)

configure_file(input : 'tests/data/sample.pdf', output : 'sample.pdf', copy : true)

test('bicmag-pdf', pdf_test)
