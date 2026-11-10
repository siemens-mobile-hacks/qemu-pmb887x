project('tomlc17', 'c', license: 'MIT',
        default_options: ['warning_level=1', 'c_std=gnu11'])

tomlc17_inc = include_directories('src')
libtomlc17 = static_library('tomlc17', 'src/tomlc17.c',
                            include_directories: tomlc17_inc)
tomlc17_dep = declare_dependency(link_with: libtomlc17,
                                 include_directories: tomlc17_inc)
