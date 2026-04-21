cmake_minimum_required(VERSION 3.20)
project(redis-pvxs-ioc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REDIS_PVXS_IOC_BUILD_TESTS "Build redis-pvxs-ioc unit tests" ON)

set(EPICS_BASE "${CMAKE_SOURCE_DIR}/third_party/epics-base" CACHE PATH "Path to built epics-base")
set(PVXS_ROOT "${CMAKE_SOURCE_DIR}/third_party/pvxs" CACHE PATH "Path to built pvxs")

find_package(Perl REQUIRED)

if(NOT DEFINED EPICS_HOST_ARCH OR EPICS_HOST_ARCH STREQUAL "")
  execute_process(
    COMMAND ${PERL_EXECUTABLE} ${EPICS_BASE}/lib/perl/EpicsHostArch.pl
    OUTPUT_VARIABLE EPICS_HOST_ARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

message(STATUS "EPICS_BASE=${EPICS_BASE}")
message(STATUS "PVXS_ROOT=${PVXS_ROOT}")
message(STATUS "EPICS_HOST_ARCH=${EPICS_HOST_ARCH}")

set(EPICS_BASE_LIB "${EPICS_BASE}/lib/${EPICS_HOST_ARCH}")
set(PVXS_LIB "${PVXS_ROOT}/lib/${EPICS_HOST_ARCH}")
set(PVXS_USR "${PVXS_ROOT}/bundle/usr/${EPICS_HOST_ARCH}")
set(SHARED_LIB_EXT "${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(PVXS_BUNDLE_EVENT_LIB "${PVXS_USR}/lib/libevent${SHARED_LIB_EXT}")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(EPICS_COMPILER_DIR "compiler/clang")
else()
  set(EPICS_COMPILER_DIR "compiler/gcc")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(EPICS_OS_DIR "os/Darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(EPICS_OS_DIR "os/Linux")
else()
  message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()

foreach(_required
    "${EPICS_BASE_LIB}/libCom${SHARED_LIB_EXT}"
    "${PVXS_LIB}/libpvxs${SHARED_LIB_EXT}")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR
      "Missing built dependency: ${_required}\n"
      "Build epics-base and pvxs first. See README.md for the dependency bootstrap steps.")
  endif()
endforeach()

set(PVXS_EXTRA_INCLUDE_DIRS)
set(PVXS_EXTRA_LINK_LIBS)
if(EXISTS "${PVXS_BUNDLE_EVENT_LIB}")
  list(APPEND PVXS_EXTRA_INCLUDE_DIRS "${PVXS_USR}/include")
  list(APPEND PVXS_EXTRA_LINK_LIBS "${PVXS_BUNDLE_EVENT_LIB}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "Missing bundled libevent: ${PVXS_BUNDLE_EVENT_LIB}\n"
    "Build pvxs/bundle libevent before configuring this project.")
endif()

set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "Build yaml-cpp as a static library" FORCE)
add_subdirectory(third_party/yaml-cpp)
add_subdirectory(third_party/redis-adapter)

add_library(redis-pvxs-ioc-core STATIC
  src/config.cpp
  src/util.cpp
  src/alarm_publisher.cpp
  src/runtime.cpp
  src/app.cpp
  ${REDIS_ADAPTER_SOURCES}
)

target_include_directories(redis-pvxs-ioc-core
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${EPICS_BASE}/include
    ${EPICS_BASE}/include/${EPICS_COMPILER_DIR}
    ${EPICS_BASE}/include/${EPICS_OS_DIR}
    ${PVXS_ROOT}/include
    ${PVXS_EXTRA_INCLUDE_DIRS}
    ${REDIS_ADAPTER_INCLUDE_DIRS}
)

target_link_libraries(redis-pvxs-ioc-core
  PUBLIC
    yaml-cpp
    ${REDIS_ADAPTER_LIBRARIES}
    ${EPICS_BASE_LIB}/libCom${SHARED_LIB_EXT}
    ${PVXS_LIB}/libpvxs${SHARED_LIB_EXT}
    ${PVXS_EXTRA_LINK_LIBS}
)

add_executable(redis-pvxs-ioc src/main.cpp)
target_link_libraries(redis-pvxs-ioc PRIVATE redis-pvxs-ioc-core)

install(TARGETS redis-pvxs-ioc RUNTIME DESTINATION bin)

if(REDIS_PVXS_IOC_BUILD_TESTS)
  enable_testing()

  add_executable(config_tests tests/config_tests.cpp)
  target_link_libraries(config_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME config_tests COMMAND config_tests)

  add_executable(model_tests tests/model_tests.cpp)
  target_link_libraries(model_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME model_tests COMMAND model_tests)
endif()
