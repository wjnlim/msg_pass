#ifndef MP_CONN_H
#define MP_CONN_H

#include "ep_engine/epoll_event_engine.h"
#include "msg_pass/mp_buf_sizes.h"
#include <pthread.h>
#include <sys/queue.h>


typedef struct Mp_conn Mp_conn;

typedef enum {
    MPCONN_ERR_NONE,
    MPCONN_IO_EOF,
    MPCONN_IO_ERR,
    MPCONN_RD_CLOSED,
    MPCONN_WR_CLOSED
} mpconn_err_t;

typedef struct Msg {
    char* buf;
    char* first;
    char* last;
    int size;

    TAILQ_ENTRY(Msg) msg_link;
} Msg;

typedef TAILQ_HEAD(Msg_q, Msg) Msg_q;

typedef enum {
    MPCONN_WR_STATE_IDLE,
    MPCONN_WR_STATE_WRITING,
    MPCONN_WR_STATE_CLOSED
} mpconn_wr_state;

typedef void (*on_read_msg_done_cb)(Mp_conn* mpconn, const char* msg, mpconn_err_t err);
typedef void (*on_write_msg_done_cb)(Mp_conn* mpconn, Msg* msg, mpconn_err_t err);
typedef void (*on_deinit_cb)(Mp_conn* mpconn, void* arg);

struct Mp_conn {
    Epolled_fd* epdfd;
    int fd;
    EP_engine* engine;

    void* data;

    struct Mp_conn_read_buf {
        char buf[MP_MAXREADCHUNK];
        char* first;
        char* last;
        int size;
    } read_buf;

    struct Mp_conn_incoming_msg {
        char buf[MP_MAXMSGLEN];
        char* first;
        char* last;
        int size;
    } incoming_msg;

    struct Mp_conn_outgoing_msg{
        Msg_q curr_list;
        Msg_q pending_list;
    } outgoing_msgs;

    mpconn_wr_state wr_state;

    pthread_mutex_t lock;

    on_read_msg_done_cb on_read_msg_done;
    on_write_msg_done_cb on_write_msg_done;

    on_deinit_cb on_deinit;
    void* on_deinit_arg;

    bool destroying;
    bool rd_closed;
    bool wr_closed;
};

int mp_conn_init(Mp_conn* mpconn, void* data, int connfd, EP_engine* engine,
        on_read_msg_done_cb on_read_msg_done, on_write_msg_done_cb on_write_msg_done);

void mp_conn_deinit(Mp_conn* mpconn, on_deinit_cb on_done, void* on_done_arg);
void mp_conn_start_reading_msgs(Mp_conn* mpconn);
int mp_conn_initiate_write_msg(Mp_conn* mpconn, Msg* msg);

// EP_engine* mp_conn_get_engine(Mp_conn* mpconn);

#endif
