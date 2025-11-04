#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "mp_conn.h"
#include "ep_engine/epoll_event_engine.h"
#include "msg_pass/mp_buf_sizes.h"
#include "utils/my_err.h"

static void handle_read_event(Epolled_fd* epdfd, void* arg, Epdfd_state fd_state);
static void handle_write_event(Epolled_fd* epdfd, void* arg, Epdfd_state fd_state);

int mp_conn_init(Mp_conn* mpconn, void* data, int connfd, EP_engine* engine,
        on_read_msg_done_cb on_read_msg_done, on_write_msg_done_cb on_write_msg_done) {
    assert(on_read_msg_done != NULL && on_write_msg_done != NULL);

    Epolled_fd* epdfd = epdfd_create(connfd, engine);
    if (epdfd == NULL) {
        err_msg("[mp_conn.c: mp_conn_init()] epdfd_create() error.");
        return -1;
    }
    mpconn->epdfd = epdfd;
    mpconn->fd = connfd;
    mpconn->engine = engine;
    mpconn->data = data;

    memset(mpconn->read_buf.buf, 0, sizeof(mpconn->read_buf.buf));
    mpconn->read_buf.first = mpconn->read_buf.buf;
    mpconn->read_buf.last = mpconn->read_buf.first;
    mpconn->read_buf.size = 0;

    memset(mpconn->incoming_msg.buf, 0, sizeof(mpconn->incoming_msg.buf));
    mpconn->incoming_msg.first = mpconn->incoming_msg.buf;
    mpconn->incoming_msg.last = mpconn->incoming_msg.first;
    mpconn->incoming_msg.size = 0;
    
    TAILQ_INIT(&mpconn->outgoing_msgs.curr_list);
    TAILQ_INIT(&mpconn->outgoing_msgs.pending_list);

    mpconn->wr_state = MPCONN_WR_STATE_IDLE;

    pthread_mutex_init(&mpconn->lock, NULL);

    mpconn->on_read_msg_done = on_read_msg_done;
    mpconn->on_write_msg_done = on_write_msg_done;

    mpconn->on_deinit = NULL;
    mpconn->on_deinit_arg = NULL;

    mpconn->destroying = false;
    mpconn->rd_closed = false;
    mpconn->wr_closed = false;

    return 0;
}

static void deinit_outgoing_msg_list(Mp_conn* mpconn) {
    TAILQ_CONCAT(&mpconn->outgoing_msgs.curr_list, 
                        &mpconn->outgoing_msgs.pending_list, msg_link);
    Msg* msg;
    while ((msg = TAILQ_FIRST(&mpconn->outgoing_msgs.curr_list)) != NULL) {
        TAILQ_REMOVE(&mpconn->outgoing_msgs.curr_list, msg, msg_link);
    }

    TAILQ_INIT(&mpconn->outgoing_msgs.curr_list);
}

static void on_epdfd_destroy(void* arg) {
    Mp_conn* mpconn = (Mp_conn*)arg;
    // deinit mpconn
    deinit_outgoing_msg_list(mpconn);

    if (mpconn->on_deinit != NULL) {
        mpconn->on_deinit(mpconn, mpconn->on_deinit_arg);
    }
    pthread_mutex_destroy(&mpconn->lock);
}

static void deinit_mpconn(Mp_conn* mpconn) {
    epdfd_destroy(mpconn->epdfd, on_epdfd_destroy, mpconn);
}

static void close_read(Mp_conn* mpconn) {
    mpconn->rd_closed = true;

    if (!mpconn->destroying) {
        epdfd_shutdown(mpconn->epdfd, true, false);
    }

    if (mpconn->wr_closed && mpconn->destroying) {
        deinit_mpconn(mpconn);
    }
}

static void close_read_locked(Mp_conn* mpconn) {
    pthread_mutex_lock(&mpconn->lock);
    close_read(mpconn);
    pthread_mutex_unlock(&mpconn->lock);
}

static void flush_outgoing_msgs(Mp_conn* mpconn, mpconn_err_t err) {
    TAILQ_CONCAT(&mpconn->outgoing_msgs.curr_list, 
                            &mpconn->outgoing_msgs.pending_list, msg_link);
    Msg* msg;
    while (!TAILQ_EMPTY(&mpconn->outgoing_msgs.curr_list)) {
        msg = TAILQ_FIRST(&mpconn->outgoing_msgs.curr_list);
        TAILQ_REMOVE(&mpconn->outgoing_msgs.curr_list, msg, msg_link);

        mpconn->on_write_msg_done(mpconn, msg, err);
    }
}
/*
    This requires that the lock be held
*/
static void close_write(Mp_conn* mpconn, mpconn_err_t err) {
    mpconn->wr_closed = true;
    mpconn->wr_state = MPCONN_WR_STATE_CLOSED;

    if (!mpconn->destroying) {
        epdfd_shutdown(mpconn->epdfd, false, true);
    }

    pthread_mutex_unlock(&mpconn->lock);
    flush_outgoing_msgs(mpconn, err);

    pthread_mutex_lock(&mpconn->lock);
    if (mpconn->rd_closed && mpconn->destroying) {
        deinit_mpconn(mpconn);
    }
}

void mp_conn_deinit(Mp_conn* mpconn, on_deinit_cb on_done, void* on_done_arg) {
    pthread_mutex_lock(&mpconn->lock);

    mpconn->destroying = true;

    mpconn->on_deinit = on_done;
    mpconn->on_deinit_arg = on_done_arg;

    if (!mpconn->rd_closed) {
        /*
            This will make the read() in the read event handler
            return error or the engine invoke the handler callback
            with RD_CLOSED state (because mpconn is always waiting on 
            read event with the notify_on func at the end of the handler)
            and then the handler will finish this closing process
            (in error handling or RD_CLOSED handling)
        */
        epdfd_shutdown(mpconn->epdfd, true, false);
    }

    if (!mpconn->wr_closed) {
        epdfd_shutdown(mpconn->epdfd, false, true);
        /*
            MPCONN_WR_STATE_IDLE means the mpconn is not waiting on
            write event thus, finish this closing process here
        */
        if (mpconn->wr_state == MPCONN_WR_STATE_IDLE) {
            close_write(mpconn, MPCONN_WR_CLOSED);
        }
    }

    if (mpconn->rd_closed && mpconn->wr_closed) {
        deinit_mpconn(mpconn);
    }

    pthread_mutex_unlock(&mpconn->lock);
}

void mp_conn_start_reading_msgs(Mp_conn* mpconn) {
    epdfd_notify_on_readable(mpconn->epdfd, handle_read_event, mpconn);
}

static ssize_t read_char(Mp_conn* mpconn, char* cp) {
    ssize_t n;
    if (mpconn->read_buf.size <= 0) {
        do {
            n = read(mpconn->fd, mpconn->read_buf.buf, sizeof(mpconn->read_buf.buf));
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 0;
        }

        mpconn->read_buf.first = mpconn->read_buf.buf;
        mpconn->read_buf.last += n;
        mpconn->read_buf.size += n;
    }
    mpconn->read_buf.size--;
    *cp = *(mpconn->read_buf.first++);

    return 1;
}

static mpconn_err_t mp_conn_read_msg(Mp_conn* mpconn, 
                                                struct Mp_conn_incoming_msg* msg) {
    ssize_t rc;

    char c;
    while (msg->size < MP_MAXMSGLEN - 1) {
        if ((rc = read_char(mpconn, &c)) == 1) {
            *(msg->last++) = c;
            msg->size++;
            if (c == '\n') {
                break;
            }
        } else if (rc == 0) {
            *(msg->last) = 0; //null terminate
            return MPCONN_IO_EOF;
        } else {
            return MPCONN_IO_ERR;
        }
    }

    if (msg->size == MP_MAXMSGLEN - 1 && c != '\n') {
        err_msg("[mp_conn.c: mp_conn_read_msg()] The incoming msg exceeds buffer.");
        return MPCONN_IO_ERR;
    }   

    *(msg->last) = 0; //null terminate

    return MPCONN_ERR_NONE;
}

static void mp_conn_notify_on_readable(Mp_conn* mpconn, on_event_cb on_readable_cb,
                                                                        void* arg) {

    epdfd_notify_on_readable(mpconn->epdfd, on_readable_cb, arg);
}



/*
    read event handler
*/
static void handle_read_event(Epolled_fd* epdfd, void* arg, Epdfd_state fd_state) {
    Mp_conn* mpconn = (Mp_conn*)arg;
    
    switch (fd_state) {
        case EPDFD_RD_SHUTDOWN: {
        #ifdef DEBUG_MP
            printf("[mp_conn.c: handle_read_event()] Running read event handler(EPDFD_RD_SHUTDOWN)\n");
        #endif
            mpconn->on_read_msg_done(mpconn, NULL, MPCONN_RD_CLOSED);

            close_read_locked(mpconn);
            break;
        }
        case EPDFD_READABLE: {
        #ifdef DEBUG_MP
            printf("[mp_conn.c: handle_read_event()] Running read event handler(EPDFD_READABLE)\n");
        #endif
            struct Mp_conn_incoming_msg* msg = &mpconn->incoming_msg;
            mpconn_err_t err;
            while ((err = mp_conn_read_msg(mpconn, msg)) == MPCONN_ERR_NONE) {
                // handle msg
                mpconn->on_read_msg_done(mpconn, msg->buf, err);
                // msg reset
                memset(msg, 0, sizeof(msg->buf));
                msg->first = msg->last = msg->buf;
                msg->size = 0;
            }
            if (err == MPCONN_IO_ERR && errno == EAGAIN) {
                // notify_on_read
                mp_conn_notify_on_readable(mpconn, handle_read_event, mpconn);
                return;
            } else { // MPCONN_IO_EOF or MPCONN_IO_ERR
                mpconn->on_read_msg_done(mpconn, NULL, err);

                close_read_locked(mpconn);
                return;
            }

            break;
        }
        default: {
            err_exit("[mp_conn.c: handle_read_event()] Invalid fd_state(%d)", fd_state);
        }
    }

    return;
}

static mpconn_err_t mp_conn_write_msg(Mp_conn* mpconn, Msg* msg) {
    int nleft = msg->size;
    ssize_t nwritten = 0;

    char* bufp = msg->first;
#ifdef DEBUG_MP
    printf("[mp_conn.c: mp_conn_write_msg()] Sending: %s\n", bufp);
#endif
    while (nleft > 0) {
        if ((nwritten = write(mpconn->fd, bufp, nleft)) <= 0) {
            if (nwritten < 0 && errno == EINTR) {
                nwritten = 0;
            } else {
                msg->size = nleft;
                msg->first = bufp;

                return MPCONN_IO_ERR;
            }
        }
        nleft -= nwritten;
        bufp += nwritten;
    }

    return MPCONN_ERR_NONE;
}

static void mp_conn_notify_on_writable(Mp_conn* mpconn, on_event_cb on_writable_cb,
                                                                        void* arg) {
    pthread_mutex_lock(&mpconn->lock);

    if (mpconn->destroying) {
        close_write(mpconn, MPCONN_WR_CLOSED);
        pthread_mutex_unlock(&mpconn->lock);
        return;
    }

    epdfd_notify_on_writable(mpconn->epdfd, on_writable_cb, arg);

    pthread_mutex_unlock(&mpconn->lock);
}



static void write_msgs_end(Mp_conn* mpconn, mpconn_err_t err) {
    pthread_mutex_lock(&mpconn->lock);
    if (err != MPCONN_ERR_NONE) {
        close_write(mpconn, err);

        pthread_mutex_unlock(&mpconn->lock);
        return;
    }

    if (mpconn->destroying) {
        close_write(mpconn, MPCONN_WR_CLOSED);

        pthread_mutex_unlock(&mpconn->lock);
        return;
    }

    // Check pending list
    TAILQ_CONCAT(&mpconn->outgoing_msgs.curr_list, 
                &mpconn->outgoing_msgs.pending_list, msg_link);
    if (TAILQ_EMPTY(&mpconn->outgoing_msgs.curr_list)) {
        mpconn->wr_state = MPCONN_WR_STATE_IDLE;
    }
    pthread_mutex_unlock(&mpconn->lock);
}
/*
    write event handler
*/
static void handle_write_event(Epolled_fd* epdfd, void* arg, Epdfd_state fd_state) {
    Mp_conn* mpconn = (Mp_conn*)arg;
    Msg* msg;

    switch (fd_state) {
        case EPDFD_WR_SHUTDOWN: {
        #ifdef DEBUG_MP
            printf("[mp_conn.c: handle_write_event()] Running write event handler(EPDFD_WR_SHUTDOWN)\n");
        #endif
            write_msgs_end(mpconn, MPCONN_WR_CLOSED);
            break;
        }
        case EPDFD_WRITABLE: {
        #ifdef DEBUG_MP
            printf("[mp_conn.c: handle_write_event()] Running write event handler(EPDFD_WRITABLE)\n");
        #endif
            mpconn_err_t err = MPCONN_ERR_NONE;
            while(mpconn->wr_state == MPCONN_WR_STATE_WRITING) {
                while (!TAILQ_EMPTY(&mpconn->outgoing_msgs.curr_list)) {
                    msg = TAILQ_FIRST(&mpconn->outgoing_msgs.curr_list);
                    // write msg
                    if ((err = mp_conn_write_msg(mpconn, msg)) == MPCONN_ERR_NONE) {
                        TAILQ_REMOVE(&mpconn->outgoing_msgs.curr_list, msg, msg_link);
                        mpconn->on_write_msg_done(mpconn, msg, err);
                        continue;
                    }
                    if (err == MPCONN_IO_ERR && errno == EAGAIN) {
                        mp_conn_notify_on_writable(mpconn, handle_write_event, mpconn);
                        return;
                    } else { // MPCONN_IO_ERR
                        break;
                    }
                }

                write_msgs_end(mpconn, err);
            }
            break;
        }
        default: {
            err_exit("[mp_conn.c: handle_write_event()] Invalid fd_state(%d)", fd_state);
        }
    }

    return;
}

int mp_conn_initiate_write_msg(Mp_conn* mpconn, Msg* msg) {
    assert(msg != NULL);

#ifdef DEBUG_MP
    printf("[mp_conn.c: mp_conn_initiate_write_msg()] Initiate write msg\n");
#endif
    pthread_mutex_lock(&mpconn->lock);
    if (mpconn->wr_closed || mpconn->destroying ||
                    mpconn->wr_state == MPCONN_WR_STATE_CLOSED) {
        pthread_mutex_unlock(&mpconn->lock);

        return -1;
    }

    TAILQ_INSERT_TAIL(&mpconn->outgoing_msgs.pending_list, msg, msg_link);

    if (mpconn->wr_state == MPCONN_WR_STATE_IDLE) {
        // set fd writable
        mpconn->wr_state = MPCONN_WR_STATE_WRITING;
        pthread_mutex_unlock(&mpconn->lock);
        epdfd_set_writable(mpconn->epdfd);
        epdfd_notify_on_writable(mpconn->epdfd, handle_write_event, mpconn);
        return 0;
    }
    pthread_mutex_unlock(&mpconn->lock);

    return 0;    
}

// EP_engine* mp_conn_get_engine(Mp_conn* mpconn) {
//     return mpconn->engine;
// }