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

# todo since YAML_DECLARE_STATIC is PUBLIC in the libyaml CMakeLists.txt,
# why do I need to define it here too? Shouldn't its definition be inherited?
target_compile_definitions(libcyaml PUBLIC $<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:YAML_DECLARE_STATIC>)

target_include_directories(libcyaml PUBLIC
    ${libyaml_install_dir}/include
)
