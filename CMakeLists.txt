cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set")
  endif()
endif()

project(red_viter C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Red Viter")
set(VITA_TITLEID "RDVP00001")
set(VITA_VERSION "01.00")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fno-strict-aliasing -fcommon -Wno-incompatible-pointer-types")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fno-rtti -fno-exceptions -fno-strict-aliasing")

file(GLOB COMMON_C source/common/*.c)
file(GLOB COMMON_CPP source/common/*.cpp)
file(GLOB VITA_C source/vita/*.c)

add_executable(${PROJECT_NAME} ${COMMON_C} ${COMMON_CPP} ${VITA_C})

target_include_directories(${PROJECT_NAME} PRIVATE include source/vita source/common)
target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUGLEVEL=0)

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  mathneon
  z
  m
  SceShaccCg_stub
  SceShaccCgExt
  taihen_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceAudio_stub
  ScePower_stub
  SceSysmodule_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  SceShellSvc_stub
  SceKernelDmacMgr_stub
  SceLibKernel_stub
  SceSysmem_stub
  SceIofilemgr_stub
)

if(NOT VITA_MAKE_FSELF_FLAGS MATCHES "(^| )-a( |$)")
  set(VITA_MAKE_FSELF_FLAGS "${VITA_MAKE_FSELF_FLAGS} -a 0x2800000000000001" CACHE STRING "vita-make-fself flags" FORCE)
endif()
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
