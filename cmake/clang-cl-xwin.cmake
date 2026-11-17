# SPDX-License-Identifier: MIT

# Builds Hokuto's Windows x64 program on Linux with clang-cl and lld-link, against the Windows SDK and the MSVC runtime
# that xwin downloads. The program then runs under Wine. Pass the folder that `xwin splat` wrote as XWIN_DIR:
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake -DXWIN_DIR=$HOME/.local/share/xwin

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_DIR "$ENV{XWIN_DIR}" CACHE PATH "The folder that xwin splat wrote")
if (NOT IS_DIRECTORY "${XWIN_DIR}/VC/Tools/MSVC")
    message(FATAL_ERROR "XWIN_DIR (${XWIN_DIR}) holds no MSVC runtime. Run xwin splat and pass its folder.")
endif()
# The toolchain file runs again in try_compile's projects, which need the folder too. Some of them, such as the test of
# CheckIPOSupported, take no platform variables, and find the folder in the environment.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)
set(ENV{XWIN_DIR} "${XWIN_DIR}")

file(GLOB msvc_versions LIST_DIRECTORIES true "${XWIN_DIR}/VC/Tools/MSVC/*")
list(GET msvc_versions -1 msvc)
file(GLOB sdk_versions LIST_DIRECTORIES true "${XWIN_DIR}/Windows Kits/10/Include/*")
list(GET sdk_versions -1 sdk_include)
get_filename_component(sdk_version "${sdk_include}" NAME)
set(sdk_lib "${XWIN_DIR}/Windows Kits/10/Lib/${sdk_version}")

# xwin 0.10.0 with Visual Studio 17's manifest, its default, takes the Universal CRT from a package of 2015 and files it
# under the SDK's version. That CRT's fmaf rounds twice where the product and the addend don't nearly cancel, and the
# Wii U's renders then differ from Windows' by a bit in some samples. Its headers lack corecrt_math.h. Visual Studio
# 18's manifest takes the SDK's own CRT.
if (NOT EXISTS "${sdk_include}/ucrt/corecrt_math.h")
    message(FATAL_ERROR "XWIN_DIR (${XWIN_DIR}) holds the Universal CRT of 2015, whose fmaf rounds twice. Run xwin "
                        "with --manifest-version 18, as the README gives.")
endif()

# xwin names the architecture's folders x86_64 by default, and x64 with --preserve-ms-arch-notation.
if (IS_DIRECTORY "${msvc}/lib/x64")
    set(arch x64)
else()
    set(arch x86_64)
endif()

# LLVM_BIN_DIR picks one version of LLVM where several are installed, such as /usr/lib/llvm-22/bin on Ubuntu. Without
# it, the tools come from PATH.
set(LLVM_BIN_DIR "$ENV{LLVM_BIN_DIR}" CACHE PATH "The folder of LLVM's tools, or empty for PATH")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_BIN_DIR)
set(ENV{LLVM_BIN_DIR} "${LLVM_BIN_DIR}")
set(llvm "")
if (LLVM_BIN_DIR)
    set(llvm "${LLVM_BIN_DIR}/")
endif()

set(triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER ${llvm}clang-cl)
set(CMAKE_CXX_COMPILER ${llvm}clang-cl)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_LINKER ${llvm}lld-link)
set(CMAKE_AR ${llvm}llvm-lib)
set(CMAKE_RC_COMPILER ${llvm}llvm-rc)
set(CMAKE_MT ${llvm}llvm-mt)

set(include_dirs "${msvc}/include" "${sdk_include}/ucrt" "${sdk_include}/um" "${sdk_include}/shared"
    "${sdk_include}/winrt" "${sdk_include}/cppwinrt")
set(lib_dirs "${msvc}/lib/${arch}" "${sdk_lib}/ucrt/${arch}" "${sdk_lib}/um/${arch}")

set(compile_flags "")
set(rc_flags "")
foreach (dir IN LISTS include_dirs)
    string(APPEND compile_flags " /imsvc \"${dir}\"")
    string(APPEND rc_flags " -I \"${dir}\"")
endforeach()
set(link_flags "")
foreach (dir IN LISTS lib_dirs)
    string(APPEND link_flags " /libpath:\"${dir}\"")
endforeach()

set(CMAKE_C_FLAGS_INIT "${compile_flags}")
set(CMAKE_CXX_FLAGS_INIT "${compile_flags}")
set(CMAKE_RC_FLAGS_INIT "${rc_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${link_flags}")

# Tests and tools run under Wine.
set(CMAKE_CROSSCOMPILING_EMULATOR wine)

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
