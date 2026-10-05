cmake_minimum_required(VERSION 3.12)

set(TARGET_NAME ollama_systemone)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include src)

set(EXTENSION_SOURCES src/tev1_extension.cpp src/registration.cpp src/systemone/config.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} CURL::libcurl nlohmann_json::nlohmann_json)
target_link_libraries(${LOADABLE_EXTENSION_NAME} CURL::libcurl nlohmann_json::nlohmann_json)

file(GLOB SYSTEMONE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/test_*.cpp")
file(GLOB SYSTEMONE_MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/systemone/*.cpp")
if(SYSTEMONE_TEST_SOURCES)
  enable_testing()
  foreach(TEST_SOURCE IN LISTS SYSTEMONE_TEST_SOURCES)
    get_filename_component(TEST_FILENAME "${TEST_SOURCE}" NAME_WE)
    string(REGEX REPLACE "^test_" "" MODULE_NAME "${TEST_FILENAME}")
    set(MODULE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/systemone/${MODULE_NAME}.cpp")
    if(NOT EXISTS "${MODULE_SOURCE}")
      message(FATAL_ERROR "Missing pure SystemOne module source for ${TEST_FILENAME}: ${MODULE_SOURCE}")
    endif()

    set(TEST_TARGET "systemone_test_${MODULE_NAME}")
    add_executable(${TEST_TARGET} "${TEST_SOURCE}" "${MODULE_SOURCE}")
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${TEST_TARGET} PRIVATE CURL::libcurl Threads::Threads nlohmann_json::nlohmann_json)
    add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
  endforeach()
endif()

add_executable(systemone_transport_test_driver
  test/cpp/transport_test_driver.cpp
  src/systemone/request.cpp
  src/systemone/response.cpp
  src/systemone/transport.cpp)
target_include_directories(systemone_transport_test_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(systemone_transport_test_driver PRIVATE CURL::libcurl Threads::Threads nlohmann_json::nlohmann_json)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
