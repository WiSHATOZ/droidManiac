/*
 * Authored by Alex Hultman, 2018-2021.
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

#include "libusockets.h"
#include "internal/internal.h"
#include <stdlib.h>

/* The loop has 2 fallthrough polls */
void us_internal_loop_data_init(struct us_loop_t *loop, void (*wakeup_cb)(struct us_loop_t *loop),
    void (*pre_cb)(struct us_loop_t *loop), void (*post_cb)(struct us_loop_t *loop)) {
    loop->data.sweep_timer = us_create_timer(loop, 1, 0);
    loop->data.recv_buf = malloc(LIBUS_RECV_BUFFER_LENGTH + LIBUS_RECV_BUFFER_PADDING * 2);
    loop->data.ssl_data = 0;
    loop->data.head = 0;
    loop->data.iterator = 0;
    loop->data.closed_head = 0;

    loop->data.pre_cb = pre_cb;
    loop->data.post_cb = post_cb;
    loop->data.iteration_nr = 0;

    loop->data.wakeup_async = us_internal_create_async(loop, 1, 0);
    us_internal_async_set(loop->data.wakeup_async, (void (*)(struct us_internal_async *)) wakeup_cb);
}

void us_internal_loop_data_free(struct us_loop_t *loop) {
#ifndef LIBUS_NO_SSL
    us_internal_free_loop_ssl_data(loop);
#endif

    free(loop->data.recv_buf);

    us_timer_close(loop->data.sweep_timer);
    us_internal_async_close(loop->data.wakeup_async);
}

void us_wakeup_loop(struct us_loop_t *loop) {
    us_internal_async_wakeup(loop->data.wakeup_async);
}

void us_internal_loop_link(struct us_loop_t *loop, struct us_socket_context_t *context) {
    /* Insert this context as the head of loop */
    context->next = loop->data.head;
    context->prev = 0;
    if (loop->data.head) {
        loop->data.head->prev = context;
    }
    loop->data.head = context;
}

/* Unlink is called before free */
void us_internal_loop_unlink(struct us_loop_t *loop, struct us_socket_context_t *context) {
    if (loop->data.head == context) {
        loop->data.head = context->next;
        if (loop->data.head) {
            loop->data.head->prev = 0;
        }
    } else {
        context->prev->next = context->next;
        if (context->next) {
            context->next->prev = context->prev;
        }
    }
}

/* This functions should never run recursively */
void us_internal_timer_sweep(struct us_loop_t *loop) {
    struct us_internal_loop_data_t *loop_data = &loop->data;
    /* For all socket contexts in this loop */
    for (loop_data->iterator = loop_data->head; loop_data->iterator; loop_data->iterator = loop_data->iterator->next) {

        struct us_socket_context_t *context = loop_data->iterator;

        /* Update this context's 15-bit timestamp */
        context->timestamp = (context->timestamp + 1) & 0x7fff;

        /* Update our 16-bit full timestamp (the needle in the haystack) */
        unsigned short needle = 0x8000 | context->timestamp;

        /* Begin at head */
        struct us_socket_t *s = context->head;
        while (s) {
            /* Seek until end or timeout found (tightest loop) */
            while (1) {
                /* We only read from 1 random cache line here */
                if (needle == s->timeout) {
                    break;
                }

                /* Did we reach the end without a find? */
                if ((s = s->next) == 0) {
                    goto next_context;
                }
            }

            /* Here we have a timeout to emit (slow path) */
            s->timeout = 0;
            context->iterator = s;

            context->on_socket_timeout(s);

            /* Check for unlink / link (if the event handler did not modify the chain, we step 1) */
            if (s == context->iterator) {
                s = s->next;
            } else {
                /* The iterator was changed by event handler */
                s = context->iterator;
            }
        }
        /* We always store a 0 to context->iterator here since we are no longer iterating this context */
        next_context:
        context->iterator = 0;
    }
}

/* Note: Properly takes the linked list and timeout sweep into account */
void us_internal_free_closed_sockets(struct us_loop_t *loop) {
    /* Free all closed sockets (maybe it is better to reverse order?) */
    if (loop->data.closed_head) {
        for (struct us_socket_t *s = loop->data.closed_head; s; ) {
            struct us_socket_t *next = s->next;
            us_poll_free((struct us_poll_t *) s, loop);
            s = next;
        }
        loop->data.closed_head = 0;
    }
}

void sweep_timer_cb(struct us_internal_callback_t *cb) {
    us_internal_timer_sweep(cb->loop);
}

long long us_loop_iteration_number(struct us_loop_t *loop) {
    return loop->data.iteration_nr;
}

/* These may have somewhat different meaning depending on the underlying event library */
void us_internal_loop_pre(struct us_loop_t *loop) {
    loop->data.iteration_nr++;
    loop->data.pre_cb(loop);
}

void us_internal_loop_post(struct us_loop_t *loop) {
    us_internal_free_closed_sockets(loop);
    loop->data.post_cb(loop);
}

void us_internal_dispatch_ready_poll(struct us_poll_t *p, int error, int events) {
    switch (us_internal_poll_type(p)) {
    case POLL_TYPE_CALLBACK: {
            /* Let's just do this to clear the CodeQL alert */
        #ifndef LIBUS_USE_LIBUV
            us_internal_accept_poll_event(p);
        #endif
            struct us_internal_callback_t *cb = (struct us_internal_callback_t *) p;
            cb->cb(cb->cb_expects_the_loop ? (struct us_internal_callback_t *) cb->loop : (struct us_internal_callback_t *) &cb->p);
        }
        break;
    case POLL_TYPE_SEMI_SOCKET: {
            /* Both connect and listen sockets are semi-sockets
             * but they poll for different events */
            if (us_poll_events(p) == LIBUS_SOCKET_WRITABLE) {
                struct us_socket_t *s = (struct us_socket_t *) p;

                /* It is perfectly possible to come here with an error */
                if (error) {
                    /* Emit error, close without emitting on_close */
                    s->context->on_connect_error(s, 0);
                    us_socket_close_connecting(0, s);
                } else {
                    /* All sockets poll for readable */
                    us_poll_change(p, s->context->loop, LIBUS_SOCKET_READABLE);

                    /* We always use nodelay */
                    bsd_socket_nodelay(us_poll_fd(p), 1);

                    /* We are now a proper socket */
                    us_internal_poll_set_type(p, POLL_TYPE_SOCKET);

                    /* If we used a connection timeout we have to reset it here */
                    us_socket_timeout(0, s, 0);

                    s->context->on_open(s, 1, 0, 0);
                }
            } else {
                struct us_listen_socket_t *listen_socket = (struct us_listen_socket_t *) p;
                struct bsd_addr_t addr;

                LIBUS_SOCKET_DESCRIPTOR client_fd = bsd_accept_socket(us_poll_fd(p), &addr);
                if (client_fd == LIBUS_SOCKET_ERROR) {
                    /* Todo: start timer here */

                } else {

                    /* Todo: stop timer if any */

                    do {
                        struct us_poll_t *accepted_p = us_create_poll(us_socket_context(0, &listen_socket->s)->loop, 0, sizeof(struct us_socket_t) - sizeof(struct us_poll_t) + listen_socket->socket_ext_size);
                        us_poll_init(accepted_p, client_fd, POLL_TYPE_SOCKET);
                        us_poll_start(accepted_p, listen_socket->s.context->loop, LIBUS_SOCKET_READABLE);

                        struct us_socket_t *s = (struct us_socket_t *) accepted_p;

                        s->context = listen_socket->s.context;

                        /* We always use nodelay */
                        bsd_socket_nodelay(client_fd, 1);

                        us_internal_socket_context_link(listen_socket->s.context, s);

                        listen_socket->s.context->on_open(s, 0, bsd_addr_get_ip(&addr), bsd_addr_get_ip_length(&addr));

                        /* Exit accept loop if listen socket was closed in on_open handler */
                        if (us_socket_is_closed(0, &listen_socket->s)) {
                            break;
                        }

                    } while ((client_fd = bsd_accept_socket(us_poll_fd(p), &addr)) != LIBUS_SOCKET_ERROR);
                }
            }
        }
        break;
    case POLL_TYPE_SOCKET_SHUT_DOWN:
    case POLL_TYPE_SOCKET: {
            /* We should only use s, no p after this point */
            struct us_socket_t *s = (struct us_socket_t *) p;

            /* Such as epollerr epollhup */
            if (error) {
                /* Todo: decide what code we give here */
                s = us_socket_close(0, s, 0, NULL);
                return;
            }

            if (events & LIBUS_SOCKET_WRITABLE) {
                /* Note: if we failed a write as a socket of one loop then adopted
                 * to another loop, this will be wrong. Absurd case though */
                s->context->loop->data.last_write_failed = 0;

                s = s->context->on_writable(s);

                if (us_socket_is_closed(0, s)) {
                    return;
                }

                /* If we have no failed write or if we shut down, then stop polling for more writable */
                if (!s->context->loop->data.last_write_failed || us_socket_is_shut_down(0, s)) {
                    us_poll_change(&s->p, us_socket_context(0, s)->loop, us_poll_events(&s->p) & LIBUS_SOCKET_READABLE);
                }
            }

            if (events & LIBUS_SOCKET_READABLE) {
                /* Contexts may ignore data and postpone it to next iteration, for balancing purposes such as
                 * when SSL handshakes take too long to finish and we only want a few of them per iteration */
                if (s->context->ignore_data(s)) {
                    break;
                }

                int length = bsd_recv(us_poll_fd(&s->p), s->context->loop->data.recv_buf + LIBUS_RECV_BUFFER_PADDING, LIBUS_RECV_BUFFER_LENGTH, 0);
                if (length > 0) {
                    s = s->context->on_data(s, s->context->loop->data.recv_buf + LIBUS_RECV_BUFFER_PADDING, length);
                } else if (!length) {
                    if (us_socket_is_shut_down(0, s)) {
                        /* We got FIN back after sending it */
                        /* Todo: We should give "CLEAN SHUTDOWN" as reason here */
                        s = us_socket_close(0, s, 0, NULL);
                    } else {
                        /* We got FIN, so stop polling for readable */
                        us_poll_change(&s->p, us_socket_context(0, s)->loop, us_poll_events(&s->p) & LIBUS_SOCKET_WRITABLE);
                        s = s->context->on_end(s);
                    }
                } else if (length == LIBUS_SOCKET_ERROR && !bsd_would_block()) {
                    /* Todo: decide also here what kind of reason we should give */
                    s = us_socket_close(0, s, 0, NULL);
                }
            }
        }
        break;
    }
}

/* Integration only requires the timer to be set up */
void us_loop_integrate(struct us_loop_t *loop) {
    us_timer_set(loop->data.sweep_timer, (void (*)(struct us_timer_t *)) sweep_timer_cb, LIBUS_TIMEOUT_GRANULARITY * 1000, LIBUS_TIMEOUT_GRANULARITY * 1000);
}

void *us_loop_ext(struct us_loop_t *loop) {
    return loop + 1;
}
