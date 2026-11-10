cmake_minimum_required(VERSION 3.24)
project(CoD1Native C OBJC)
set(CMAKE_C_STANDARD 11)
set(CMAKE_OSX_ARCHITECTURES arm64)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
find_package(SDL3 REQUIRED CONFIG PATHS /opt/homebrew/opt/sdl3)

# Lifted guest code (CoDSP.exe + gamex86/cgamex86/uix86.dll), one static library per module.
foreach(mod exe game cgame ui)
  file(GLOB GEN_${mod} CONFIGURE_DEPENDS generated/${mod}/gen_*.c)
  add_library(gen_${mod} STATIC ${GEN_${mod}})
  target_include_directories(gen_${mod} PUBLIC runtime generated)
  # generated code: warnings off, x86 wrap-around semantics, no strict aliasing
  target_compile_options(gen_${mod} PRIVATE -w -fwrapv -fno-strict-aliasing -O2)
  list(APPEND GEN_LIBS gen_${mod})
endforeach()

file(GLOB RT_SRC CONFIGURE_DEPENDS runtime/*.c runtime/*.m runtime/win32/*.c runtime/gl/*.c runtime/gl/*.m runtime/audio/*.c runtime/overrides/*.c)
add_executable(cod1sp ${RT_SRC} generated/import_stubs.c generated/dispatch_table.c generated/import_table.c)
target_include_directories(cod1sp PRIVATE runtime generated)
target_compile_options(cod1sp PRIVATE -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing)
set_source_files_properties(runtime/gl/gl_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc -Wno-unused-variable")
set_source_files_properties(runtime/mac.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
target_link_libraries(cod1sp PRIVATE ${GEN_LIBS} SDL3::SDL3
  "-framework CoreFoundation" "-framework Foundation" "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework AudioToolbox")

# Multiplayer: CoDMP.exe + game_mp/cgame_mp/ui_mp_x86.dll (COD_TARGET=mp lift.py -> generated_mp/).
# Same runtime and the same patches (MP addresses via COD_MP), except screenfield.c: SP intro videos only.
if(EXISTS ${CMAKE_SOURCE_DIR}/generated_mp/dispatch_table.c)
  foreach(mod exe game cgame ui)
    file(GLOB GEN_MP_${mod} CONFIGURE_DEPENDS generated_mp/${mod}/gen_*.c)
    add_library(gen_mp_${mod} STATIC ${GEN_MP_${mod}})
    target_include_directories(gen_mp_${mod} PUBLIC runtime generated_mp)
    target_compile_options(gen_mp_${mod} PRIVATE -w -fwrapv -fno-strict-aliasing -O2)
    list(APPEND GEN_MP_LIBS gen_mp_${mod})
  endforeach()
  file(GLOB RT_MP_SRC CONFIGURE_DEPENDS runtime/*.c runtime/*.m runtime/win32/*.c runtime/gl/*.c runtime/gl/*.m runtime/audio/*.c)
  add_executable(cod1mp ${RT_MP_SRC} runtime/overrides/setjmp.c runtime/overrides/vidmodes.c
    runtime/overrides/focus.c runtime/overrides/widescreen.c
    generated_mp/import_stubs.c generated_mp/dispatch_table.c generated_mp/import_table.c)
  target_include_directories(cod1mp PRIVATE runtime generated_mp)
  target_compile_definitions(cod1mp PRIVATE COD_MP=1)
  target_compile_options(cod1mp PRIVATE -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing)
  target_link_libraries(cod1mp PRIVATE ${GEN_MP_LIBS} SDL3::SDL3
    "-framework CoreFoundation" "-framework Foundation" "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework AudioToolbox")
endif()

if(TARGET cod1mp)
add_custom_target(package_app
  COMMAND ${CMAKE_SOURCE_DIR}/packaging/package_app.sh ${CMAKE_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS cod1sp cod1mp
  USES_TERMINAL)

endif()
