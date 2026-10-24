find_program(ELF2NRO elf2nro)
find_program(NACPTOOL nacptool)
find_program(BUILD_ROMFS build_romfs)
find_library(GLAPI_LIBRARY glapi REQUIRED)
find_library(EGL_LIBRARY EGL REQUIRED)

set(OS_DEFINES _GNU_SOURCE IOAPI_NO_64 ENABLE_VFS_FD)
list(APPEND VFS_SRC ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-fd.c ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-dirent.c)
list(REMOVE_ITEM GUI_SRC ${PROJECT_SOURCE_DIR}/src/util/gui/menu.c ${PROJECT_SOURCE_DIR}/src/util/gui/file-select.c)
list(APPEND GUI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/gui-font.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-menu.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-ui.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/game-settings.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-plaza.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/plaza.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-console.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-files.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-images.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/artwork.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/artwork-jpeg.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/achievements.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/ra-http.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/ra-badges.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-achievements.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/gui-updates.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/switch-updater.cpp)
set(SWITCH_UPDATE_VERSION "1.0.7" CACHE STRING "Switch frontend release version")
find_library(SWITCH_JPEG_LIBRARY jpeg REQUIRED)
find_library(SWITCH_CURL_LIBRARY curl REQUIRED)
set(SWITCH_ROMHACKPLAZA_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../romhackplaza" CACHE PATH "Reusable Romhack Plaza module source directory")
if(NOT EXISTS "${SWITCH_ROMHACKPLAZA_SOURCE_DIR}/CMakeLists.txt")
	include(FetchContent)
	FetchContent_Declare(switch_romhackplaza
	    URL https://codeload.github.com/Zoboo/romhackplaza/tar.gz/6f07727f81895b410f0aff50e78ce453b38968ba
	    URL_HASH SHA256=384de9791024e9c4690848a670472d272466559dd70bdab5c17a724707f03255)
	FetchContent_GetProperties(switch_romhackplaza)
	if(NOT switch_romhackplaza_POPULATED)
		FetchContent_Populate(switch_romhackplaza)
	endif()
	set(SWITCH_ROMHACKPLAZA_SOURCE_DIR "${switch_romhackplaza_SOURCE_DIR}")
endif()
add_subdirectory("${SWITCH_ROMHACKPLAZA_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/romhackplaza")
target_compile_options(romhackplaza PRIVATE "-ffile-prefix-map=${SWITCH_ROMHACKPLAZA_SOURCE_DIR}=romhackplaza" "-fmacro-prefix-map=${SWITCH_ROMHACKPLAZA_SOURCE_DIR}=romhackplaza")
find_package(Freetype REQUIRED)
find_library(SWITCH_HARFBUZZ_LIBRARY harfbuzz REQUIRED)
find_library(SWITCH_BZIP2_LIBRARY bz2 REQUIRED)
include(FetchContent)
FetchContent_Declare(switch_rcheevos
    URL https://codeload.github.com/RetroAchievements/rcheevos/zip/1433173220a7eaede6a9ed7a18e94117be1821e0
    URL_HASH SHA256=044d51d6a206ebef00536b9cd31c9b90310d99fe0f760a5cbf250aff1c821312)
FetchContent_GetProperties(switch_rcheevos)
if(NOT switch_rcheevos_POPULATED)
	FetchContent_Populate(switch_rcheevos)
endif()
file(GLOB SWITCH_RCHEEVOS_SRC
    "${switch_rcheevos_SOURCE_DIR}/src/rcheevos/*.c"
    "${switch_rcheevos_SOURCE_DIR}/src/rapi/*.c"
    "${switch_rcheevos_SOURCE_DIR}/src/rhash/*.c")
add_library(switch-rcheevos STATIC ${SWITCH_RCHEEVOS_SRC}
    "${switch_rcheevos_SOURCE_DIR}/src/rc_client.c"
    "${switch_rcheevos_SOURCE_DIR}/src/rc_compat.c"
    "${switch_rcheevos_SOURCE_DIR}/src/rc_util.c"
    "${switch_rcheevos_SOURCE_DIR}/src/rc_version.c")
target_include_directories(switch-rcheevos PUBLIC "${switch_rcheevos_SOURCE_DIR}/include"
    "${switch_rcheevos_SOURCE_DIR}/src")
target_compile_definitions(switch-rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH PRIVATE RC_DISABLE_LUA RC_NO_THREADS)
target_compile_options(switch-rcheevos PRIVATE "-ffile-prefix-map=${switch_rcheevos_SOURCE_DIR}=rcheevos")
set_target_properties(switch-rcheevos PROPERTIES C_STANDARD 99)

include_directories(AFTER ${OPENGLES3_INCLUDE_DIR} ${OPENGL_EGL_INCLUDE_DIR})

if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo)
	find_library(NOUVEAU_LIBRARY drm_nouveaud REQUIRED)
	list(APPEND OS_LIB nxd)
else()
	find_library(NOUVEAU_LIBRARY drm_nouveau REQUIRED)
	list(APPEND OS_LIB nx)
endif()
set(VFS_SRC ${VFS_SRC} PARENT_SCOPE)
set(OS_DEFINES ${OS_DEFINES} PARENT_SCOPE)
set(OS_LIB ${OS_LIB} PARENT_SCOPE)

if(BUILD_PERF)
	add_custom_target(${BINARY_NAME}-perf.nro ALL
	                  ${ELF2NRO} ../${BINARY_NAME}-perf ${BINARY_NAME}-perf.nro
	                  DEPENDS ${BINARY_NAME}-perf)
	install(FILES
		    ${CMAKE_CURRENT_BINARY_DIR}/${BINARY_NAME}-perf.nro
		    DESTINATION . COMPONENT ${BINARY_NAME}-perf)
endif()

add_executable(${BINARY_NAME}.elf ${GUI_SRC} ${PLATFORM_SRC} main.c)
set_target_properties(${BINARY_NAME}.elf PROPERTIES COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}")
target_include_directories(${BINARY_NAME}.elf PRIVATE ${FREETYPE_INCLUDE_DIRS})
target_compile_definitions(${BINARY_NAME}.elf PRIVATE
    SWITCH_UPDATE_REPOSITORY="Zoboo/mgba-switch" SWITCH_UPDATE_VERSION="${SWITCH_UPDATE_VERSION}")
target_compile_options(${BINARY_NAME}.elf PRIVATE
    "-ffile-prefix-map=${PROJECT_SOURCE_DIR}=." "-fmacro-prefix-map=${PROJECT_SOURCE_DIR}=.")
target_link_libraries(${BINARY_NAME}.elf ${BINARY_NAME} switch-rcheevos romhackplaza ${M_LIBRARY} ${EGL_LIBRARY} ${OPENGLES3_LIBRARY} ${GLAPI_LIBRARY} ${NOUVEAU_LIBRARY} ${SWITCH_CURL_LIBRARY}
                      -Wl,--start-group ${FREETYPE_LIBRARIES} ${SWITCH_HARFBUZZ_LIBRARY} ${SWITCH_BZIP2_LIBRARY} ${SWITCH_JPEG_LIBRARY} ${PNG_LIBRARIES} ${ZLIB_LIBRARIES} -Wl,--end-group
                      stdc++ ${OS_LIB})

add_custom_command(OUTPUT control.nacp
                   COMMAND ${NACPTOOL} --create "${PROJECT_NAME}" "endrift" "${SWITCH_UPDATE_VERSION}" control.nacp)

add_custom_command(OUTPUT romfs.bin
                   COMMAND ${CMAKE_COMMAND} -E make_directory romfs
                   COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/res/font-new.png" romfs/
                   COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/res/scripts/demos/SourceSans3-Regular.otf" romfs/
                   COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/fileassoc.cfg.in" romfs/
                   COMMAND ${CMAKE_COMMAND} -E copy "${switch_rcheevos_SOURCE_DIR}/LICENSE" romfs/rcheevos-LICENSE
                   COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/SourceSans3-LICENSE.txt" romfs/SourceSans3-LICENSE.txt
                   COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo-LICENSE.txt" romfs/libjpeg-turbo-LICENSE.txt
                   COMMAND ${BUILD_ROMFS} romfs romfs.bin
                   COMMAND ${CMAKE_COMMAND} -E remove_directory romfs
               DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo-LICENSE.txt" "${CMAKE_CURRENT_SOURCE_DIR}/SourceSans3-LICENSE.txt" "${switch_rcheevos_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/res/font-new.png" "${PROJECT_SOURCE_DIR}/res/scripts/demos/SourceSans3-Regular.otf" "${CMAKE_CURRENT_SOURCE_DIR}/fileassoc.cfg.in")

add_custom_target(${BINARY_NAME}.nro ALL
                  ${ELF2NRO} ${BINARY_NAME}.elf "${PROJECT_SOURCE_DIR}/${BINARY_NAME}.nro" --romfs=romfs.bin --nacp=control.nacp "--icon=${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
                  DEPENDS ${BINARY_NAME}.elf control.nacp ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg romfs.bin
                  VERBATIM)

install(TARGETS ${BINARY_NAME}.elf DESTINATION . COMPONENT ${BINARY_NAME}-dbg)
install(FILES ${PROJECT_SOURCE_DIR}/${BINARY_NAME}.nro DESTINATION . COMPONENT ${BINARY_NAME}-switch)
