#include <stdbool.h>
#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <inttypes.h>

#include "msg_pass/mp_client.h"
#include "ep_engine/epoll_event_engine.h"
#include "utils/my_err.h"
#include "utils/socket_helper.h"
#include "mp_conn.h"
#include "msg_pass/mp_buf_sizes.h"
#include "utils_ds/hash_table.h"

typedef struct Server_conn Server_conn;
typedef struct Request Request;

typedef TAILQ_HEAD(Req_q, Request) Req_q;

static Request* request_create(const char* req_msg, 
                            on_completion_cb on_completion, void* on_completion_arg);
static void request_destroy(Request* req);
static void handle_read_resp_msg_done(Mp_conn* mpconn, const char* msg, mpconn_err_t err);
static void handle_write_req_msg_done(Mp_conn* mpconn, Msg* msg, mpconn_err_t err);

struct Mp_client {
    EP_engine* engine;
    /*
        Client side server connection
    */
    struct Server_conn {
        Mp_conn conn;
 
        char ip[INET_ADDRSTRLEN];
        int port;
        int connfd;
    } server;

    Hash_table* request_id_map;

    Req_q requests;
    Req_q sent_requests;

    bool rd_err;
    bool rd_closed;
    bool wr_err;
    bool wr_closed;

    void (*on_destroy_cb)(void* arg);
    void* on_destroy_arg;
    pthread_mutex_t lock;
    pthread_cond_t cv;
};

struct Request {
    Msg msg;
    char* msg_buf;
    char id[MP_HEX_ADDR_LEN];
    int id_len;
    on_completion_cb on_response_fn;
    void* on_response_arg;

    TAILQ_ENTRY(Request) req_link;
};

static Mp_client* create_client(int connfd, const char* server_ip, int port, 
                                                            EP_engine* engine) {
    
    Mp_client* client = malloc(sizeof(*client));
    if (client == NULL) {
        err_msg_sys("[mp_client.c: mp_client_create()] malloc() error");
        goto CLIENT_ALLOC_ERR;
    }

    client->engine = engine;

    if (connfd < 0) {
        int conn_fd = sock_connect(server_ip, port);
        if (conn_fd < 0) {
            err_msg("[mp_client.c: mp_client_create()] sock_connect() error");
            goto CONNECT_ERR;
        }
        snprintf(client->server.ip, INET_ADDRSTRLEN, "%s", server_ip);
        client->server.port = port;
        client->server.connfd = conn_fd;
    } else {
        struct sockaddr_in server_addr;
        socklen_t server_addr_len = sizeof(server_addr);
        if (getpeername(connfd, (struct sockaddr*)&server_addr, &server_addr_len) < 0) {
            err_msg("[mp_client.c: mp_client_create()] getpeername() error");
            goto GETPEER_ERR;
        }
        inet_ntop(AF_INET, &server_addr.sin_addr, client->server.ip, sizeof(client->server.ip));
        client->server.port = ntohs(server_addr.sin_port);
        client->server.connfd = connfd;
    }

    if (!setnonblock(client->server.connfd)) {
        err_msg_sys("[mp_client.c: mp_client_create()] setnonblock() error");
        goto NONBLOCK_ERR;
    }

    // snprintf(client->server.ip, INET_ADDRSTRLEN, "%s", server_ip);
    // client->server.port = port;
    // client->server.connfd = connfd;
    if (mp_conn_init(&client->server.conn, client, client->server.connfd, engine,
            handle_read_resp_msg_done, handle_write_req_msg_done) < 0) {
        err_msg("[mp_client.c: mp_client_create()] mp_conn_init() error");
        goto MP_CONN_INIT_ERR;
    }

    Hash_table* ht = hash_table_create();
    if (ht == NULL) {
        err_msg("[mp_client.c: mp_client_create()] hash_table_create() error");
        goto HT_CREATE_ERR;
    }
    client->request_id_map = ht;

    TAILQ_INIT(&client->requests);
    TAILQ_INIT(&client->sent_requests);

    client->rd_err = false;
    client->rd_closed = false;
    client->wr_err = false;
    client->wr_closed = false;

    client->on_destroy_cb = NULL;
    client->on_destroy_arg = NULL;
    pthread_mutex_init(&client->lock, NULL);
    pthread_cond_init(&client->cv, NULL);


    mp_conn_start_reading_msgs(&client->server.conn);

    return client;
HT_CREATE_ERR:
MP_CONN_INIT_ERR:
NONBLOCK_ERR:
    if (connfd < 0) {
        close(client->server.connfd);
    }
GETPEER_ERR:
CONNECT_ERR:
    free(client);
CLIENT_ALLOC_ERR:
    return NULL;
}

Mp_client* mp_client_create(const char* server_ip, int port, EP_engine* engine) {
    assert(server_ip != NULL && engine != NULL);

    return create_client(-1, server_ip, port, engine);
}

Mp_client* mp_client_create_from_fd(int connfd, EP_engine* engine) {
    assert(engine != NULL);

    return create_client(connfd, NULL, -1, engine);
}

static void destroy_requests(Req_q* request_q) {
    Request* req;
    while ((req = TAILQ_FIRST(request_q)) != NULL) {
        TAILQ_REMOVE(request_q, req, req_link);
        request_destroy(req);
    }
}

static void destroy_client(Mp_conn* mpconn, void* arg) {
    assert(arg != NULL);
    Mp_client* client = arg;

    if (client->on_destroy_cb)
        client->on_destroy_cb(client->on_destroy_arg);

    hash_table_destroy(client->request_id_map);

    destroy_requests(&client->requests);
    destroy_requests(&client->sent_requests);

    pthread_mutex_destroy(&client->lock);
    pthread_cond_destroy(&client->cv);
    free(client);
}

void mp_client_destroy(Mp_client* client, void (*on_done)(void* arg), void* on_done_arg) {
    pthread_mutex_lock(&client->lock);
    client->on_destroy_cb = on_done;
    client->on_destroy_arg = on_done_arg;

    client->rd_closed = true;
    client->wr_closed = true;

    pthread_mutex_unlock(&client->lock);

    mp_conn_deinit(&client->server.conn, destroy_client, client);
}

static Request* request_create(const char* req_msg, 
                            on_completion_cb on_completion, void* on_completion_arg) {

    Request* req = (Request*)malloc(sizeof(*req));
    if (req == NULL) {
        err_msg_sys("[mp_client.c: request_create()] malloc(req) error");
        return NULL;
    }
    // set req id
    req->id_len = sprintf(req->id, "%" PRIxPTR, (uintptr_t)req);

    char buf[MP_MAXMSGLEN];
    int msg_len = snprintf(buf, MP_MAXMSGLEN, "%s %s", req->id, req_msg);

    if (buf[msg_len-1] != '\n') {
        buf[msg_len++] = '\n';
        // len++;
        if (msg_len == MP_MAXMSGLEN) {
            err_msg("mp_client.c: request_create()] req_msg exceeded MP_MAXMSGLEN");
            free(req);
            return NULL;
        }
    }

    char* msg_buf = malloc(msg_len+1);
    if (msg_buf == NULL) {
        err_msg_sys("[mp_client.c: request_create()] malloc(msg_buf) error");
        free(req);
        return NULL;
    }
    strncpy(msg_buf, buf, msg_len+1);

    req->msg_buf = msg_buf;
    req->msg.buf = req->msg_buf;

    req->msg.first = req->msg.buf;
    req->msg.last = req->msg.first + msg_len;
    req->msg.size = msg_len;

    req->on_response_fn = on_completion;
    req->on_response_arg = on_completion_arg;

    return req;
}

static void request_destroy(Request* req) {
    assert(req != NULL);
    free(req->msg_buf);
    free(req);
}

int mp_client_send_request(Mp_client* client, const char* req_msg, 
                            on_completion_cb on_completion, void* on_completion_arg) {

    pthread_mutex_lock(&client->lock);
    if (client->wr_closed) {
        pthread_mutex_unlock(&client->lock);
        err_msg("[mp_client.c: mp_client_send_request()] The client's write half is closed");
        return -1;
    }
    pthread_mutex_unlock(&client->lock);

    Request* req = request_create(req_msg, on_completion, on_completion_arg);
    if (req == NULL) {
        err_exit("[mp_client.c: mp_client_send_request()] request_create() error");
    }

    if (!hash_table_put_locked(client->request_id_map, req->id, req)) {
        err_exit("[mp_client.c: mp_client_send_request()] hash_table_put() error");
    }
#ifdef DEBUG_MP
    printf("[mp_client.c: mp_client_send_request()] put {%s, %p} in hash table\n",
        req->id, hash_table_get(client->request_id_map, req->id));
#endif

    pthread_mutex_lock(&client->lock);
    TAILQ_INSERT_TAIL(&client->requests, req, req_link);
    pthread_mutex_unlock(&client->lock);

    if (mp_conn_initiate_write_msg(&client->server.conn, &req->msg) < 0) {
        err_msg("[mp_client.c: mp_client_send_request()] mp_conn_initiate_write_msg() error");
        return -1;
    }

    return 0;
}

static void request_done(Request* req, Req_status rs, const char* resp_msg, Mp_client* client) {
    if (req->on_response_fn) {
        Req_completion rc = {.status = rs,
                             .request_msg = req->msg.buf,
                             .respose_msg = resp_msg,
                             .client = client};
        req->on_response_fn(&rc, req->on_response_arg);
    }
    if (!hash_table_delete_locked(client->request_id_map, req->id)) {
        err_exit("[mp_client.c: request_done()] hash_table_delete_locked(): Failed to delete item with key(%s)", req->id);
    }
    request_destroy(req);
}

static void handle_write_req_msg_done(Mp_conn* mpconn, Msg* msg, mpconn_err_t err) {
    Mp_client* client = mpconn->data;
    Request* req = (Request*)msg;

    pthread_mutex_lock(&client->lock);
    TAILQ_REMOVE(&client->requests, req, req_link);

    if (err == MPCONN_ERR_NONE && !client->rd_closed) {
        TAILQ_INSERT_TAIL(&client->sent_requests, req, req_link);
        pthread_mutex_unlock(&client->lock);

        return;
    }

    Req_status rs;
    client->wr_closed = true;
    if (err == MPCONN_IO_ERR) {
        client->wr_err = true;
        rs = REQ_SEND_ERR;
    } else if (err == MPCONN_WR_CLOSED) {
        // client->wr_closed= true;
        rs = REQ_SEND_CLOSED;
    } else if (client->rd_closed) {
        rs = (client->rd_err) ? REQ_RECV_RESP_ERR: REQ_RECV_RESP_CLOSED;
    }
    pthread_mutex_unlock(&client->lock);
    request_done(req, rs, NULL, client);

    return;
}

static void flush_sent_requests(Mp_client* client, Req_status rs) {
    Request* req;
    while ((req = TAILQ_FIRST(&client->sent_requests)) != NULL) {
        TAILQ_REMOVE(&client->sent_requests, req, req_link);
        request_done(req, rs, NULL, client);
    }
}

static void handle_read_resp_msg_done(Mp_conn* mpconn, const char* msg, mpconn_err_t err) {
#ifdef DEBUG_MP
    printf("[mp_client.c: handle_read_resp_msg_done()] msg: %s\n", msg);
#endif
    Mp_client* client = mpconn->data;
    if (err == MPCONN_ERR_NONE) {
        char req_id[MP_HEX_ADDR_LEN];
        sscanf(msg, "%s ", req_id);

        Request* req = hash_table_get_locked(client->request_id_map, req_id);
        if (req == NULL) { // req should be in the map; thus, this is error
            err_exit("[mp_client.c: handle_read_resp_msg_done()] hash_table_get(): Couldn't find req object from id(%s)", req_id);
        }
        pthread_mutex_lock(&client->lock);
        TAILQ_REMOVE(&client->sent_requests, req, req_link);
        pthread_mutex_unlock(&client->lock);

        int id_len = strlen(req_id);
        request_done(req, REQ_ERR_NONE, msg + (id_len + 1), client);
        return;
    }

    Req_status rs;
    pthread_mutex_lock(&client->lock);
    client->rd_closed = true;
    rs = REQ_RECV_RESP_CLOSED;
    if (err == MPCONN_IO_ERR) {
        client->rd_err = true;
        rs = REQ_RECV_RESP_ERR;
    }
    pthread_mutex_unlock(&client->lock);
    flush_sent_requests(client, rs);

    return;
}

EP_engine* mp_client_get_engine(Mp_client* client) {
    return client->engine;
}

const char* req_status_str(Req_status rs) {
    switch (rs) {
    case REQ_ERR_NONE: {
        return "REQ_ERR_NONE";
    }
    case REQ_SEND_ERR: {
        return "REQ_SEND_ERR";
    }
    case REQ_SEND_CLOSED: {
        return "REQ_SEND_CLOSED";
    }
    case REQ_RECV_RESP_ERR: {
        return "REQ_RECV_RESP_ERR";
    }
    case REQ_RECV_RESP_CLOSED: {
        return "REQ_RECV_RESP_CLOSED";
    }
    default: {
        return "Unkown req status";
    }
    }
}