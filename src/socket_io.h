#ifndef NEUTRINO_SOCKET_IO_H
#define NEUTRINO_SOCKET_IO_H

#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
typedef int Count;
#define BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
typedef ssize_t Count;
#define BAD_SOCKET (-1)
#endif

static inline void close_socket(Socket s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static inline bool nonblocking(Socket s)
{
#ifdef _WIN32
    u_long value = 1;
    return ioctlsocket(s, FIONBIO, &value) == 0;
#else
    return fcntl(s, F_SETFL, O_NONBLOCK) == 0;
#endif
}

static inline bool would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

#endif
