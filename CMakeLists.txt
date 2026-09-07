cmake_minimum_required(VERSION 3.20)

set(REDIS_PVXS_IOC_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT EXISTS "${REDIS_PVXS_IOC_VERSION_FILE}")
  message(FATAL_ERROR "Missing VERSION file: ${REDIS_PVXS_IOC_VERSION_FILE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REDIS_PVXS_IOC_VERSION_FILE}")

file(READ "${REDIS_PVXS_IOC_VERSION_FILE}" REDIS_PVXS_IOC_VERSION)
string(STRIP "${REDIS_PVXS_IOC_VERSION}" REDIS_PVXS_IOC_VERSION)
if(NOT REDIS_PVXS_IOC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain MAJOR.MINOR.PATCH, got: ${REDIS_PVXS_IOC_VERSION}")
endif()

project(redis-pvxs-ioc VERSION "${REDIS_PVXS_IOC_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REDIS_PVXS_IOC_BUILD_TESTS "Build redis-pvxs-ioc unit tests" ON)
option(REDIS_PVXS_IOC_ENABLE_GRPC "Build reflected PVA RPC to gRPC forwarding" ON)
option(REDIS_PVXS_IOC_ENABLE_CHANNELFINDER "Build the one-shot ChannelFinder publication tool" ON)

set(EPICS_BASE "${CMAKE_SOURCE_DIR}/third_party/epics-base" CACHE PATH "Path to built epics-base")
set(PVXS_ROOT "${CMAKE_SOURCE_DIR}/third_party/pvxs" CACHE PATH "Path to built pvxs")

find_package(Perl REQUIRED)
if(REDIS_PVXS_IOC_ENABLE_CHANNELFINDER)
  find_package(CURL REQUIRED)
endif()

# ---- gRPC + protobuf for the RPC->gRPC forwarding feature ------------------
# Provided by the dev packages installed in the Dockerfile
# (libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc) or a
# source build. Protobuf in MODULE mode (Ubuntu ships no ProtobufConfig.cmake;
# CMake's FindProtobuf provides Protobuf_PROTOC_EXECUTABLE + protobuf::libprotobuf).
# gRPC ships gRPCConfig.cmake (gRPC::grpc++ + gRPC::grpc_cpp_plugin).
if(REDIS_PVXS_IOC_ENABLE_GRPC)
  find_package(Protobuf REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
endif()

set(
  REDIS_PVXS_IOC_GIT_REVISION_OVERRIDE
  ""
  CACHE STRING
  "Explicit git revision to embed in version metadata"
)

if(REDIS_PVXS_IOC_GIT_REVISION_OVERRIDE)
  set(REDIS_PVXS_IOC_GIT_REVISION "${REDIS_PVXS_IOC_GIT_REVISION_OVERRIDE}")
elseif(DEFINED ENV{REDIS_PVXS_IOC_GIT_REVISION} AND NOT "$ENV{REDIS_PVXS_IOC_GIT_REVISION}" STREQUAL "")
  set(REDIS_PVXS_IOC_GIT_REVISION "$ENV{REDIS_PVXS_IOC_GIT_REVISION}")
else()
  set(REDIS_PVXS_IOC_GIT_REVISION "unknown")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
      RESULT_VARIABLE REDIS_PVXS_IOC_GIT_REVISION_RESULT
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE REDIS_PVXS_IOC_GIT_REVISION_CANDIDATE
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(REDIS_PVXS_IOC_GIT_REVISION_RESULT EQUAL 0
        AND NOT REDIS_PVXS_IOC_GIT_REVISION_CANDIDATE STREQUAL "")
      set(REDIS_PVXS_IOC_GIT_REVISION "${REDIS_PVXS_IOC_GIT_REVISION_CANDIDATE}")
    endif()
  endif()
endif()

set(REDIS_PVXS_IOC_GENERATED_INCLUDE_DIR
  "${CMAKE_BINARY_DIR}/generated/include/redis_pvxs_ioc")
file(MAKE_DIRECTORY "${REDIS_PVXS_IOC_GENERATED_INCLUDE_DIR}")

configure_file(
  "${CMAKE_SOURCE_DIR}/include/redis_pvxs_ioc/version.h.in"
  "${REDIS_PVXS_IOC_GENERATED_INCLUDE_DIR}/version.h"
  @ONLY
)

if(NOT DEFINED EPICS_HOST_ARCH OR EPICS_HOST_ARCH STREQUAL "")
  execute_process(
    COMMAND ${PERL_EXECUTABLE} ${EPICS_BASE}/lib/perl/EpicsHostArch.pl
    OUTPUT_VARIABLE EPICS_HOST_ARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

message(STATUS "redis-pvxs-ioc VERSION=${PROJECT_VERSION}")
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
      "Build epics-base and pvxs first. See docs/building-from-source.md for the dependency bootstrap steps.")
  endif()
endforeach()

set(PVXS_EXTRA_INCLUDE_DIRS)
set(PVXS_EXTRA_LINK_LIBS)
if(EXISTS "${PVXS_BUNDLE_EVENT_LIB}")
  list(APPEND PVXS_EXTRA_INCLUDE_DIRS "${PVXS_USR}/include")
  # PVXS already links event_core and event_pthreads. Linking monolithic
  # libevent as well creates two copies of its global threading state on macOS
  # and can deadlock server construction. Follow PVXS's transitive dependencies.
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "Missing bundled libevent: ${PVXS_BUNDLE_EVENT_LIB}\n"
    "Build pvxs/bundle libevent before configuring this project.")
endif()

set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "Build yaml-cpp as a static library" FORCE)
add_subdirectory(third_party/yaml-cpp)
add_subdirectory(third_party/redis-adapter)

# ---- Generated gRPC server-reflection stubs --------------------------------
# The IOC compiles in only the standard gRPC reflection proto; it discovers any
# backend service's schema at runtime, so it carries no application proto.
if(REDIS_PVXS_IOC_ENABLE_GRPC)
set(REFLECTION_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/reflection.proto")
set(BRIDGE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-proto")
file(MAKE_DIRECTORY "${BRIDGE_GEN_DIR}")

set(REFLECTION_PB_CC   "${BRIDGE_GEN_DIR}/reflection.pb.cc")
set(REFLECTION_PB_H    "${BRIDGE_GEN_DIR}/reflection.pb.h")
set(REFLECTION_GRPC_CC "${BRIDGE_GEN_DIR}/reflection.grpc.pb.cc")
set(REFLECTION_GRPC_H  "${BRIDGE_GEN_DIR}/reflection.grpc.pb.h")

add_custom_command(
  OUTPUT "${REFLECTION_PB_CC}" "${REFLECTION_PB_H}"
         "${REFLECTION_GRPC_CC}" "${REFLECTION_GRPC_H}"
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
          --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
          --cpp_out=${BRIDGE_GEN_DIR}
          --grpc_out=${BRIDGE_GEN_DIR}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          ${REFLECTION_PROTO}
  DEPENDS "${REFLECTION_PROTO}"
  COMMENT "Generating gRPC reflection protobuf + gRPC C++")

add_library(bpm-query-proto STATIC "${REFLECTION_PB_CC}" "${REFLECTION_GRPC_CC}")
target_link_libraries(bpm-query-proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_include_directories(bpm-query-proto PUBLIC "${BRIDGE_GEN_DIR}" ${Protobuf_INCLUDE_DIRS})
set_target_properties(bpm-query-proto PROPERTIES POSITION_INDEPENDENT_CODE ON)

endif()

add_library(redis-pvxs-ioc-core STATIC
  src/access_control.cpp
  src/config.cpp
  src/config_diff.cpp
  src/discovery.cpp
  src/util.cpp
  src/alarm_publisher.cpp
  src/runtime.cpp
  src/app.cpp
  ${REDIS_ADAPTER_SOURCES}
)

target_include_directories(redis-pvxs-ioc-core
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated/include
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

target_compile_definitions(redis-pvxs-ioc-core PUBLIC
  REDIS_PVXS_IOC_ENABLE_GRPC=$<BOOL:${REDIS_PVXS_IOC_ENABLE_GRPC}>
  REDIS_PVXS_IOC_ENABLE_CHANNELFINDER=$<BOOL:${REDIS_PVXS_IOC_ENABLE_CHANNELFINDER}>)
if(REDIS_PVXS_IOC_ENABLE_GRPC)
  target_sources(redis-pvxs-ioc-core PRIVATE src/grpc_bridge.cpp src/rpc_pv.cpp)
  target_link_libraries(redis-pvxs-ioc-core PUBLIC bpm-query-proto)
endif()

# Require the narrow Base extension used to refresh live HAG membership.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${EPICS_BASE}/include" "${EPICS_BASE}/include/${EPICS_COMPILER_DIR}" "${EPICS_BASE}/include/${EPICS_OS_DIR}")
set(CMAKE_REQUIRED_LIBRARIES "${EPICS_BASE_LIB}/libCom${SHARED_LIB_EXT}")
check_cxx_source_compiles("#include <asLib.h>\nint main() { unsigned changed = 0; return asRefreshHag(&changed); }" REDIS_PVXS_IOC_HAS_HAG_REFRESH)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT REDIS_PVXS_IOC_HAS_HAG_REFRESH)
  message(FATAL_ERROR "EPICS Base must provide asRefreshHag(). Build the pinned Base extension; see docs/access-control.md.")
endif()

add_executable(redis-pvxs-ioc src/main.cpp)
target_link_libraries(redis-pvxs-ioc PRIVATE redis-pvxs-ioc-core)

if(REDIS_PVXS_IOC_ENABLE_CHANNELFINDER)
  add_library(redis-pvxs-channelfinder STATIC src/channelfinder.cpp)
  target_link_libraries(redis-pvxs-channelfinder PUBLIC redis-pvxs-ioc-core)
  add_executable(redis-pvxs-channelfinder-sync tools/channelfinder_sync.cpp)
  target_link_libraries(redis-pvxs-channelfinder-sync PRIVATE redis-pvxs-channelfinder CURL::libcurl)
  install(TARGETS redis-pvxs-channelfinder-sync RUNTIME DESTINATION bin)
endif()

add_executable(redis-pvxs-access-benchmark tools/access_benchmark.cpp)
target_link_libraries(redis-pvxs-access-benchmark PRIVATE redis-pvxs-ioc-core)

install(TARGETS redis-pvxs-ioc RUNTIME DESTINATION bin)

if(REDIS_PVXS_IOC_BUILD_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(REDIS_SERVER_EXECUTABLE redis-server REQUIRED)
  add_test(NAME config_cli_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/config_cli_tests.py $<TARGET_FILE:redis-pvxs-ioc>)
  set_tests_properties(config_cli_tests PROPERTIES TIMEOUT 60)
  add_executable(runtime_tests tests/runtime_tests.cpp)
  target_link_libraries(runtime_tests PRIVATE redis-pvxs-ioc-core)
  target_compile_options(runtime_tests PRIVATE -UNDEBUG)
  add_test(NAME runtime_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/run-with-redis.py --redis-server ${REDIS_SERVER_EXECUTABLE}
    -- $<TARGET_FILE:runtime_tests>)
  set_tests_properties(runtime_tests PROPERTIES TIMEOUT 75)

  add_test(NAME discovery_e2e COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/run-with-redis.py --redis-server ${REDIS_SERVER_EXECUTABLE}
    -- ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/discovery_e2e.py
    --ioc $<TARGET_FILE:redis-pvxs-ioc> --pvxget ${PVXS_ROOT}/bin/${EPICS_HOST_ARCH}/pvxget)
  set_tests_properties(discovery_e2e PROPERTIES TIMEOUT 75)

  add_executable(discovery_tests tests/discovery_tests.cpp)
  target_link_libraries(discovery_tests PRIVATE redis-pvxs-ioc-core)
  target_compile_options(discovery_tests PRIVATE -UNDEBUG)
  add_test(NAME discovery_tests COMMAND discovery_tests)
  set_tests_properties(discovery_tests PROPERTIES TIMEOUT 15)

  add_executable(config_tests tests/config_tests.cpp)
  target_link_libraries(config_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME config_tests COMMAND config_tests)

  add_executable(access_control_tests tests/access_control_tests.cpp)
  target_link_libraries(access_control_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME access_control_tests COMMAND access_control_tests)

  add_executable(access_runtime_tests tests/access_runtime_tests.cpp)
  target_link_libraries(access_runtime_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME access_runtime_tests COMMAND access_runtime_tests)

  add_executable(model_tests tests/model_tests.cpp)
  target_link_libraries(model_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME model_tests COMMAND model_tests)

  add_executable(alarm_publisher_tests tests/alarm_publisher_tests.cpp)
  target_link_libraries(alarm_publisher_tests PRIVATE redis-pvxs-ioc-core)
  add_test(NAME alarm_publisher_tests COMMAND alarm_publisher_tests)

  if(REDIS_PVXS_IOC_ENABLE_CHANNELFINDER)
    add_executable(channelfinder_tests tests/channelfinder_tests.cpp)
    target_link_libraries(channelfinder_tests PRIVATE redis-pvxs-channelfinder)
    target_compile_options(channelfinder_tests PRIVATE -UNDEBUG)
    add_test(NAME channelfinder_tests COMMAND channelfinder_tests)
  endif()

  # redis-plus-plus supplies a default Release build type when callers don't
  # specify one.  These small test executables use assert() as their test
  # primitive, so keep assertions active even in that configuration.
  foreach(_test_target
      config_tests
      access_control_tests
      access_runtime_tests
      model_tests
      alarm_publisher_tests)
    target_compile_options(${_test_target} PRIVATE -UNDEBUG)
    set_tests_properties(${_test_target} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
