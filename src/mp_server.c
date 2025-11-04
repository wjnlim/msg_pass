#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "msg_pass/mp_server.h"
#include "ep_engine/epoll_event_engine.h"
#include "utils/my_err.h"
#include "utils/socket_helper.h"
#include "mp_conn.h"
#include "utils_ds/hash_table.h"

static void client_conn_destroy_locked(Client_conn* ccn);
static void accept_conns(Epolled_fd* epdfd, void* arg, Epdfd_state fd_state);
static void handle_write_reply_msg_done(Mp_conn* mpconn, Msg* msg, mpconn_err_t err);
static void handle_read_req_msg_done(Mp_conn* mpconn, const char* msg, mpconn_err_t err);
static Mp_srv_request* request_create(const char* req_msg, Client_conn* ccn);
static void request_destroy(Mp_srv_request* req);

typedef TAILQ_HEAD(Client_q, Client_conn) Client_q;
typedef TAILQ_HEAD(Mp_srv_req_q, Mp_srv_request) Mp_srv_req_q;

typedef struct Listener {
    int port;
    // int listenfd;

    Epolled_fd* listen_epdfd;

    Mp_server* server;
} Listener;

struct Mp_server {
    Listener* listener;
    // struct listener {
    //     int port;
    //     int listenfd;

    //     Epolled_fd* listen_epdfd;

    //     Mp_server* server;
    // } listener;

    EP_engine* engine;
    handle_request_cb handle_request;
    void* cb_arg;

    Client_q clients;

    int ref_cnt;
    pthread_mutex_t lock;
    pthread_cond_t cv;
};

/*
    Server side client connection
    which represent a client
*/
struct Client_conn {
    Mp_conn conn;
    int connfd;

    char ipaddr[INET_ADDRSTRLEN];

    Mp_server* server;

    Mp_srv_req_q requests;

    bool closed;
    bool destroying;

    pthread_mutex_t lock;
    TAILQ_ENTRY(Client_conn) client_link;
};

typedef struct Mp_srv_reply {
    Msg msg;
    char* msg_buf;

    Mp_srv_request* req;
} Mp_srv_reply;


static Mp_server* ref_server(Mp_server* server) {
    pthread_mutex_lock(&server->lock);
    server->ref_cnt++;
    pthread_mutex_unlock(&server->lock);

    return server;
}

static void destroy_server(Mp_server* server) {
    pthread_mutex_destroy(&server->lock);
    pthread_cond_destroy(&server->cv);

    free(server);
}

static void unref_server(Mp_server* server) {
    pthread_mutex_lock(&server->lock);
    server->ref_cnt--;
    if (server->ref_cnt == 0) {
        destroy_server(server);
    }
    pthread_mutex_unlock(&server->lock);
}

// static void start_listen(Mp_server* server, int port) {
static void start_listen(Listener* listener) {

    // server->listener.port = port;
    int listenfd = sock_bind(listener->port);
    if (listenfd < 0) {
        err_exit("[mp_server.c: start_listen()] sock_bind() error");
    }
    if (!setnonblock(listenfd)) {
        err_exit_sys("[mp_server.c: start_listen()] setnonblock() error");
    }

    // start listening connections
    if (listen(listenfd, LISTENQ) < 0) {
        close(listenfd);
        err_exit_sys("[mp_server.c: start_listen()] listen() error");
    }

    // EP_engine* engine = server->engine;
    Epolled_fd* listen_epdfd = epdfd_create(listenfd, listener->server->engine);
    if (listen_epdfd == NULL) {
        close(listenfd);
        err_exit_sys("[mp_server.c: start_listen()] epdfd_create() error");
    }

    // server->listener.listenfd = listenfd;
    listener->listen_epdfd = listen_epdfd;
    // server->listener.server = ref_server(server);

    epdfd_notify_on_readable(listen_epdfd, accept_conns, listener);
}

static Listener* listener_create(int port, Mp_server* server) {
    Listener* l = (Listener*)malloc(sizeof(*l));
    if (l == NULL) {
        err_msg_sys("[mp_server.c: listener_create()] malloc() error");
        return NULL;
    }

    l->port = port;
    l->server = ref_server(server);
    start_listen(l);

    return l;
}

static void listener_destroy(Listener* listener, 
                           void (*on_done)(void *arg), void *on_done_arg) {
    assert(listener != NULL);
    epdfd_destroy(listener->listen_epdfd, on_done, on_done_arg);
    free(listener);
}

Mp_server* mp_server_create(int port, EP_engine* engine, 
                                handle_request_cb handle_request, void* cb_arg) {
    Mp_server* server = malloc(sizeof(*server));
    if (server == NULL) {
        err_msg_sys("[mp_server.c: mp_server_create()] malloc() error");
        return NULL;
    }

    server->listener = NULL;
    server->engine = engine;
    server->handle_request = handle_request;
    server->cb_arg = cb_arg;

    TAILQ_INIT(&server->clients);

    pthread_mutex_init(&server->lock, NULL);
    pthread_cond_init(&server->cv, NULL);

    server->ref_cnt = 1;
    // start_listen(server, port);
    if (port >= 0) {
        server->listener = listener_create(port, server);
    }

    return server;
}

static void destroy_client_conns(Mp_server* server) {
    // Mp_server* server = arg;

    Client_conn* ccn;
    pthread_mutex_lock(&server->lock);
    while((ccn = TAILQ_FIRST(&server->clients)) != NULL) {
        TAILQ_REMOVE(&server->clients, ccn, client_link);
        client_conn_destroy_locked(ccn);
    }
    pthread_mutex_unlock(&server->lock);

    // unref_server(server);
}

static void on_listen_epdfd_destroy(void* arg) {
    Mp_server* server = arg;

    destroy_client_conns(server);
    // Client_conn* ccn;
    // pthread_mutex_lock(&server->lock);
    // while((ccn = TAILQ_FIRST(&server->clients)) != NULL) {
    //     TAILQ_REMOVE(&server->clients, ccn, client_link);
    //     client_conn_destroy_locked(ccn);
    // }
    // pthread_mutex_unlock(&server->lock);

    unref_server(server);
}

void mp_server_destroy(Mp_server* server) {
    assert(server != NULL);
    if (server->listener != NULL) {
        listener_destroy(server->listener, on_listen_epdfd_destroy, server);
        return;
    }

    destroy_client_conns(server);
    unref_server(server);
    // epdfd_destroy(server->listener.listen_epdfd, on_listen_epdfd_destroy, server);
}

const char* mp_server_get_client_ip(Client_conn* client) {
    assert(client != NULL);
    return (const char*)client->ipaddr;
}

static Client_conn* client_conn_create(int connfd, char* ipaddr, Mp_server* server, EP_engine* engine) {
    assert(server != NULL && engine != NULL);

#ifdef DEBUG_MP
    printf("[mp_server.c: client_conn_create()] Creating client conn(%d)...\n", connfd);
#endif

    Client_conn* ccn = malloc(sizeof(*ccn));
    if (ccn == NULL) {
        err_msg_sys("[mp_server.c: client_conn_create()] malloc() error");
        return NULL;
    }

    ccn->connfd = connfd;
    strncpy(ccn->ipaddr, ipaddr, INET_ADDRSTRLEN);
    ccn->server = ref_server(server);

    TAILQ_INIT(&ccn->requests);
    ccn->closed = false;
    ccn->destroying = false;

    pthread_mutex_init(&ccn->lock, NULL);

    mp_conn_init(&ccn->conn, ccn, connfd, engine,
                 handle_read_req_msg_done, handle_write_reply_msg_done);

    return ccn;
}

static void destroy_client(Mp_conn* mpconn, void* arg) {
    Client_conn* ccn = arg;
    // flush requests
    Mp_srv_request* req;
    while ((req = TAILQ_FIRST(&ccn->requests)) != NULL) {
        TAILQ_REMOVE(&ccn->requests, req, req_link);
        request_destroy(req);
    }

    pthread_mutex_lock(&ccn->server->lock);
    TAILQ_REMOVE(&ccn->server->clients, ccn, client_link);
    pthread_mutex_unlock(&ccn->server->lock);

    unref_server(ccn->server);

    pthread_mutex_destroy(&ccn->lock);
    free(ccn);
}

static void client_conn_destroy(Client_conn* ccn) {
    if (ccn->destroying)
        return;
    
#ifdef DEBUG_MP
    printf("[mp_server.c: client_conn_destroy()] Destroying client conn(%d)...\n", ccn->connfd);
#endif
    ccn->destroying = true;
    mp_conn_deinit(&ccn->conn, destroy_client, ccn);
}

static void client_conn_destroy_locked(Client_conn* ccn) {
    pthread_mutex_lock(&ccn->lock);
    client_conn_destroy(ccn);
    pthread_mutex_unlock(&ccn->lock);
}

void mp_server_add_conn_from_fd(Mp_server* server, int connfd, char* ipaddr) {
    if (!setnonblock(connfd)) {
        err_exit_sys("[mp_server.c: mp_server_add_conn_from_fd()] setnonblock() error");
    }

    Client_conn* ccn = client_conn_create(connfd, ipaddr, server, server->engine);
    if (ccn == NULL) {
        err_exit("[mp_server.c: mp_server_add_conn_from_fd()] client_conn_create() error");
    }
#ifdef DEBUG_MP
    printf("[mp_server.c: mp_server_add_conn_from_fd()] Created client conn(%d).\n", connfd);
#endif
    pthread_mutex_lock(&server->lock);
    TAILQ_INSERT_TAIL(&server->clients, ccn, client_link);
    pthread_mutex_unlock(&server->lock);

// #ifdef DEBUG_MP
//     printf("[mp_server.c: accept_conns()] Client conn(%d) starts reading.\n", connfd);
// #endif
    mp_conn_start_reading_msgs(&ccn->conn);
}

/*
    accept_cb
*/
static void accept_conns(Epolled_fd* epdfd, void* arg, Epdfd_state fd_state) {

    Listener* listener = arg;
    Mp_server* server = listener->server;

    switch (fd_state) {
        case EPDFD_READABLE: {
            // int listenfd = listener->listenfd;
            int listenfd = epdfd_get_fd(listener->listen_epdfd);

            struct sockaddr_in clientaddr;
            socklen_t clientlen = sizeof(struct sockaddr_in);
            int connfd;
            while ((connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen)) >= 0) {
                char ipaddr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &clientaddr.sin_addr, ipaddr, INET_ADDRSTRLEN);
            #ifdef DEBUG_MP
                printf("[mp_server.c: accept_conns()] Accepted a connection from %s\n", ipaddr);
            #endif
                mp_server_add_conn_from_fd(server, connfd, ipaddr);
            }

            if (connfd < 0 && errno != EAGAIN) {
                err_exit_sys("[mp_server.c: on_accept()] accept() error");
            }
            // EAGAIN
            epdfd_notify_on_readable(listener->listen_epdfd, accept_conns, listener);
            break;
        }
        case EPDFD_RD_SHUTDOWN: {
            break;
        }
        default: {
            err_exit("[mp_server.c: accept_conns()] Invalid fd_state");
        }
    }
}

static Mp_srv_reply* reply_create(const char* reply_msg, Mp_srv_request* req) {
    Mp_srv_reply* rp = (Mp_srv_reply*)malloc(sizeof(*rp));
    if (rp == NULL) {
        err_msg_sys("[mp_server.c: reply_create()] malloc(req) error");
        return NULL;
    }

    char buf[MP_MAXMSGLEN];
    int msg_len = snprintf(buf, MP_MAXMSGLEN, "%s %s", req->req_id, reply_msg);

    if (buf[msg_len-1] != '\n') {
        buf[msg_len++] = '\n';
        if (msg_len == MP_MAXMSGLEN) {
            err_msg("[mp_server.c: reply_create()] reply msg exceeded MP_MAXMSGLEN");
            free(rp);
            return NULL;
        }
    }

    char* msg_buf = malloc(msg_len+1);
    if (msg_buf == NULL) {
        err_msg("[mp_server.c: reply_create()] malloc(msg_buf) error");
        free(rp);
        return NULL;
    }
    strncpy(msg_buf, buf, msg_len+1);

    rp->msg_buf = msg_buf;
    rp->msg.buf = rp->msg_buf;

    rp->msg.first = rp->msg.buf;
    rp->msg.last = rp->msg.first + msg_len;
    rp->msg.size = msg_len;

    rp->req = req;
    return rp;
}

static void reply_destroy(Mp_srv_reply* reply) {
    assert(reply != NULL);
    free(reply->msg_buf);
    free(reply);
}

static void request_done_locked(Mp_srv_request* req) {
    Client_conn* ccn = req->client;
    pthread_mutex_lock(&ccn->lock);
    TAILQ_REMOVE(&ccn->requests, req, req_link);
    request_destroy(req);

    if (TAILQ_EMPTY(&ccn->requests) && ccn->closed) {
        client_conn_destroy(ccn);
    }
    pthread_mutex_unlock(&ccn->lock);
}

void mp_server_request_done(Mp_srv_request* req, const char* reply_msg) {
    Client_conn* ccn = req->client;
    if (reply_msg) {
        // Send reply
        Mp_srv_reply* rp = reply_create(reply_msg, req);
        if (rp == NULL) {
            err_exit("[mp_server.c: mp_server_request_done()] reply_create() error");
        }

        if (mp_conn_initiate_write_msg(&ccn->conn, &rp->msg) < 0) {
            reply_destroy(rp);
            request_done_locked(req);
        }

    } else {
        request_done_locked(req);
    }
}

static Mp_srv_request* request_create(const char* req_msg, Client_conn* ccn) {
    Mp_srv_request* req = (Mp_srv_request*)malloc(sizeof(*req));
    if (req == NULL) {
        err_msg_sys("[mp_server.c: request_create()] malloc(req) error");
        return NULL;
    }

    sscanf(req_msg, "%s ", req->req_id);
    int id_len = strlen(req->req_id);
    int msg_len = strlen(req_msg);

    char* buf = malloc(msg_len - id_len);
    if (buf == NULL) {
        err_msg_sys("[mp_server.c: request_create()] malloc(buf) error");
        free(req);
        return NULL;
    }

    sprintf(buf, "%s", req_msg + id_len+1);
    req->msg = buf;

    req->client = ccn;

    req->server = ccn->server;
    return req;
}

static void request_destroy(Mp_srv_request* req) {
    assert(req != NULL);
    free(req->msg);
    free(req);
}

static void handle_read_req_msg_done(Mp_conn* mpconn, const char* msg, mpconn_err_t err) {
    Client_conn* ccn = mpconn->data;
    if (err == MPCONN_ERR_NONE) {
        Mp_srv_request* req = request_create(msg, ccn);
        if (req == NULL) {
            err_exit("[mp_server.c: handle_read_req_msg_done()] request_create() error");
        }

        pthread_mutex_lock(&ccn->lock);
        TAILQ_INSERT_TAIL(&ccn->requests, req, req_link);
        pthread_mutex_unlock(&ccn->lock);
        ccn->server->handle_request(req, ccn->server->cb_arg);
        return;
    }

    pthread_mutex_lock(&ccn->lock);
    if (TAILQ_EMPTY(&ccn->requests)) {
        client_conn_destroy(ccn);
    } else {
        ccn->closed = true;
    }
    pthread_mutex_unlock(&ccn->lock);
 
    return;
}

static void handle_write_reply_msg_done(Mp_conn* mpconn, Msg* msg, mpconn_err_t err) {
    Mp_srv_reply* reply = (Mp_srv_reply*)msg;
    Mp_srv_request* req = reply->req;

    reply_destroy(reply);
    request_done_locked(req);

    return;
}

EP_engine* mp_server_get_engine(Mp_server* server) {
    return server->engine;
}