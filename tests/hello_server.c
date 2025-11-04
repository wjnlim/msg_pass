#include <string.h>
#include <stdio.h>
#include <pthread.h>

// #include "ep_engine/epoll_event_engine.h"
#include "ep_engine/epoll_event_engine.h"
#include "msg_pass/mp_server.h"
// #include "mp_msg_buf.h"

#define SERVER_PORT 30030
#define NUM_WORKER_THREAD 4

void handle_request(Mp_srv_request* request, void* cb_arg) {
    // char* req_msg = req->req_msg;
    const char* req_msg = request->msg;
    // do work with req_msg
    int n;
    char client_name[256];
    memset(client_name, 0, sizeof(client_name));
    sscanf(req_msg, "Hello world(%[^)]) %d\n", client_name, &n);
    printf("hello_server.c: got msg from client(%s): %s\n",
            client_name, req_msg);

    char reply_msg[MP_MAXMSGLEN];
    sprintf(reply_msg, "This is hello reply(%s) %d\n", client_name, n);

    mp_server_request_done(request, reply_msg);
}

int main(int argc, char** argv) {
    EP_engine* engine = ep_engine_create(true, NUM_WORKER_THREAD);
    Mp_server* server = mp_server_create(SERVER_PORT, engine, handle_request, NULL);
    
    // pthread_t tid = ep_engine_start_event_loop(engine);
    ep_engine_start_event_loop(engine);

    // pthread_join(tid, NULL);

    // mp_server_destroy(server);
    // ep_engine_destroy(engine);

    return 0;
}