cmake_minimum_required(VERSION 3.20)
project(castogg)

find_package(Vorbis REQUIRED)

add_executable(client client.c socket.c)
add_executable(relay relay.c llhttp/api.c llhttp/http.c llhttp/llhttp.c)

target_link_libraries(client PRIVATE Vorbis::vorbis)
target_link_libraries(relay PRIVATE Vorbis::vorbis)

if (WIN32)
    target_link_libraries(client PRIVATE ws2_32)
    target_link_libraries(relay PRIVATE ws2_32)
endif()