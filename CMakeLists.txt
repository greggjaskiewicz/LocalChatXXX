include(CMakePrintHelpers)
cmake_minimum_required(VERSION 3.5)
project(LocalChat)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(BOOST_ROOT /opt/homebrew/Cellar/boost)

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O0") # Default is debug TODO: Utilize Release and Debug dirs
cmake_print_variables(Boost_INCLUDE_DIRS)
find_package(Boost)
find_package(Threads REQUIRED)
find_package(wxWidgets REQUIRED)
include_directories(${CMAKE_SOURCE_DIR}/include)

if(NOT TARGET spdlog)
    find_package(spdlog REQUIRED)
endif()

# Belt-and-suspenders: make spdlog's headers reachable for every target, not
# just those that link spdlog::spdlog. On some machines (mixed Homebrew
# prefixes, etc.) the include dir doesn't reliably propagate through the
# imported target, which makes sub-libraries fail with "spdlog/spdlog.h not
# found". Pulling the dir out of the target and adding it globally fixes that.
if(TARGET spdlog::spdlog)
    get_target_property(SPDLOG_INC spdlog::spdlog INTERFACE_INCLUDE_DIRECTORIES)
    if(SPDLOG_INC)
        include_directories(SYSTEM ${SPDLOG_INC})
    endif()
endif()

add_subdirectory(tests)

add_library(linked_list STATIC ${CMAKE_SOURCE_DIR}/src/linked_list.c ${CMAKE_SOURCE_DIR}/include/linked_list.h)

include_directories(${CMAKE_SOURCE_DIR}/include)
add_executable(LocalChat ${CMAKE_SOURCE_DIR}/src/main.cpp
						 ${CMAKE_SOURCE_DIR}/src/BVApp_ConsoleClient.cpp
						 ${CMAKE_SOURCE_DIR}/src/BVBroker.cpp
						 ${CMAKE_SOURCE_DIR}/src/BVTCPConnectionManager.cpp
						 src/BVTCPSession.cpp
)

if(Boost_FOUND)
	include_directories(${Boost_INCLUDE_DIRS})
	target_link_libraries(LocalChat ${Boost_LIBRARIES})
endif()

enable_testing()

if(LINUX)
	add_library(BVService_Avahi STATIC ${CMAKE_SOURCE_DIR}/src/BVService_Avahi.cpp)
	add_library(BVDiscovery_Avahi STATIC ${CMAKE_SOURCE_DIR}/src/BVDiscovery_Avahi.cpp)
	add_library(avahi_api STATIC ${CMAKE_SOURCE_DIR}/src/avahi_api.c)
	add_library(avahi_api_bridge STATIC ${CMAKE_SOURCE_DIR}/src/avahi_api_bridge.cpp)
	# These include BVLoggable.hpp -> spdlog, so they need spdlog's include dirs.
	target_link_libraries(BVService_Avahi spdlog::spdlog)
	target_link_libraries(BVDiscovery_Avahi spdlog::spdlog)
	target_link_libraries(avahi_api_bridge spdlog::spdlog)
	target_link_libraries(LocalChat BVService_Avahi)
	target_link_libraries(LocalChat BVDiscovery_Avahi)
	target_link_libraries(LocalChat avahi_api)
	target_link_libraries(LocalChat avahi_api_bridge)
	target_link_libraries(LocalChat -lavahi-client)
	target_link_libraries(LocalChat -lavahi-common)
endif()

# Why are these static libraries?
# It's an okay solution for multiplatform development
# Do not add or link these libraries when on UNIX

target_compile_definitions(
  LocalChat 
  PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

if (APPLE)
	add_library(BVService_Bonjour STATIC ${CMAKE_SOURCE_DIR}/src/BVService_Bonjour.cpp)
	add_library(BVDiscovery_Bonjour STATIC ${CMAKE_SOURCE_DIR}/src/BVDiscovery_Bonjour.cpp)
	add_library(bonjour_api STATIC ${CMAKE_SOURCE_DIR}/src/bonjour_api.c)
	add_library(bonjour_api_bridge STATIC ${CMAKE_SOURCE_DIR}/src/bonjour_api_bridge.cpp)
	# These include BVLoggable.hpp -> spdlog, so they need spdlog's include dirs.
	target_link_libraries(BVService_Bonjour spdlog::spdlog)
	target_link_libraries(BVDiscovery_Bonjour spdlog::spdlog)
	target_link_libraries(bonjour_api_bridge spdlog::spdlog)
	target_link_libraries(LocalChat BVService_Bonjour)
	target_link_libraries(LocalChat BVDiscovery_Bonjour)
	target_link_libraries(LocalChat bonjour_api)
	target_link_libraries(LocalChat bonjour_api_bridge)
	
endif()
target_link_libraries(LocalChat linked_list)
target_link_libraries(LocalChat wxWidgets::wxWidgets)
target_link_libraries(LocalChat spdlog::spdlog)
#very bad solution to hard failing in BVTCPConnectionManager constructor in test environment
target_compile_definitions(LocalChat PRIVATE RESOLUTION_POLICY_HARD_FAIL)


# We can compile different targets - only BVService_Bonjour, BVService (for BonVoyage main app and for LocalChat) or we can export these classes
# onto a separate git repository. Then, we could use it as a submodule for LC and BV.

# We also should build DEBUG and PROD targets.
# DEBUG target has always memory sanitizer attached to the process, for memory leak checks etc.
