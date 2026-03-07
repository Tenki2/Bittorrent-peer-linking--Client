cmake_minimum_required(VERSION 3.10)

# Updated project name
project(BitTorrentClient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)

# Tell CMake the executable is 'btclient' and the source is 'main.cpp'
add_executable(btclient main.cpp)

# Link the dynamically found libraries to the new 'btclient' target
target_include_directories(btclient PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
target_link_directories(btclient PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
target_link_libraries(btclient PRIVATE ${LIBTORRENT_LIBRARIES})