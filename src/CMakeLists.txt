cmake_minimum_required(VERSION 3.10)

project(btclient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json QUIET)
pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)

if(NOT nlohmann_json_FOUND)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
endif()

add_executable(
    btclient
    main.cpp
    artifact_lifecycle.cpp
    artifact_sender.cpp
    logging.cpp
    peer_stats.cpp
    summary_writer.cpp
    util.cpp)

target_include_directories(btclient PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
if(NOT nlohmann_json_FOUND)
    target_include_directories(btclient PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
if(NOT TARGET CURL::libcurl)
    target_include_directories(btclient PRIVATE ${CURL_INCLUDE_DIRS})
endif()
target_link_directories(btclient PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
target_link_libraries(btclient PRIVATE ${LIBTORRENT_LIBRARIES})
if(TARGET CURL::libcurl)
    target_link_libraries(btclient PRIVATE CURL::libcurl)
else()
    target_link_libraries(btclient PRIVATE ${CURL_LIBRARIES})
endif()
if(nlohmann_json_FOUND)
    target_link_libraries(btclient PRIVATE nlohmann_json::nlohmann_json)
endif()
