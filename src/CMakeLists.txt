project(libcyaml)

add_library(libcyaml 
    data.h
    free.c
    load.c
    mem.c
    mem.h
    save.c
    utf8.c
    utf8.h
    util.c
    util.h
)

target_include_directories(libcyaml PUBLIC
    ${libyaml_install_dir}/include
)
