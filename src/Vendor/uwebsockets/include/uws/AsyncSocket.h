/*
 * Authored by Alex Hultman, 2018-2020.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_ASYNCSOCKET_H
#define UWS_ASYNCSOCKET_H

/* This class implements async socket memory management strategies */

/* NOTE: Many unsigned/signed conversion warnings could be solved by moving from int length
 * to unsigned length for everything to/from uSockets - this would however remove the opportunity
 * to signal error with -1 (which is how the entire UNIX syscalling is built). */

#include "LoopData.h"
#include "AsyncSocketData.h"

namespace uWS {

    template <bool, bool, typename> struct WebSocketContext;

template <bool SSL>
struct AsyncSocket {
    template <bool> friend struct HttpContext;
    template <bool, bool, typename> friend struct WebSocketContext;
    template <bool, typename> friend struct WebSocketContextData;
    friend struct TopicTree;

protected:
    /* Returns SSL pointer or FD as pointer */
    void *getNativeHandle() {
        return us_socket_get_native_handle(SSL, (us_socket_t *) this);
    }

    /* Get loop data for socket */
    LoopData *getLoopData() {
        return (LoopData *) us_loop_ext(us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t *) this)));
    }

    /* Get socket extension */
    AsyncSocketData<SSL> *getAsyncSocketData() {
        return (AsyncSocketData<SSL> *) us_socket_ext(SSL, (us_socket_t *) this);
    }

    /* Socket timeout */
    void timeout(unsigned int seconds) {
        us_socket_timeout(SSL, (us_socket_t *) this, seconds);
    }

    /* Shutdown socket without any automatic drainage */
    void shutdown() {
        us_socket_shutdown(SSL, (us_socket_t *) this);
    }

    /* Immediately close socket */
    us_socket_t *close() {
        return us_socket_close(SSL, (us_socket_t *) this, 0, nullptr);
    }

    /* Cork this socket. Only one socket may ever be corked per-loop at any given time */
    void cork() {
        /* What if another socket is corked? */
        getLoopData()->corkedSocket = this;
    }

    /* Returns wheter we are corked or not */
    bool isCorked() {
        return getLoopData()->corkedSocket == this;
    }

    /* Returns whether we could cork (it is free) */
    bool canCork() {
        return getLoopData()->corkedSocket == nullptr;
    }

    /* Returns a suitable buffer for temporary assemblation of send data */
    std::pair<char *, bool> getSendBuffer(size_t size) {
        /* If we are corked and we have room, return the cork buffer itself */
        LoopData *loopData = getLoopData();
        if (loopData->corkedSocket == this && loopData->corkOffset + size < LoopData::CORK_BUFFER_SIZE) {
            char *sendBuffer = loopData->corkBuffer + loopData->corkOffset;
            loopData->corkOffset += (unsigned int) size;
            return {sendBuffer, false};
        } else {
            /* Slow path for now, we want to always be corked if possible */
            return {(char *) malloc(size), true};
        }
    }

    /* Returns the user space backpressure. */
    unsigned int getBufferedAmount() {
        return (unsigned int) getAsyncSocketData()->buffer.size();
    }

    /* Returns the text representation of an IPv4 or IPv6 address */
    std::string_view addressAsText(std::string_view binary) {
        static thread_local char buf[64];
        int ipLength = 0;

        if (!binary.length()) {
            return {};
        }

        unsigned char *b = (unsigned char *) binary.data();

        if (binary.length() == 4) {
            ipLength = sprintf(buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        } else {
            ipLength = sprintf(buf, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                b[12], b[13], b[14], b[15]);
        }

        return {buf, (unsigned int) ipLength};
    }

    /* Returns the remote IP address or empty string on failure */
    std::string_view getRemoteAddress() {
        static thread_local char buf[16];
        int ipLength = 16;
        us_socket_remote_address(SSL, (us_socket_t *) this, buf, &ipLength);
        return std::string_view(buf, (unsigned int) ipLength);
    }

    /* Returns the text representation of IP */
    std::string_view getRemoteAddressAsText() {
        return addressAsText(getRemoteAddress());
    }

    /* Write in three levels of prioritization: cork-buffer, syscall, socket-buffer. Always drain if possible.
     * Returns pair of bytes written (anywhere) and wheter or not this call resulted in the polling for
     * writable (or we are in a state that implies polling for writable). */
    std::pair<int, bool> write(const char *src, int length, bool optionally = false, int nextLength = 0) {
        /* Fake success if closed, simple fix to allow uncork of closed socket to succeed */
        if (us_socket_is_closed(SSL, (us_socket_t *) this)) {
            return {length, false};
        }

        LoopData *loopData = getLoopData();
        AsyncSocketData<SSL> *asyncSocketData = getAsyncSocketData();

        /* We are limited if we have a per-socket buffer */
        if (asyncSocketData->buffer.length()) {
            /* Write off as much as we can */
            int written = us_socket_write(SSL, (us_socket_t *) this, asyncSocketData->buffer.data(), (int) asyncSocketData->buffer.length(), /*nextLength != 0 | */length);

            /* On failure return, otherwise continue down the function */
            if ((unsigned int) written < asyncSocketData->buffer.length()) {

                /* Update buffering (todo: we can do better here if we keep track of what happens to this guy later on) */
                asyncSocketData->buffer = asyncSocketData->buffer.substr((size_t) written);

                if (optionally) {
                    /* Thankfully we can exit early here */
                    return {0, true};
                } else {
                    /* This path is horrible and points towards erroneous usage */
                    asyncSocketData->buffer.append(src, (unsigned int) length);

                    return {length, true};
                }
            }

            /* At this point we simply have no buffer and can continue as normal */
            asyncSocketData->buffer.clear();
        }

        if (length) {
            if (loopData->corkedSocket == this) {
                /* We are corked */
                if (LoopData::CORK_BUFFER_SIZE - loopData->corkOffset >= (unsigned int) length) {
                    /* If the entire chunk fits in cork buffer */
                    memcpy(loopData->corkBuffer + loopData->corkOffset, src, (unsigned int) length);
                    loopData->corkOffset += (unsigned int) length;
                    /* Fall through to default return */
                } else {
                    /* Strategy differences between SSL and non-SSL regarding syscall minimizing */
                    if constexpr (SSL) {
                        /* Cork up as much as we can */
                        unsigned int stripped = LoopData::CORK_BUFFER_SIZE - loopData->corkOffset;
                        memcpy(loopData->corkBuffer + loopData->corkOffset, src, stripped);
                        loopData->corkOffset = LoopData::CORK_BUFFER_SIZE;

                        auto [written, failed] = uncork(src + stripped, length - (int) stripped, optionally);
                        return {written + (int) stripped, failed};
                    }

                    /* For non-SSL we take the penalty of two syscalls */
                    return uncork(src, length, optionally);
                }
            } else {
                /* We are not corked */
                int written = us_socket_write(SSL, (us_socket_t *) this, src, length, nextLength != 0);

                /* Did we fail? */
                if (written < length) {
                    /* If the write was optional then just bail out */
                    if (optionally) {
                        return {written, true};
                    }

                    /* Fall back to worst possible case (should be very rare for HTTP) */
                    /* At least we can reserve room for next chunk if we know it up front */
                    if (nextLength) {
                        asyncSocketData->buffer.reserve(asyncSocketData->buffer.length() + (size_t) (length - written + nextLength));
                    }

                    /* Buffer this chunk */
                    asyncSocketData->buffer.append(src + written, (size_t) (length - written));

                    /* Return the failure */
                    return {length, true};
                }
                /* Fall through to default return */
            }
        }

        /* Default fall through return */
        return {length, false};
    }

    /* Uncork this socket and flush or buffer any corked and/or passed data. It is essential to remember doing this. */
    /* It does NOT count bytes written from cork buffer (they are already accounted for in the write call responsible for its corking)! */
    std::pair<int, bool> uncork(const char *src = nullptr, int length = 0, bool optionally = false) {
        LoopData *loopData = getLoopData();

        if (loopData->corkedSocket == this) {
            loopData->corkedSocket = nullptr;

            if (loopData->corkOffset) {
                /* Corked data is already accounted for via its write call */
                auto [written, failed] = write(loopData->corkBuffer, (int) loopData->corkOffset, false, length);
                loopData->corkOffset = 0;

                if (failed) {
                    /* We do not need to care for buffering here, write does that */
                    return {0, true};
                }
            }

            /* We should only return with new writes, not things written to cork already */
            return write(src, length, optionally, 0);
        } else {
            /* We are not even corked! */
            return {0, false};
        }
    }
};

}

#endif // UWS_ASYNCSOCKET_H
