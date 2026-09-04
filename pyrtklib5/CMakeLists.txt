cmake_minimum_required(VERSION 3.10)
project(pyrtklib5)
set(CMAKE_CXX_STANDARD 17)

if(DEFINED PYTHON_INCLUDE_DIR AND NOT "${PYTHON_INCLUDE_DIR}" STREQUAL "")
    message(STATUS "Using python_include_dir from argument: ${PYTHON_INCLUDE_DIR}")
    include_directories(${PYTHON_INCLUDE_DIR})
else()
    message(STATUS "python_include_dir not provided. Using find_package(PythonLibs) instead.")
    find_package(PythonLibs REQUIRED)
endif()

add_subdirectory(pybind11)
include_directories(${PYBIND11_INCLUDE_DIRS})



# Number of carrier frequency slots per satellite. rtklib.h guards NFREQ with
# #ifndef, so this override needs no header edit. 5 is the useful maximum:
# code2idx() never returns an index above 4 (Galileo E1/E5b/E5a/E6/E5ab and
# BeiDou B1/B2/B2a/B3/B2ab), so slots 5..MAXFREQ-1 are unreachable (MAXFREQ=6).
# Must reach every translation unit - ssat_t/obsd_t are sized by it, and a
# partial rebuild fails as silent memory corruption, not as a link error.
set(NFREQ 5 CACHE STRING "Number of RTKLIB carrier frequency slots (max useful: 5)")
# -DWIN_DLL is inherited from upstream and harmless with GCC (rtkcmn.c tests
# defined(WIN_DLL)||defined(DLL)); kept so the build stays identical to upstream.
ADD_DEFINITIONS(-DENACMP -DENAGAL -DENAGLO -DENAQZS -DENAIRN -DTRACE -DDLL -DEBUG -DWIN_DLL -DNFREQ=${NFREQ})
# pyrtklib (RTKLIB 2.4.3) and pyrtklib5 register the same C++ type names
# (FileWrapper, gtime_t, obsd_t, ...) and pybind11 shares one type registry
# between every module built with the same internals id: importing both in one
# interpreter fails with "generic_type: type FileWrapper is already registered".
# A private internals version gives this module its own registry. Any value
# > 4 selects the same code paths pybind11 uses for Python >= 3.12.
ADD_DEFINITIONS(-DPYBIND11_INTERNALS_VERSION=105)
# Git commit of this repository, exported as pyrtklib5.GIT_SHA (see setup.py).
if(DEFINED PYRTKLIB5_GIT_SHA AND NOT "${PYRTKLIB5_GIT_SHA}" STREQUAL "")
    ADD_DEFINITIONS(-DPYRTKLIB5_GIT_SHA="${PYRTKLIB5_GIT_SHA}")
endif()


if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

if (GCC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -Wall -O3 -ansi -pedantic")
    set(CMAKE_C_FLAGS "-Wno-unused-but-set-variable -Wno-format-overflow -Wno-unused-result -Wpointer-to-int-cast")
endif()


if(DARWIN)
ADD_DEFINITIONS(-DDARWIN)
endif()

if(WIN32)
ADD_DEFINITIONS(-DWIN32)
endif()

set(CMAKE_VERBOSE_MAKEFILE ON)
set(LIBRARY OFF)
add_compile_options(-fno-stack-protector)
if(DEBUG)
    add_compile_options("-g")
else()
    add_compile_options("-O3")
endif()

include_directories("${PROJECT_SOURCE_DIR}/rtksrc/"  "${PROJECT_SOURCE_DIR}")
if(LIBRARY)
    ADD_SUBDIRECTORY(rtksrc)
    link_directories(${PROJECT_SOURCE_DIR}/build/lib)
    AUX_SOURCE_DIRECTORY(${PROJECT_SOURCE_DIR}/rtksrc RTK_src)
    AUX_SOURCE_DIRECTORY(${PROJECT_SOURCE_DIR}/rtksrc/rcv rcv_src)
    # add_library(pyrtklib MODULE pyrtklib.cpp)
    # target_link_libraries(pyrtklib PUBLIC rtk rcv pybind11::module  m pthread)
else()
    AUX_SOURCE_DIRECTORY(${PROJECT_SOURCE_DIR}/rtksrc RTK_src)
    AUX_SOURCE_DIRECTORY(${PROJECT_SOURCE_DIR}/rtksrc/rcv rcv_src)
    # rtkpos.c is compiled through relpos_steps.c (#include "rtkpos.c"), which
    # needs its static helpers; compiling it a second time would define
    # rtkinit/rtkpos/... twice.
    list(REMOVE_ITEM RTK_src ${PROJECT_SOURCE_DIR}/rtksrc/rtkpos.c)
    add_library(pyrtklib5 MODULE ${RTK_src} ${rcv_src} pyrtklib5.cpp)
    # pybind11_add_module(pyrtklib NO_EXTRAS ${RTK_src} ${rcv_src} pyrtklib.cpp)
    if(WIN32)
        set_target_properties(pyrtklib5 PROPERTIES PREFIX "" SUFFIX ".pyd")
        target_link_libraries(pyrtklib5 PUBLIC pybind11::module winmm ws2_32)
    else()
        set_target_properties(pyrtklib5 PROPERTIES PREFIX "")
        target_link_libraries(pyrtklib5 PUBLIC pybind11::module m pthread)
    endif()
endif()
