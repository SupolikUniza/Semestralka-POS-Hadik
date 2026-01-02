cmake_minimum_required(VERSION 3.10)
project(snake_ipc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Curses REQUIRED)

include_directories(include)

add_executable(server
  src/common.c
  src/server.c
  src/server_main.c
)

add_executable(client
  src/common.c
  src/client.c
  src/client_main.c
)

target_link_libraries(server pthread)
target_link_libraries(client pthread ${CURSES_LIBRARIES})
target_include_directories(client PRIVATE ${CURSES_INCLUDE_DIR})
