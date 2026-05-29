cmake_minimum_required(VERSION 3.14)
project(my_project)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BOOST_ROOT /opt/homebrew/Cellar/boost)
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost)
find_package(Threads REQUIRED)

if(NOT TARGET spdlog)
    find_package(spdlog REQUIRED)
endif()

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wall -Wextra  -g -O0")

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59 # v1.15.2
)
FetchContent_MakeAvailable(googletest)

# enable_testing()
# add_library(linked_list STATIC ${CMAKE_SOURCE_DIR}/src/linked_list.c ${CMAKE_SOURCE_DIR}/include/linked_list.h)

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/tests)

if(Boost_FOUND)
	include_directories(${Boost_INCLUDE_DIRS})
endif()

add_executable(
  dns_sd_test_Registration
  dns_sd_test_Registration.cpp
)

target_link_libraries(
  dns_sd_test_Registration
  GTest::gtest_main
)

add_executable(
  file_transfer_test_Integrity
  file_transfer_test_Integrity.cpp
)

target_link_libraries(
  file_transfer_test_Integrity
  GTest::gtest_main
  spdlog::spdlog
)

add_executable(
  linked_list_test_Basic
  linked_list_test_Basic.cpp
)

target_link_libraries(
  linked_list_test_Basic
  GTest::gtest_main
  linked_list
)

add_executable(
  broker_test_Basic
  broker_test_Basic.cpp
  component_mocks.cpp
  ${CMAKE_SOURCE_DIR}/src/BVBroker.cpp
  ${CMAKE_SOURCE_DIR}/src/BVTCPConnectionManager.cpp
  ${CMAKE_SOURCE_DIR}/src/BVTCPSession.cpp
)

target_link_libraries(
  broker_test_Basic
  GTest::gtest_main
  linked_list
	${Boost_LIBRARIES}
  spdlog::spdlog
)

add_executable(
  component_test_Discovery_Basic
  component_test_Discovery_Basic.cpp
  component_mocks.cpp
  ${CMAKE_SOURCE_DIR}/src/BVTCPConnectionManager.cpp
  ${CMAKE_SOURCE_DIR}/src/BVTCPSession.cpp
)

target_link_libraries(
  component_test_Discovery_Basic
  GTest::gtest_main
  linked_list
	${Boost_LIBRARIES}
  spdlog::spdlog
)

add_executable(
  communication_test_Multiple_Components
  communication_test_Multiple_Components.cpp
  component_mocks.cpp
  ${CMAKE_SOURCE_DIR}/src/BVBroker.cpp
  ${CMAKE_SOURCE_DIR}/src/BVTCPConnectionManager.cpp
  ${CMAKE_SOURCE_DIR}/src/BVTCPSession.cpp
)

target_link_libraries(
  communication_test_Multiple_Components
  GTest::gtest_main
  linked_list
	${Boost_LIBRARIES}
  spdlog::spdlog
)

target_compile_definitions(
  communication_test_Multiple_Components 
  PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

# cmake -S . -B build -DUSE_ASAN=ON -> enable ASan

option(USE_ASAN "Enable AddressSanitizer" OFF)

if(USE_ASAN)
    set(SANITIZE_FLAGS "-fsanitize=address" "-fno-omit-frame-pointer" "-g" "-O1")
    target_compile_options(linked_list PRIVATE ${SANITIZE_FLAGS})
    target_link_options(linked_list PRIVATE ${SANITIZE_FLAGS})
    target_compile_options(linked_list_test_Basic PRIVATE ${SANITIZE_FLAGS})
    target_link_options(linked_list_test_Basic PRIVATE ${SANITIZE_FLAGS})
endif()

include(GoogleTest)
gtest_discover_tests(dns_sd_test_Registration)
gtest_discover_tests(linked_list_test_Basic)
gtest_discover_tests(file_transfer_test_Integrity)

# TODO: Run all tests, either link every object to a single executable and run
#       or write script that executes all tests