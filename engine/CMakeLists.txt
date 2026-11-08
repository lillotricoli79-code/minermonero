cmake_minimum_required(VERSION 3.24)
project(UniqueRX LANGUAGES C CXX)

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

FetchContent_Declare(
  randomx
  GIT_REPOSITORY https://github.com/tevador/RandomX.git
  GIT_TAG aaafe71322df6602c21a5c72937ac284724ae561
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(randomx)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(unique-rx src/main.cpp src/stratum/client.cpp src/stratum/job.cpp
  src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/randomx/benchmark.cpp
  src/core/mining.cpp src/stratum/transport.cpp)
target_compile_features(unique-rx PRIVATE cxx_std_20)
target_include_directories(unique-rx PRIVATE src "${randomx_SOURCE_DIR}/src")
target_link_libraries(unique-rx PRIVATE randomx Threads::Threads OpenSSL::SSL
  OpenSSL::Crypto nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(unique-rx PRIVATE ws2_32)
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

  add_executable(unique-rx-stratum-tests tests/stratum/job_validation_test.cpp src/stratum/client.cpp
    src/stratum/job.cpp src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/core/mining.cpp src/stratum/transport.cpp)
  target_compile_features(unique-rx-stratum-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-stratum-tests PRIVATE src
    "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-stratum-tests PRIVATE randomx Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(unique-rx-stratum-tests PRIVATE ws2_32)
  endif()
  add_test(NAME unique-rx-stratum-job-validation COMMAND unique-rx-stratum-tests)

  add_executable(unique-rx-stratum-integration-tests
    tests/stratum/loopback_test.cpp src/stratum/client.cpp src/stratum/job.cpp
    src/cpu/topology.cpp src/randomx/cache.cpp src/randomx/dataset.cpp src/core/mining.cpp
    src/stratum/transport.cpp)
  target_compile_features(unique-rx-stratum-integration-tests PRIVATE cxx_std_20)
  target_include_directories(unique-rx-stratum-integration-tests PRIVATE src
    "${randomx_SOURCE_DIR}/src")
  target_link_libraries(unique-rx-stratum-integration-tests PRIVATE randomx
    Threads::Threads OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(unique-rx-stratum-integration-tests PRIVATE ws2_32)
  endif()
  add_test(NAME unique-rx-stratum-loopback COMMAND unique-rx-stratum-integration-tests)

  if(TARGET randomx-tests)
    add_test(NAME randomx-upstream-consensus COMMAND randomx-tests)
  endif()
endif()