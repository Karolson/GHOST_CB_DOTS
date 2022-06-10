project('dodsghost', 'cpp', default_options: ['strip=true','cpp_std=c++17','b_vscrt=mtd'])

compiler = meson.get_compiler('cpp')

incdir = include_directories('include')

library_path = meson.source_root() + '/lib/' + host_machine.cpu_family()
if host_machine.system() == 'windows'
    library_path += '/windows'
    if compiler.get_id() == 'msvc'
        library_path += '/msvc'
    else
        add_global_link_arguments('-static', '-static-libgcc', '-static-libstdc++', language: 'cpp')
        library_path += '/mingw'
    endif
else
    library_path += '/linux'
endif

lib_bncsutil = [
    compiler.find_library('bncsutil', dirs : library_path),
    compiler.find_library('gmp', dirs : library_path)
]
lib_storm = [compiler.find_library('storm', dirs : library_path)]
if host_machine.system() != 'windows'
    lib_storm += dependency('bzip2')
endif
lib_sqlite3 = [
    compiler.find_library('sqlite3', dirs : library_path),
    compiler.find_library('ssl', dirs : library_path),
    compiler.find_library('crypto', dirs : library_path),
]
if host_machine.system() == 'windows' and compiler.get_id() != 'msvc'
    lib_sqlite3 += [
        compiler.find_library('ssl'),
        compiler.find_library('crypto'),
    ]
endif
if host_machine.system() != 'windows'
    lib_sqlite3 += compiler.find_library('dl', dirs : library_path)
endif
if host_machine.system() == 'windows'
    lib_mysql = [compiler.find_library('mariadbclient', dirs : library_path)]
else
    lib_mysql = [compiler.find_library('mysqlclient', dirs : library_path)]
endif
lib_mysql += [
    compiler.find_library('curl', dirs : library_path),
    compiler.find_library('z', dirs : library_path),
]
lib_curses = compiler.find_library('pdcurses', dirs : library_path)

subdir('src')