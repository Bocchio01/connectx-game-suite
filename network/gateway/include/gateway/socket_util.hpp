#pragma once

#include <cstddef>

#include <sockpp/stream_socket.h>

namespace gateway
{
    /**
     * Waits until there is something to read on the socket (or somebody hung up), for up to `timeout_ms`.
     * Readers wait in short slices instead of blocking in read(), so that they can notice when another thread
     * wants them to stop: closing a socket under the feet of a reader is not safe, nor does it wake it up
     * on every platform.
     *
     * @return true if a read will not block (it may report an error or the end of the stream)
     */
    bool waitForData(const sockpp::socket &socket, int timeout_ms);

    /**
     * Writes all the data. Unlike a plain write, it does not raise SIGPIPE when the other side has already hung up
     * (that would kill the whole process): it just fails.
     *
     * @return false if the data could not be sent: the connection is gone, or the peer does not read what it is sent
     */
    bool sendAll(const sockpp::socket &socket, const char *data, size_t size);

    /**
     * The settings every connection of the gateway gets: small messages go out at once instead of waiting to be
     * grouped with others, and writing to a peer that does not read gives up after a while instead of blocking
     * forever (a thread stuck there would never be seen again).
     */
    void tune(sockpp::stream_socket &socket);
}
