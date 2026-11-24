cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK")
  endif()
endif()
project(prefetch_enabler C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vendored by default (include/ + prebuilt lib/*.a) - see third_party/psp2spl/NOTICE.md.
# Point this at your own splib build (same include/ + lib/ layout) to override it.
set(PSP2SPL_DIR "${CMAKE_SOURCE_DIR}/third_party/psp2spl" CACHE PATH "psp2spl splib: headers + libspl.a/libsplCFK.a")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -nostdlib -fno-tree-vectorize")

include_directories(${PSP2SPL_DIR}/include)
link_directories(${PSP2SPL_DIR}/lib)

add_executable(prefetch_enabler src/main.c src/spl_layout.c src/config.c)
target_link_libraries(prefetch_enabler
  splCFK
  spl
  SceSblSmSchedProxyForKernel_stub
  SceSblSmCommForKernel_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceSysrootForKernel_stub
  SceKernelSuspendForDriver_stub
  SceThreadmgrForDriver_stub
  SceDebugForDriver_stub
  SceDebugForKernel_stub
)
vita_create_self(prefetch_enabler.skprx prefetch_enabler CONFIG ${CMAKE_SOURCE_DIR}/prefetch_enabler.yml UNSAFE)
