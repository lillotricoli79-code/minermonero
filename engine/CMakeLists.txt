cmake_minimum_required(VERSION 3.24)
project(UniqueRX LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(randomx_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/randomx/upstream")
if(NOT EXISTS "${randomx_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Vendored RandomX rx/0 source tree is missing; network fetching is disabled.")
endif()
add_subdirectory(src/randomx/upstream)
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party")

add_executable(unique-rx src/logging/log.cpp src/main.cpp src/stratum/client.cpp src/stratum/job.cpp\n  src/hashvault/api.cpp
  src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/randomx/benchmark.cpp
  src/core/mining.cpp src/workers/hash_worker.cpp src/msr/msr.cpp src/stratum/transport.cpp
  src/telemetry/system.cpp)
target_compile_features(unique-rx PRIVATE cxx_std_20)
target_include_directories(unique-rx PRIVATE src "${randomx_SOURCE_DIR}/src")
target_link_libraries(unique-rx PRIVATE randomx Threads::Threads OpenSSL::SSL
  OpenSSL::Crypto nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(unique-rx PRIVATE ws2_32 advapi32 psapi powrprof)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/msr/IntelMSR.bin")
    add_custom_command(TARGET unique-rx POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:unique-rx>/msr"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/msr/IntelMSR.bin"
        "$<TARGET_FILE_DIR:unique-rx>/msr/IntelMSR.bin")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(unique-rx-logging-tests tests/logging/logging_rotation_test.cpp
    src/logging/log.cpp)
  target_compile_features(unique-rx-logging-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-logging-tests PRIVATE src)
  target_link_libraries(unique-rx-logging-tests PRIVATE Threads::Threads)
  add_test(NAME unique-rx-logging-rotation COMMAND unique-rx-logging-tests)

  add_test(NAME unique-rx-cli-rejects-invalid-max-hashes
    COMMAND unique-rx --mine config.json not-a-number)
  set_tests_properties(unique-rx-cli-rejects-invalid-max-hashes PROPERTIES WILL_FAIL TRUE)
  add_test(NAME unique-rx-cli-rejects-extra-mine-arguments
    COMMAND unique-rx --mine config.json 100 extra)
  set_tests_properties(unique-rx-cli-rejects-extra-mine-arguments PROPERTIES WILL_FAIL TRUE)
  add_test(NAME unique-rx-cli-rejects-negative-max-hashes
    COMMAND unique-rx --mine config.json -1)
  set_tests_properties(unique-rx-cli-rejects-negative-max-hashes PROPERTIES WILL_FAIL TRUE)
  add_test(NAME unique-rx-cli-rejects-whitespace-max-hashes
    COMMAND unique-rx --mine config.json " 1")
  set_tests_properties(unique-rx-cli-rejects-whitespace-max-hashes PROPERTIES WILL_FAIL TRUE)
  add_test(NAME unique-rx-cli-rejects-overflow-max-hashes
    COMMAND unique-rx --mine config.json 18446744073709551616)
  set_tests_properties(unique-rx-cli-rejects-overflow-max-hashes PROPERTIES WILL_FAIL TRUE)

  add_test(NAME unique-rx-cli-rejects-zero-threads
    COMMAND unique-rx --mine config.json --threads 0)
  set_tests_properties(unique-rx-cli-rejects-zero-threads PROPERTIES WILL_FAIL TRUE)
  add_test(NAME unique-rx-cli-rejects-too-many-threads
    COMMAND unique-rx --mine config.json --threads 257)
  set_tests_properties(unique-rx-cli-rejects-too-many-threads PROPERTIES WILL_FAIL TRUE)
  add_test(NAME unique-rx-cli-rejects-invalid-threads
    COMMAND unique-rx --mine config.json --threads invalid)
  set_tests_properties(unique-rx-cli-rejects-invalid-threads PROPERTIES WILL_FAIL TRUE)

  add_executable(unique-rx-cpu-topology-tests tests/cpu/topology_test.cpp src/cpu/topology.cpp)
  target_compile_features(unique-rx-cpu-topology-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-cpu-topology-tests PRIVATE src "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-cpu-topology-tests PRIVATE randomx Threads::Threads
    nlohmann_json::nlohmann_json)
  add_test(NAME unique-rx-cpu-topology COMMAND unique-rx-cpu-topology-tests)

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
    src/logging/log.cpp src/core/mining.cpp src/workers/hash_worker.cpp)
  target_compile_features(unique-rx-worker-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-worker-tests PRIVATE src "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-worker-tests PRIVATE randomx Threads::Threads
    nlohmann_json::nlohmann_json)
  add_test(NAME unique-rx-worker-validation COMMAND unique-rx-worker-tests)

  add_executable(unique-rx-msr-tests tests/msr/msr_manager_test.cpp
    src/msr/msr.cpp src/logging/log.cpp)
  target_compile_features(unique-rx-msr-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-msr-tests PRIVATE src)
  if(WIN32)
    target_link_libraries(unique-rx-msr-tests PRIVATE advapi32 OpenSSL::Crypto)
  endif()
  target_link_libraries(unique-rx-msr-tests PRIVATE Threads::Threads)
  add_test(NAME unique-rx-msr-lifecycle COMMAND unique-rx-msr-tests)

  add_executable(unique-rx-stratum-tests tests/stratum/job_validation_test.cpp src/logging/log.cpp src/stratum/client.cpp
    src/stratum/job.cpp src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/core/mining.cpp src/workers/hash_worker.cpp src/msr/msr.cpp src/stratum/transport.cpp src/telemetry/system.cpp)
  target_compile_features(unique-rx-stratum-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-stratum-tests PRIVATE src
    "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-stratum-tests PRIVATE randomx Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(unique-rx-stratum-tests PRIVATE ws2_32 advapi32 psapi powrprof)
  endif()
  add_test(NAME unique-rx-stratum-job-validation COMMAND unique-rx-stratum-tests)

  # This harness only hosts a mock TCP pool and launches the production
  # unique-rx executable as a child. Do not link the miner engine into the
  # harness: doing so duplicates its CRT, logger and worker runtime on Windows.
  add_executable(unique-rx-stratum-integration-tests
    tests/stratum/loopback_test.cpp)
  target_compile_features(unique-rx-stratum-integration-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-stratum-integration-tests PRIVATE src)
  target_link_libraries(unique-rx-stratum-integration-tests PRIVATE
    Threads::Threads nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(unique-rx-stratum-integration-tests PRIVATE ws2_32)
  endif()
  add_test(NAME unique-rx-stratum-loopback COMMAND unique-rx-stratum-integration-tests $<TARGET_FILE:unique-rx>)
  set_tests_properties(unique-rx-stratum-loopback PROPERTIES TIMEOUT 45)

  if(RANDOMX_VAES_ENABLED)
    add_executable(unique-rx-vaes-equivalence-tests tests/randomx/vaes_equivalence_test.cpp)
    target_compile_features(unique-rx-vaes-equivalence-tests PRIVATE cxx_std_20)
    target_compile_definitions(unique-rx-vaes-equivalence-tests PRIVATE RANDOMX_HAVE_VAES=1)
    target_include_directories(unique-rx-vaes-equivalence-tests PRIVATE src "${randomx_SOURCE_DIR}/src")
    target_link_libraries(unique-rx-vaes-equivalence-tests PRIVATE randomx)
    add_test(NAME unique-rx-vaes-equivalence COMMAND unique-rx-vaes-equivalence-tests)
  endif()

  if(TARGET randomx-tests)
    add_test(NAME randomx-upstream-consensus COMMAND randomx-tests)
  endif()
endif()