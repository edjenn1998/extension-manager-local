local_zip_driver = executable('local-zip-driver',
  'local-zip-driver.c', '../src/local/exm-local-zip.c',
  include_directories: include_directories('../src'),
  dependencies: [dependency('gio-unix-2.0'), dependency('json-glib-1.0'), dependency('libarchive')],
)
test('Local ZIP validation and GNOME installer integration', find_program('python3'),
  args: [files('test-local-zip.py'), local_zip_driver], timeout: 180,
)
executable('image-driver', 'image-driver.c', dependencies: dependency('glycin-2'))
