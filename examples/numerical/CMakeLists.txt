project(numerical)

link_directories(
    ${libyaml_install_dir}/${DGK_LINKAGE_DIRNAME}/${DGK_CONFIG_DIRNAME}
)

add_executable(numerical main.c)

target_link_libraries(numerical
    libcyaml
    yaml_static
)

set_target_properties(numerical PROPERTIES FOLDER examples/)
