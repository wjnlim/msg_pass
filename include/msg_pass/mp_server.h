/*
    Message passing server
*/
#ifndef MSG_PASSING_SERVER_H
#define MSG_PASSING_SERVER_H

#include <sys/queue.h>
#include "ep_engine/epoll_event_engine.h"
#include "msg_pass/mp_buf_sizes.h"

typedef struct Mp_server Mp_server;
typedef struct Mp_srv_request Mp_srv_request;
typedef struct Client_conn Client_conn;

struct Mp_srv_request {
    char* msg;
    char req_id[MP_HEX_ADDR_LEN];
    Client_conn* client;

    Mp_server* server;
    TAILQ_ENTRY(Mp_srv_request) req_link;
};

typedef void (*handle_request_cb)(Mp_srv_request* reqest, void* cb_arg);

/*
    Create a message passing server.
    if port < 0, a server without listener is created.
*/
Mp_server* mp_server_create(int port, EP_engine* engine, 
                                handle_request_cb handle_request, void* cb_arg);

void mp_server_destroy(Mp_server* server);

void mp_server_add_conn_from_fd(Mp_server* server, int connfd, char* ipaddr);

void mp_server_request_done(Mp_srv_request* req, const char* reply_msg);

const char* mp_server_get_client_ip(Client_conn* client);

EP_engine* mp_server_get_engine(Mp_server* server);

#endif