cmake_minimum_required(VERSION 3.16)
project(skillctl C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W3)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# The release workflow stamps the version in: cmake -DSKILLCTL_VERSION=0.2.0
if(DEFINED SKILLCTL_VERSION)
  add_compile_definitions(SKILLCTL_VERSION="${SKILLCTL_VERSION}")
endif()

# ---------------------------------------------------------------- backends
#
# ONNX Runtime and llama.cpp are optional. When their headers are not
# around, the matching code compiles to stubs and the vector path is
# simply absent - the lexical router is unaffected. This mirrors what
# the Makefile does, so both build systems produce the same engine.

find_path(ORT_INCLUDE_DIR onnxruntime_c_api.h
  HINTS ${ORT_PREFIX} ENV ORT_PREFIX
  PATH_SUFFIXES include/onnxruntime include)
find_library(ORT_LIBRARY NAMES onnxruntime
  HINTS ${ORT_PREFIX} ENV ORT_PREFIX
  PATH_SUFFIXES lib lib64)

find_path(LLAMA_INCLUDE_DIR llama.h
  HINTS ${LLAMA_PREFIX} ENV LLAMA_PREFIX
  PATH_SUFFIXES include include/llama)
find_path(GGML_INCLUDE_DIR ggml.h
  HINTS ${GGML_PREFIX} ENV GGML_PREFIX
  PATH_SUFFIXES include include/ggml)
find_library(LLAMA_LIBRARY NAMES llama
  HINTS ${LLAMA_PREFIX} ENV LLAMA_PREFIX
  PATH_SUFFIXES lib lib64)
find_library(GGML_LIBRARY NAMES ggml
  HINTS ${GGML_PREFIX} ENV GGML_PREFIX
  PATH_SUFFIXES lib lib64)
find_library(GGML_BASE_LIBRARY NAMES ggml-base
  HINTS ${GGML_PREFIX} ENV GGML_PREFIX
  PATH_SUFFIXES lib lib64)

if(ORT_INCLUDE_DIR AND ORT_LIBRARY)
  message(STATUS "ONNX Runtime backend: on (${ORT_LIBRARY})")
else()
  message(STATUS "ONNX Runtime backend: off (compiles to stubs)")
  add_compile_definitions(SKILLCTL_NO_ONNX)
endif()

if(LLAMA_INCLUDE_DIR AND LLAMA_LIBRARY AND GGML_LIBRARY)
  message(STATUS "llama.cpp backend: on (${LLAMA_LIBRARY})")
else()
  message(STATUS "llama.cpp backend: off (compiles to stubs)")
  add_compile_definitions(SKILLCTL_NO_LLAMA)
endif()

file(GLOB CORE_SRC core/*.c)
if(WIN32)
  list(FILTER CORE_SRC EXCLUDE REGEX "sh_fs_posix")
else()
  list(FILTER CORE_SRC EXCLUDE REGEX "sh_fs_win32")
endif()
file(GLOB CLI_SRC cli/*.c)

add_library(skillctl_core STATIC ${CORE_SRC})
target_include_directories(skillctl_core PUBLIC core)

if(ORT_INCLUDE_DIR AND ORT_LIBRARY)
  target_include_directories(skillctl_core PUBLIC ${ORT_INCLUDE_DIR})
  target_link_libraries(skillctl_core PUBLIC ${ORT_LIBRARY})
endif()

if(LLAMA_INCLUDE_DIR AND LLAMA_LIBRARY AND GGML_LIBRARY)
  target_include_directories(skillctl_core PUBLIC ${LLAMA_INCLUDE_DIR})
  if(GGML_INCLUDE_DIR)
    target_include_directories(skillctl_core PUBLIC ${GGML_INCLUDE_DIR})
  endif()
  target_link_libraries(skillctl_core PUBLIC ${LLAMA_LIBRARY} ${GGML_LIBRARY})
  if(GGML_BASE_LIBRARY)
    target_link_libraries(skillctl_core PUBLIC ${GGML_BASE_LIBRARY})
  endif()
  # ggml repeats typedefs (C11); -Wall flags that under -std=c99.
  if(NOT MSVC)
    set_source_files_properties(core/sh_llama.c PROPERTIES
      COMPILE_OPTIONS "-Wno-typedef-redefinition")
  endif()
endif()

add_executable(skillctl ${CLI_SRC})
target_link_libraries(skillctl PRIVATE skillctl_core)

install(TARGETS skillctl RUNTIME DESTINATION bin)

enable_testing()
file(GLOB TEST_SRC tests/test_core.c)
if(TEST_SRC)
  add_executable(test_core ${TEST_SRC})
  target_link_libraries(test_core PRIVATE skillctl_core)
  add_test(NAME core COMMAND test_core)
endif()
