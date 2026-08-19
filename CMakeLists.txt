cmake_minimum_required(VERSION 3.24)

project(vectordb VERSION 0.1.0 LANGUAGES CXX)

option(VDB_BUILD_TESTS "Build VectorDB tests" ON)
option(VDB_BUILD_TOOLS "Build VectorDB tools" ON)
option(VDB_ENABLE_SANITIZERS "Enable platform-appropriate runtime sanitizers" OFF)
option(VDB_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(VDB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(VDB_BUILD_GRPC "Build gRPC network services when gRPC is available" ON)

if((VDB_ENABLE_SANITIZERS AND VDB_ENABLE_ASAN) OR
   (VDB_ENABLE_SANITIZERS AND VDB_ENABLE_TSAN) OR
   (VDB_ENABLE_ASAN AND VDB_ENABLE_TSAN))
  message(FATAL_ERROR "Select only one VectorDB sanitizer mode")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

add_library(vectordb_core
  core/src/distance.cpp
  core/src/wal.cpp
  core/src/segment.cpp
  core/src/hnsw.cpp
  core/src/engine.cpp
)
add_library(vectordb::core ALIAS vectordb_core)

target_include_directories(vectordb_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(vectordb_core PUBLIC cxx_std_20)
vdb_enable_warnings(vectordb_core)
vdb_enable_sanitizers(vectordb_core)

if(APPLE)
  target_compile_definitions(vectordb_core PRIVATE VDB_PLATFORM_APPLE=1)
endif()

add_library(vectordb_shard
  services/shard/src/service.cpp
)
add_library(vectordb::shard ALIAS vectordb_shard)
target_include_directories(vectordb_shard
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/services/shard/include>
)
target_link_libraries(vectordb_shard PUBLIC vectordb::core)
vdb_enable_warnings(vectordb_shard)
vdb_enable_sanitizers(vectordb_shard)

add_library(vectordb_coordinator
  services/coordinator/src/directory.cpp
)
add_library(vectordb::coordinator ALIAS vectordb_coordinator)
target_include_directories(vectordb_coordinator
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/services/coordinator/include>
)
target_link_libraries(vectordb_coordinator PUBLIC vectordb::core)
vdb_enable_warnings(vectordb_coordinator)
vdb_enable_sanitizers(vectordb_coordinator)

find_program(VDB_PROTOC_EXECUTABLE protoc)
set(VDB_PROTO_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/vectordb/v1/admin.proto
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/vectordb/v1/records.proto
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/vectordb/v1/search.proto
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/vectordb/v1/cluster.proto
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/vectordb/v1/telemetry.proto
)
set(VDB_PROTO_RELATIVE_FILES
  vectordb/v1/admin.proto
  vectordb/v1/records.proto
  vectordb/v1/search.proto
  vectordb/v1/cluster.proto
  vectordb/v1/telemetry.proto
)
if(VDB_PROTOC_EXECUTABLE)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/vectordb-contract.pb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${VDB_PROTOC_EXECUTABLE}
      --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
      --descriptor_set_out=${CMAKE_CURRENT_BINARY_DIR}/generated/vectordb-contract.pb
      ${VDB_PROTO_FILES}
    DEPENDS ${VDB_PROTO_FILES}
    VERBATIM
  )
  add_custom_target(vectordb-proto-contract ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/vectordb-contract.pb)
endif()

if(VDB_BUILD_GRPC)
  find_package(Protobuf CONFIG QUIET)
  find_package(gRPC CONFIG QUIET)
  if(Protobuf_FOUND AND gRPC_FOUND)
    set(VDB_GRPC_AVAILABLE TRUE)
    set(VDB_PROTO_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(VDB_PROTO_GENERATED_SOURCES)
    foreach(VDB_PROTO_RELATIVE_FILE IN LISTS VDB_PROTO_RELATIVE_FILES)
      string(REGEX REPLACE "\\.proto$" "" VDB_PROTO_STEM ${VDB_PROTO_RELATIVE_FILE})
      set(VDB_PROTO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/proto/${VDB_PROTO_RELATIVE_FILE})
      set(VDB_PROTO_CPP ${VDB_PROTO_GENERATED_DIR}/${VDB_PROTO_STEM}.pb.cc)
      set(VDB_PROTO_HEADER ${VDB_PROTO_GENERATED_DIR}/${VDB_PROTO_STEM}.pb.h)
      set(VDB_GRPC_CPP ${VDB_PROTO_GENERATED_DIR}/${VDB_PROTO_STEM}.grpc.pb.cc)
      set(VDB_GRPC_HEADER ${VDB_PROTO_GENERATED_DIR}/${VDB_PROTO_STEM}.grpc.pb.h)
      add_custom_command(
        OUTPUT ${VDB_PROTO_CPP} ${VDB_PROTO_HEADER} ${VDB_GRPC_CPP} ${VDB_GRPC_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VDB_PROTO_GENERATED_DIR}/vectordb/v1
        COMMAND $<TARGET_FILE:protobuf::protoc>
          --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
          --cpp_out=${VDB_PROTO_GENERATED_DIR}
          --grpc_out=${VDB_PROTO_GENERATED_DIR}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          ${VDB_PROTO_RELATIVE_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/proto
        DEPENDS ${VDB_PROTO_SOURCE} protobuf::protoc gRPC::grpc_cpp_plugin
        VERBATIM
      )
      list(APPEND VDB_PROTO_GENERATED_SOURCES ${VDB_PROTO_CPP} ${VDB_GRPC_CPP})
    endforeach()
    add_library(vectordb_proto STATIC ${VDB_PROTO_GENERATED_SOURCES})
    target_include_directories(vectordb_proto PUBLIC ${VDB_PROTO_GENERATED_DIR})
    target_link_libraries(vectordb_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
    # Protobuf emits source files controlled by the installed protoc version. They are
    # treated as third-party generated code; -Werror remains enabled on every hand-written target.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(vectordb_proto PRIVATE -Wno-nullability-extension)
    endif()

    add_library(vectordb_grpc services/shard/src/grpc_service.cpp)
    target_include_directories(vectordb_grpc PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/services/shard/include>
    )
    target_link_libraries(vectordb_grpc PUBLIC vectordb::shard vectordb_proto)
    vdb_enable_warnings(vectordb_grpc)
    vdb_enable_sanitizers(vectordb_grpc)

    add_library(vectordb_coordinator_grpc services/coordinator/src/grpc_service.cpp)
    target_include_directories(vectordb_coordinator_grpc PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/services/coordinator/include>
    )
    target_link_libraries(vectordb_coordinator_grpc PUBLIC vectordb::coordinator vectordb_proto)
    vdb_enable_warnings(vectordb_coordinator_grpc)
    vdb_enable_sanitizers(vectordb_coordinator_grpc)

    if(VDB_BUILD_TOOLS)
      add_executable(vectordb-shardd services/shard/src/main.cpp)
      target_link_libraries(vectordb-shardd PRIVATE vectordb_grpc)
      vdb_enable_warnings(vectordb-shardd)
      vdb_enable_sanitizers(vectordb-shardd)

      add_executable(vectordb-coordinatord services/coordinator/src/main.cpp)
      target_link_libraries(vectordb-coordinatord PRIVATE vectordb_coordinator_grpc)
      vdb_enable_warnings(vectordb-coordinatord)
      vdb_enable_sanitizers(vectordb-coordinatord)
    endif()
  else()
    message(STATUS "gRPC/Protobuf packages not found; network services are disabled")
  endif()
endif()

if(VDB_BUILD_TOOLS)
  add_executable(vectordb-cli tools/vectordb-cli/main.cpp)
  target_link_libraries(vectordb-cli PRIVATE vectordb::core)
  vdb_enable_warnings(vectordb-cli)
  vdb_enable_sanitizers(vectordb-cli)
endif()

if(VDB_BUILD_TESTS)
  enable_testing()
  add_executable(vectordb-tests
    tests/unit/main.cpp
    tests/unit/distance_test.cpp
    tests/unit/wal_test.cpp
    tests/unit/hnsw_test.cpp
    tests/unit/sq8_test.cpp
    tests/unit/engine_test.cpp
    tests/unit/shard_service_test.cpp
    tests/unit/directory_test.cpp
  )
  target_link_libraries(vectordb-tests PRIVATE vectordb::shard vectordb::coordinator)
  if(VDB_GRPC_AVAILABLE)
    add_executable(vectordb-grpc-tests
      tests/unit/main.cpp
      tests/integration/grpc_service_test.cpp
      tests/integration/coordinator_grpc_test.cpp
    )
    target_include_directories(vectordb-grpc-tests PRIVATE tests/unit)
    target_link_libraries(vectordb-grpc-tests PRIVATE vectordb_grpc vectordb_coordinator_grpc)
    vdb_enable_warnings(vectordb-grpc-tests)
    vdb_enable_sanitizers(vectordb-grpc-tests)

    add_test(NAME vectordb.grpc COMMAND vectordb-grpc-tests)
  endif()
  target_include_directories(vectordb-tests PRIVATE tests/unit)
  vdb_enable_warnings(vectordb-tests)
  vdb_enable_sanitizers(vectordb-tests)
  add_test(NAME vectordb.unit COMMAND vectordb-tests)
endif()
