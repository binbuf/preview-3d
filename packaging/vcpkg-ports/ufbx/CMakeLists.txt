cmake_minimum_required(VERSION 3.20)
project(ufbx LANGUAGES C)

add_library(ufbx STATIC ufbx.c)
target_include_directories(ufbx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(ufbx PRIVATE /W3)
endif()

install(TARGETS ufbx ARCHIVE DESTINATION lib)
install(FILES ufbx.h DESTINATION include)
