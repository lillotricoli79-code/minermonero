cmake_minimum_required(VERSION 3.24)
project(UniqueRX LANGUAGES C CXX)

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(randomx_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/randomx/upstream")
if(NOT EXISTS "${randomx_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Vendored RandomX rx/0 source tree is missing; network fetching is disabled.")
endif()
add_subdirectory(src/randomx/upstream)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(unique-rx src/main.cpp src/stratum/client.cpp src/stratum/job.cpp
  src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/randomx/benchmark.cpp
  src/core/mining.cpp src/workers/hash_worker.cpp src/msr/msr.cpp src/stratum/transport.cpp)
target_compile_features(unique-rx PRIVATE cxx_std_20)
target_include_directories(unique-rx PRIVATE src "${randomx_SOURCE_DIR}/src")
target_link_libraries(unique-rx PRIVATE randomx Threads::Threads OpenSSL::SSL
  OpenSSL::Crypto nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(unique-rx PRIVATE ws2_32 advapi32)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/msr/IntelMSR.bin")
    add_custom_command(TARGET unique-rx POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:unique-rx>/msr"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/msr/IntelMSR.bin"
        "$<TARGET_FILE_DIR:unique-rx>/msr/IntelMSR.bin")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(unique-rx-jit-tests tests/randomx/jit_codegen_test.cpp)
  target_compile_features(unique-rx-jit-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-jit-tests PRIVATE src "${randomx_SOURCE_DIR}/src")
  add_test(NAME unique-rx-x64-codegen COMMAND unique-rx-jit-tests)

  add_executable(unique-rx-tests tests/randomx/hash_pipeline_test.cpp)
  target_compile_features(unique-rx-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-tests PRIVATE "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-tests PRIVATE randomx Threads::Threads)
  add_test(NAME unique-rx-batch-and-lane-equivalence COMMAND unique-rx-tests)

  add_executable(unique-rx-worker-tests tests/workers/worker_validation_test.cpp
    src/core/mining.cpp src/workers/hash_worker.cpp)
  target_compile_features(unique-rx-worker-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-worker-tests PRIVATE src "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-worker-tests PRIVATE randomx Threads::Threads
    nlohmann_json::nlohmann_json)
  add_test(NAME unique-rx-worker-validation COMMAND unique-rx-worker-tests)

  add_executable(unique-rx-msr-tests tests/msr/msr_manager_test.cpp src/msr/msr.cpp)
  target_compile_features(unique-rx-msr-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-msr-tests PRIVATE src)
  if(WIN32)
    target_link_libraries(unique-rx-msr-tests PRIVATE advapi32)
  endif()
  add_test(NAME unique-rx-msr-lifecycle COMMAND unique-rx-msr-tests)

  add_executable(unique-rx-stratum-tests tests/stratum/job_validation_test.cpp src/stratum/client.cpp
    src/stratum/job.cpp src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/core/mining.cpp src/workers/hash_worker.cpp src/msr/msr.cpp src/stratum/transport.cpp)
  target_compile_features(unique-rx-stratum-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-stratum-tests PRIVATE src
    "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-stratum-tests PRIVATE randomx Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(unique-rx-stratum-tests PRIVATE ws2_32 advapi32)
  endif()
  add_test(NAME unique-rx-stratum-job-validation COMMAND unique-rx-stratum-tests)

  add_executable(unique-rx-stratum-integration-tests
    tests/stratum/loopback_test.cpp src/stratum/client.cpp src/stratum/job.cpp
    src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/core/mining.cpp src/workers/hash_worker.cpp src/msr/msr.cpp
    src/stratum/transport.cpp)
  target_compile_features(unique-rx-stratum-integration-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-stratum-integration-tests PRIVATE src
    "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-stratum-integration-tests PRIVATE randomx
    Threads::Threads OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(unique-rx-stratum-integration-tests PRIVATE ws2_32 advapi32)
  endif()
  add_test(NAME unique-rx-stratum-loopback COMMAND unique-rx-stratum-integration-tests)
  set_tests_properties(unique-rx-stratum-loopback PROPERTIES TIMEOUT 45)

  if(TARGET randomx-tests)
    add_test(NAME randomx-upstream-consensus COMMAND randomx-tests)
  endif()
endif()