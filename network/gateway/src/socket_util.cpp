#include <algorithm>
#include <chrono>

#ifndef _WIN32
#include <cerrno>
#include <netinet/tcp.h>
#include <poll.h>
#endif

#include "gateway/socket_util.hpp"

namespace gateway
{
    namespace
    {
        /** How long a write may wait for the peer to make room: a browser that is alive reads much sooner than this. */
        constexpr std::chrono::seconds WRITE_TIMEOUT{10};
    }

    bool waitForData(const sockpp::socket &socket, int timeout_ms)
    {
        if (!socket.is_open())
            return true; // the read that follows will fail, which is what the caller wants to find out

        const auto handle = socket.handle();

#ifdef _WIN32
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(handle, &readable);

        timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        return ::select(0, &readable, nullptr, nullptr, &timeout) != 0; // (the first argument is ignored on Windows)
#else
        pollfd descriptor;
        descriptor.fd = handle;
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        const int result = ::poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno == EINTR)
            return false;
        return result != 0;
#endif
    }

    bool sendAll(const sockpp::socket &socket, const char *data, size_t size)
    {
        const auto handle = socket.handle();

#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0; // (where there is no such flag, tune() asks the socket not to raise it)
#endif

        size_t sent = 0;

        while (sent < size)
        {
#ifdef _WIN32
            const int n = ::send(handle, data + sent, static_cast<int>(std::min<size_t>(size - sent, 1 << 20)), flags);
#else
            const ssize_t n = ::send(handle, data + sent, size - sent, flags);
            if (n < 0 && errno == EINTR)
                continue;
#endif
            if (n <= 0)
                return false;

            sent += static_cast<size_t>(n);
        }

        return true;
    }

    void tune(sockpp::stream_socket &socket)
    {
        socket.set_option(IPPROTO_TCP, TCP_NODELAY, 1);
        socket.write_timeout(WRITE_TIMEOUT);

#ifdef SO_NOSIGPIPE
        socket.set_option(SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
    }
}
