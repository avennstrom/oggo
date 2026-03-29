
#ifdef __cplusplus
extern "C" {
#endif

#ifndef SW_SOCKET_H
#define SW_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sw_socket_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
typedef int sw_socket_t;
#endif

#define SW_INVALID_SOCKET (-1)

typedef enum {
    SW_OK = 0,
    SW_ERR = -1,
    SW_WOULD_BLOCK = 1,
    SW_CLOSED = 2
} sw_result;

int  sw_init(void);
void sw_shutdown(void);

/* socket ops */
sw_socket_t sw_socket_create(void);
void      sw_socket_close(sw_socket_t s);

/* configure */
int sw_socket_set_nonblocking(sw_socket_t s, int enabled);
int sw_socket_set_reuseaddr(sw_socket_t s, int enabled);

/* client */
sw_result sw_connect(sw_socket_t s, const char* ip, uint16_t port);

/* server */
sw_result sw_bind(sw_socket_t s, uint16_t port);
sw_result sw_listen(sw_socket_t s, int backlog);
sw_result sw_accept(sw_socket_t s, sw_socket_t* out_client);

/* io */
sw_result sw_send(sw_socket_t s, const void* data, size_t len, size_t* sent);
sw_result sw_recv(sw_socket_t s, void* buffer, size_t len, size_t* received);

void sw_http_date_now(char out[64])
{
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &now);          /* Windows */
#else
    gmtime_r(&now, &tm);          /* POSIX */
#endif
    /* RFC1123 format: Mon, 29 Mar 2026 00:00:00 GMT */
    strftime(out, 64, "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
}

static inline unsigned _ctz64(unsigned long long x)
{
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanForward64(&idx, x);
    return (unsigned)idx;
#else
    return __builtin_ctzll(x);
#endif
}

#ifdef __cplusplus
}
#endif

#endif

/* ================= Implementation ================= */
#ifdef SW_SOCKET_IMPLEMENTATION

#include <string.h>
#include <signal.h>
#include <assert.h>

#ifdef _WIN32
static int sw__last_error(void) { return WSAGetLastError(); }
#else
#include <errno.h>
static int sw__last_error(void) { return errno; }
#endif

int sw_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2,2), &wsa) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}

void sw_shutdown(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

sw_socket_t sw_socket_create(void) {
    sw_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return SW_INVALID_SOCKET;
#else
    if (s < 0) return SW_INVALID_SOCKET;
#endif
    return s;
}

void sw_socket_close(sw_socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

int sw_socket_set_nonblocking(sw_socket_t s, int enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    if (enabled) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, flags) == 0 ? 0 : -1;
#endif
}

int sw_socket_set_reuseaddr(sw_socket_t s, int enabled) {
    int opt = enabled ? 1 : 0;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == 0 ? 0 : -1;
}

static int sw__would_block(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

sw_result sw_connect(sw_socket_t s, const char* ip, uint16_t port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
    {
        return SW_ERR;
    }

    int r = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (r == -1)
    {
        int err = sw__last_error();
        if (sw__would_block(err))
        {
            return SW_WOULD_BLOCK;
        }

        return SW_ERR;
    }
    
    assert(r == 0);
    return SW_OK;
}

sw_result sw_bind(sw_socket_t s, uint16_t port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    //if (!ip)
    //{
    //    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    //}
    //else if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
    //{
    //    return SW_ERR;
    //}

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        return SW_ERR;
    }

    return SW_OK;
}

sw_result sw_listen(sw_socket_t s, int backlog)
{
    if (listen(s, backlog) != 0)
    {
        return SW_ERR;
    }

    return SW_OK;
}

sw_result sw_accept(sw_socket_t s, sw_socket_t* out_client)
{
    struct sockaddr_in addr;
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif

    sw_socket_t c = accept(s, (struct sockaddr*)&addr, &len);
#ifdef _WIN32
    if (c == INVALID_SOCKET) {
        int err = sw__last_error();
        if (sw__would_block(err)) return SW_WOULD_BLOCK;
        return SW_ERR;
    }
#else
    if (c < 0) {
        int err = sw__last_error();
        if (sw__would_block(err)) return SW_WOULD_BLOCK;
        return SW_ERR;
    }
#endif

    *out_client = c;
    return SW_OK;
}

void hexdump(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i += 16) {
        // print offset
        printf("%08zx  ", i);

        // hex bytes
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len)
                printf("%02x ", p[i + j]);
            else
                printf("   ");  // padding
            if (j == 7) printf(" "); // extra space in middle
        }

        // ASCII representation
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            unsigned char c = p[i + j];
            printf("%c", isprint(c) ? c : '.');
        }
        printf("|\n");
    }
}

sw_result sw_send(sw_socket_t s, const void* data, size_t len, size_t* sent)
{
#ifdef _WIN32
    int r = send(s, (const char*)data, (int)len, 0);
#else
    ssize_t r = send(s, data, len, MSG_NOSIGNAL);
#endif

    if (r == -1)
    {
        int err = sw__last_error();
        if (sw__would_block(err))
        {
            return SW_WOULD_BLOCK;
        }

#ifdef _WIN32
        if (err == WSAECONNRESET || err == WSAECONNABORTED)
        {
            return SW_CLOSED;
        }
#endif

        return SW_ERR;
    }

    assert(r > 0);
    *sent = (size_t)r;
    return SW_OK;
}

sw_result sw_recv(sw_socket_t s, void* buffer, size_t len, size_t* received)
{
#ifdef _WIN32
    int r = recv(s, (char*)buffer, (int)len, 0);
#else
    ssize_t r = recv(s, buffer, len, 0);
#endif

    if (r == 0)
    {
        return SW_CLOSED;
    }

    if (r == -1)
    {
        int err = sw__last_error();
        if (sw__would_block(err))
        {
            return SW_WOULD_BLOCK;
        }

#ifdef _WIN32
        if (err == WSAECONNRESET || err == WSAECONNABORTED)
        {
            return SW_CLOSED;
        }
#endif

        return SW_ERR;
    }

    assert(r > 0);
    *received = (size_t)r;
    return SW_OK;
}

#endif /* SW_SOCKET_IMPLEMENTATION */
