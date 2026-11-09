# Deliberately use only the public C ABI; no GTest or private PROJ headers.
add_executable(proj_extension_test test_extensions.cpp abi_header.c)
target_link_libraries(proj_extension_test PRIVATE proj)
target_include_directories(proj_extension_test PRIVATE ${PROJ_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(proj_extension_test PRIVATE /EHsc)
endif()
add_test(NAME extensions COMMAND proj_extension_test ${PROJ_SOURCE_DIR}/data)
