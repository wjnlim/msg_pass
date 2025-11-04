/*
    Message passing client
*/
#ifndef MSG_PASSING_CLIENT_H
#define MSG_PASSING_CLIENT_H

#include "ep_engine/epoll_event_engine.h"

typedef struct Mp_client Mp_client;

typedef enum {
    REQ_ERR_NONE = 0,
    REQ_SEND_ERR = -1,
    REQ_SEND_CLOSED = -2,
    REQ_RECV_RESP_ERR = -3,
    REQ_RECV_RESP_CLOSED = -4
} Req_status;

typedef struct Req_completion {
    Req_status status;
    const char* request_msg;
    const char* respose_msg;
    Mp_client* client;
} Req_completion;

typedef void (*on_completion_cb)(Req_completion* rc, void* cb_arg);


Mp_client* mp_client_create(const char* server_ip, int port, EP_engine* engine);
Mp_client* mp_client_create_from_fd(int connfd, EP_engine* engine);

void mp_client_destroy(Mp_client* client, void (*on_done)(void* arg), void* on_done_arg);

int mp_client_send_request(Mp_client* client, const char* req_msg, 
                            on_completion_cb on_completion, void* cb_arg);

EP_engine* mp_client_get_engine(Mp_client* client);

const char* req_status_str(Req_status rs);

#endif