project(testcyaml)

link_directories(
    ${libyaml_install_dir}/${DGK_LINKAGE_DIRNAME}/${DGK_CONFIG_DIRNAME}
)

add_executable(testcyaml 
    units/errs.c
    units/file.c
    units/free.c
    units/load.c
    units/save.c
    units/test.c
    units/ttest.h
    units/utf8.c
    units/util.c
)

target_link_libraries(testcyaml
    libcyaml
    yaml_static
)