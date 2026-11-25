cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()
project(DeltaVita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "DeltaVita")
set(VITA_TITLEID "DLTV00001")
set(VITA_VERSION "00.10")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-unused-function")

# lógica común del lanzador y de los ejecutores de núcleo
add_library(frontend STATIC src/common.c src/ui.c src/skin.c src/audio.c src/third_party/cJSON.c)

set(SCE_LIBS
  vita2d png jpeg zip zstd bz2 lzma z m ScePvf_stub
  SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub
  SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub SceIme_stub SceRtc_stub SceLibKernel_stub pthread)

add_executable(launcher src/launcher.c)
target_link_libraries(launcher frontend ssl crypto ${SCE_LIBS} crypto SceNet_stub SceNetCtl_stub)
vita_create_self(eboot.bin launcher UNSAFE)

# libretro-common: los núcleos compilados con STATIC_LINKING esperan que el frontend lo aporte
set(LRC ${CMAKE_SOURCE_DIR}/cores/src/libretro-common)
add_library(retrocommon STATIC
  ${LRC}/compat/compat_strl.c ${LRC}/compat/compat_strcasestr.c ${LRC}/compat/compat_posix_string.c
  ${LRC}/compat/fopen_utf8.c ${LRC}/encodings/encoding_utf.c ${LRC}/file/file_path.c ${LRC}/file/file_path_io.c
  ${LRC}/file/retro_dirent.c ${LRC}/streams/file_stream.c ${LRC}/streams/file_stream_transforms.c
  ${LRC}/streams/memory_stream.c ${LRC}/string/stdstring.c ${LRC}/string/rstrtod.c ${LRC}/time/rtime.c ${LRC}/vfs/vfs_implementation.c
  ${LRC}/lists/string_list.c ${LRC}/lists/dir_list.c ${LRC}/memmap/memalign.c ${LRC}/features/features_cpu.c
  ${LRC}/rthreads/rthreads.c)
target_include_directories(retrocommon PRIVATE ${LRC}/include)
target_compile_definitions(retrocommon PRIVATE VITA)

# un ejecutable por núcleo compilado en cores/lib (tools/build_cores.sh)
set(VPK_FILES "")
file(GLOB CORE_LIBS "${CMAKE_SOURCE_DIR}/cores/lib/*.a")
list(FILTER CORE_LIBS EXCLUDE REGEX "/\\._") # basura AppleDouble de macOS en discos exFAT
foreach(lib ${CORE_LIBS})
  get_filename_component(core ${lib} NAME_WE)
  add_executable(core_${core} src/runner.c)
  target_link_libraries(core_${core} frontend ${lib} retrocommon stdc++ ${SCE_LIBS} crypto)
  set_target_properties(core_${core} PROPERTIES LINKER_LANGUAGE CXX)
  vita_create_self(${core}.bin core_${core} UNSAFE)
  list(APPEND VPK_FILES FILE ${CMAKE_BINARY_DIR}/${core}.bin ${core}_libretro.self)
endforeach()

# skins Classic originales (tools/make_skins.py de DeltaDroid) o, en builds personales, las de Delta
file(GLOB SKINS "${CMAKE_SOURCE_DIR}/assets/skins/*.deltaskin")
list(FILTER SKINS EXCLUDE REGEX "/\\._")
foreach(skin ${SKINS})
  get_filename_component(name ${skin} NAME)
  list(APPEND VPK_FILES FILE ${skin} skins/${name})
endforeach()

vita_create_vpk(DeltaVita.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${VPK_FILES}
)
foreach(lib ${CORE_LIBS})
  get_filename_component(core ${lib} NAME_WE)
  add_dependencies(DeltaVita.vpk-vpk ${core}.bin-self)
endforeach()
