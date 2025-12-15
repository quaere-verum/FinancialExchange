cmake_minimum_required(VERSION 3.20)
project(FinancialExchange LANGUAGES CXX)
add_compile_definitions(_WIN32_WINNT=0x0603 WINVER=0x0603)

# Prevent CMake from searching Anaconda for packages
set(CMAKE_IGNORE_PREFIX_PATH "${CONDA_PATH}")
set(CMAKE_IGNORE_PREFIX_PATH "${CONDA_PATH}/Library")

if(MSVC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17")
endif(MSVC)

# -------------------------------
# vcpkg integration
# -------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "You must pass CMAKE_TOOLCHAIN_FILE to vcpkg toolchain")
endif()

set(Boost_USE_STATIC_LIBS OFF)   # use dynamic libraries
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
set(CMAKE_INCLUDE_PATH ${CMAKE_INCLUDE_PATH} "${VCPKG_PATH}/installed/x64-windows/include")
set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} "${VCPKG_PATH}/installed/x64-windows/lib")

find_package(Boost REQUIRED COMPONENTS
    system
    log
    thread
    filesystem
    date_time
    chrono
    regex
)
file(GLOB SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_executable(${PROJECT_NAME} ${SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Boost::system
    Boost::log
    Boost::thread
    Boost::filesystem
    Boost::date_time
    Boost::chrono
    Boost::regex
    ws2_32
    mswsock
    secur32
)

target_include_directories(FinancialExchange PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)