cmake_minimum_required (VERSION 3.10)

set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/_deploy)
set(libyaml_install_dir $ENV{GIT_DIRECTORY}/libyaml/${DGK_INSTALL_PREFIX})

# Copying versions from libyaml
add_compile_definitions(VERSION_MAJOR=0)
add_compile_definitions(VERSION_MINOR=1)
add_compile_definitions(VERSION_PATCH=7)
add_compile_definitions(VERSION_STRING="${YAML_VERSION_MAJOR}.${YAML_VERSION_MINOR}.${YAML_VERSION_PATCH}")

include_directories(${CMAKE_SOURCE_DIR}/include)

project(cyaml)

if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG     "/MTd /Zi /Ob0 /Od /RTC1 /D_DEBUG ")
    set(CMAKE_C_FLAGS_RELEASE   "/MT /O2 /Ob2 /D NDEBUG")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(src)
add_subdirectory(test)
add_subdirectory(examples/numerical)
add_subdirectory(examples/planner)

install(
  TARGETS libcyaml DESTINATION ${DGK_LINKAGE_DIRNAME}/${DGK_CONFIG_DIRNAME}
)

install(
  FILES include/cyaml/cyaml.h DESTINATION include/cyaml
)
