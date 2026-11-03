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

file(GLOB CORE_SRC core/*.c)
if(WIN32)
  list(FILTER CORE_SRC EXCLUDE REGEX "sh_fs_posix")
else()
  list(FILTER CORE_SRC EXCLUDE REGEX "sh_fs_win32")
endif()
file(GLOB CLI_SRC cli/*.c)

add_library(skillctl_core STATIC ${CORE_SRC})
target_include_directories(skillctl_core PUBLIC core)

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
