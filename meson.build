project('BicMag', ['c', 'cpp'],
  version : '0.1.0',
  default_options : [
    'c_std=c17',
    'cpp_std=c++17',
    'warning_level=3',
  ])

glib_dep = dependency('glib-2.0', version : '>=2.66')
gio_dep = dependency('gio-2.0', version : '>=2.66')
libsoup_dep = dependency('libsoup-3.0', version : '>=3.0')
podofo_dep = dependency('libpodofo', version : '>=1.1.0')
sqlite_dep = dependency('sqlite3')

inc = include_directories('.')

pdf_lib = static_library('bicmag-pdf',
  'bicmag/pdf.cpp',
  include_directories : inc,
  dependencies : [glib_dep, podofo_dep],
  install : false,
)

notice_lib = static_library('bicmag-notice',
  'bicmag/notice.c',
  include_directories : inc,
  dependencies : glib_dep,
  install : false,
)

cache_lib = static_library('bicmag-cache',
  'bicmag/cache.c',
  include_directories : inc,
  dependencies : [glib_dep, gio_dep, sqlite_dep],
  link_with : notice_lib,
  install : false,
)

ntis_lib = static_library('bicmag-ntis', ['bicmag/ntis.c', 'bicmag/list.c'],
  include_directories : inc, dependencies : [glib_dep, gio_dep, libsoup_dep])

pdf_test = executable('test-bicmag-pdf',
  'tests/test-pdf.c',
  include_directories : inc,
  link_with : [pdf_lib, notice_lib, cache_lib, ntis_lib],
  dependencies : [glib_dep, gio_dep, libsoup_dep, sqlite_dep],
  link_language : 'cpp',
)

configure_file(input : 'tests/data/sample.pdf', output : 'sample.pdf', copy : true)

test('bicmag-pdf', pdf_test)
