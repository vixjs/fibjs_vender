include(../tools/option.cmake)

get_filename_component(name ${CMAKE_CURRENT_SOURCE_DIR} NAME)
project(${name})

if(NOT DEFINED src_list)
file(GLOB_RECURSE src_list "src/*.c*")
endif()
add_library(${name} ${src_list})

if(NOT DEFINED BUILD_TYPE)
set(BUILD_TYPE "release")
endif()
set(VENDER_ROOT ${PROJECT_SOURCE_DIR}/../)


if(EXISTS "${VENDER_ROOT}/../build")
	set(LIBRARY_OUTPUT_PATH ${VENDER_ROOT}/../bin/${CMAKE_HOST_SYSTEM_NAME}_${ARCH}_${BUILD_TYPE})
else()
	set(LIBRARY_OUTPUT_PATH ${VENDER_ROOT}/.dist/bin/${CMAKE_HOST_SYSTEM_NAME}_${ARCH}_${BUILD_TYPE})
endif()

if(NOT flags)
	set(flags " ")
endif()
set(flags "${flags} -fsigned-char -fmessage-length=0 -fdata-sections -ffunction-sections -D_FILE_OFFSET_BITS=64")

if(NOT cflags)
	set(cflags " ")
endif()

if(NOT ccflags)
	set(ccflags " ")
endif()

set(ccflags "${ccflags} ${CXX_STD_VER_FLAG}")
if(FIBJS_CMAKE_BUILD_VERBOSE)
	message("CXX_STD_VER_FLAG is ${CXX_STD_VER_FLAG}")
endif()

set(link_flags " ")

if(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Darwin")
	set(flags "${flags} -mmacosx-version-min=10.9")
endif()

if(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Windows")
	add_definitions(-DWIN32 -D_LIB -D_CRT_SECURE_NO_WARNINGS -D_CRT_RAND_S -DNOMINMAX)
	set(flags "${flags} -fms-extensions -fmsc-version=1910 -frtti")
	
	if(${ARCH} STREQUAL "amd64")
		set(flags "${flags} -m64")
	elseif(${ARCH} STREQUAL "i386")
		set(flags "${flags} -m32")
	endif()
endif()

if(${BUILD_TYPE} STREQUAL "release")
	set(flags "${flags} -O3 -s ${BUILD_OPTION} -w -fvisibility=hidden")

	if(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "FreeBSD")
		set(flags "${flags} -fno-omit-frame-pointer")
	else()
		set(flags "${flags} -fomit-frame-pointer")
	endif()

	set(link_flags "${link_flags} ${BUILD_OPTION}")
	add_definitions(-DNDEBUG=1)
endif()

if(${BUILD_TYPE} STREQUAL "debug")
	set(flags "${flags} -g -O0 ${BUILD_OPTION} -Wall")
	set(link_flags "${link_flags} ${BUILD_OPTION}")
	add_definitions(-DDEBUG=1)
	if(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Windows")
		add_definitions(-D_DEBUG)
	endif()
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${flags} ${cflags}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flags} ${ccflags}")

if(link_flags)
	set_target_properties(${name} PROPERTIES LINK_FLAGS ${link_flags})
endif()

include_directories(${PROJECT_SOURCE_DIR} "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/../")