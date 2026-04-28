cmake_minimum_required(VERSION 3.10)

project(btclient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)

add_executable(
    btclient
    main.cpp
    logging.cpp
    peer_stats.cpp
    summary_writer.cpp
    util.cpp)

target_include_directories(btclient PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
target_link_directories(btclient PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
target_link_libraries(btclient PRIVATE ${LIBTORRENT_LIBRARIES})
