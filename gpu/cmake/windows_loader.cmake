# bbport-windows: the loader and HLE runtime (src/), linked with bbgpu into bb-probe.exe.
# On Linux build.sh compiles them with the system compiler; here CMake links the GPU library's
# dependencies for us. The runtime's POSIX code compiles against windows/compat
# (BB_WINDOWS_PORT selects it over the old Windows stubs).
set(BB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
file(GLOB BB_RUNTIME_SOURCES CONFIGURE_DEPENDS ${BB_SRC}/runtime*.c)
file(GLOB BB_ATRAC9_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/LibAtrac9/C/src/*.c)

# Third-party decoder: compiled without this project's warning policy, as in build.sh.
add_library(atrac9 STATIC ${BB_ATRAC9_SOURCES})
set_target_properties(atrac9 PROPERTIES C_STANDARD 99)
target_compile_options(atrac9 PRIVATE -w)

add_executable(bb-probe ${BB_SRC}/probe.c ${BB_RUNTIME_SOURCES} ${BB_SRC}/vulkan_smoke.c)
set_target_properties(bb-probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../out)
target_include_directories(bb-probe PRIVATE ${BB_SRC}/.. ${BB_SRC})
target_compile_definitions(bb-probe PRIVATE BB_WINDOWS_PORT BB_COMPAT_LINUX_STAT _POSIX_THREAD_SAFE_FUNCTIONS)
target_compile_options(bb-probe PRIVATE -Wall -Wextra)
target_link_libraries(bb-probe PRIVATE bbgpu atrac9)
# Pointers the game sees must stay below 1 TiB (it packs them into 40 bits): no high-entropy
# ASLR, which places heaps and stacks anywhere in the 128 TiB address space. The guest main
# thread runs on the process's main stack (8 MiB on Linux).
target_link_options(bb-probe PRIVATE -Wl,--disable-high-entropy-va -Wl,--stack,16777216)

# Upstream's runtime tests (build.sh --test): ninja -C out/gpu runtime-test sema-test content-test
function(bb_runtime_test name)
    add_executable(${name} EXCLUDE_FROM_ALL ${ARGN})
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../out)
    target_include_directories(${name} PRIVATE ${BB_SRC}/.. ${BB_SRC})
    target_compile_definitions(${name} PRIVATE BB_WINDOWS_PORT BB_COMPAT_LINUX_STAT _POSIX_THREAD_SAFE_FUNCTIONS)
    target_compile_options(${name} PRIVATE -Wall -Wextra -UNDEBUG)
    target_link_libraries(${name} PRIVATE bbgpu atrac9)
    target_link_options(${name} PRIVATE -Wl,--disable-high-entropy-va)
endfunction()
bb_runtime_test(runtime-test ${BB_SRC}/../tests/test_runtime.c ${BB_RUNTIME_SOURCES})
bb_runtime_test(sema-test ${BB_SRC}/../tests/test_sema.c ${BB_RUNTIME_SOURCES})
bb_runtime_test(content-test ${BB_SRC}/../tests/test_content.c ${BB_SRC}/runtime_content.c)
