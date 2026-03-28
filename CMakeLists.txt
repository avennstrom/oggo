cmake_minimum_required(VERSION 3.20)
project(castogg)

add_subdirectory(external/ogg)
add_subdirectory(external/vorbis)

add_executable(client client.c socket.c)
add_executable(relay relay.c llhttp/api.c llhttp/http.c llhttp/llhttp.c)

target_include_directories(client PRIVATE external/ogg/include external/vorbis/include)
target_include_directories(relay PRIVATE external/ogg/include external/vorbis/include)

target_link_libraries(client PRIVATE ogg vorbis)
target_link_libraries(relay PRIVATE ogg vorbis)

if (WIN32)
    target_link_libraries(client PRIVATE ws2_32)
    target_link_libraries(relay PRIVATE ws2_32)
endif()