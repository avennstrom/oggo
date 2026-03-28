#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
    typedef SOCKET sw_socket;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
    typedef int sw_socket;
#endif

#define SW_INVALID_SOCKET (-1)

    int sw_init(void);
    void sw_cleanup(void);

    sw_socket sw_connect(const char* host, int port);
    int sw_send(sw_socket s, const void* data, size_t len);
    int sw_recv(sw_socket s, void* buffer, size_t len);
    void sw_close(sw_socket s);

#ifdef __cplusplus
}
#endif

