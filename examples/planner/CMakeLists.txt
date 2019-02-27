project(planner)

link_directories(
    ${libyaml_install_dir}/${DGK_LINKAGE_DIRNAME}/${DGK_CONFIG_DIRNAME}
)

add_executable(planner main.c)

target_link_libraries(planner
    libcyaml
    yaml_static
)

set_target_properties(planner PROPERTIES FOLDER examples/)
