# bbport-windows: dependencies on Windows (MSYS2 CLANG64).
#
# MSYS2 packages everything bbgpu needs except magic_enum, miniz and xbyak; those are fetched
# here at pinned versions. OVERRIDE_FIND_PACKAGE makes the find_package() calls in
# CMakeLists.txt resolve to the fetched copies, so the Linux path stays unchanged.
include(FetchContent)

FetchContent_Declare(magic_enum
    URL https://github.com/Neargye/magic_enum/archive/refs/tags/v0.9.7.tar.gz
    URL_HASH SHA256=b403d3dad4ef542fdc3024fa37d3a6cedb4ad33c72e31b6d9bab89dcaf69edf7
    OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
    URL_HASH SHA256=98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
    OVERRIDE_FIND_PACKAGE)
# Header-only; used by the shader recompiler's SRT pass. Not added as a subproject.
FetchContent_Declare(xbyak
    URL https://github.com/herumi/xbyak/archive/refs/tags/v7.43.tar.gz
    URL_HASH SHA256=8fb0ed1ba0cc299b02aea3da84485083c9779a5c38b652640b2e331b7ca964f1
    SOURCE_SUBDIR none)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(magic_enum miniz xbyak)
if (NOT TARGET miniz::miniz)
    add_library(miniz::miniz ALIAS miniz)
endif()
include_directories(SYSTEM ${xbyak_SOURCE_DIR})

# MSYS2's vulkan-memory-allocator is the bare header (include/vma/vk_mem_alloc.h) with no
# CMake package; provide the target CMakeLists.txt links against.
find_path(BB_VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
file(WRITE ${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/vulkanmemoryallocator-config.cmake
"if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
    set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES \"${BB_VMA_INCLUDE_DIR}\")
endif()
")

# POSIX/Linux APIs the code uses (windows/compat): Linux-named headers first on the include
# path, implemented on Win32 in a static library linked with bbgpu. setjmp/longjmp do not
# unwind, as on Linux (unwinding would walk through game code, which has no unwind data).
set(BB_COMPAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../windows/compat)
include_directories(BEFORE SYSTEM ${BB_COMPAT_DIR}/include)
add_compile_definitions(__USE_MINGW_SETJMP_NON_SEH)
add_library(bbcompat STATIC ${BB_COMPAT_DIR}/posix_compat.c)
target_link_libraries(bbcompat PUBLIC psapi)
cmake_language(DEFER CALL target_link_libraries bbgpu PUBLIC bbcompat)

# No X11 on Windows: SDL3 creates the window and the Win32 Vulkan surface.
add_library(PkgConfig::X11 INTERFACE IMPORTED)

# bbgpu is linked into bb-probe.exe: a DLL cannot leave the runtime_* symbols it takes from
# the loader undefined (the Linux build uses --allow-shlib-undefined).
set(BB_GPU_LIBRARY_TYPE STATIC)
