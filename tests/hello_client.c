#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "ep_engine/epoll_event_engine.h"
#include "msg_pass/mp_client.h"
#include "msg_pass/mp_buf_sizes.h"

#define NUM_WORKER_THREAD 1

int req_cnt;
int response_cnt;
pthread_mutex_t cnt_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cnt_cv = PTHREAD_COND_INITIALIZER;

void handle_response(Req_completion* rc, void* cb_arg) {

    pthread_mutex_lock(&cnt_lock);
    response_cnt++;

    printf("Got response(total %d): %s\n", response_cnt, rc->respose_msg);
    if (response_cnt == req_cnt) {
        pthread_mutex_unlock(&cnt_lock);
        // pthread_cond_signal(&cnt_cv);
        EP_engine* engine = mp_client_get_engine(rc->client);
        ep_engine_stop_event_loop(engine);
        return;
    }
    pthread_mutex_unlock(&cnt_lock);   
}


static void* reader_thread(void* arg) {
    EP_engine* engine = (EP_engine*)arg;
    ep_engine_start_event_loop(engine);
    printf("EV loop has stopped\n");

    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        printf("Usage: %s <server ip> <client name> <use_threadpool> <n_requests>\n", argv[0]);
        exit(0);
    }
    const char* server_ip = argv[1];
    const char* client_name = argv[2];
    bool use_tp = atoi(argv[3]);
    req_cnt = atoi(argv[4]);

    EP_engine* engine = ep_engine_create(use_tp, NUM_WORKER_THREAD);

    Mp_client* client = mp_client_create(server_ip, 30030, engine);
    if (client == NULL) {
        printf("mp_client_create() failed.\n");
        exit(0);
    }

    pthread_t reader_thr;
    pthread_create(&reader_thr, NULL, reader_thread, engine);

    char req_msg[MP_MAXMSGLEN];
    response_cnt = 0;

    for (int i = 0; i < req_cnt; i++) {
        sprintf(req_msg, "Hello world(%s) %d\n", client_name, i);
        mp_client_send_request(client, req_msg, handle_response, engine);

        memset(req_msg, 0, MP_MAXMSGLEN);
    }

    pthread_join(reader_thr, NULL);

    printf("Destroying client...\n");
    mp_client_destroy(client, NULL, NULL);
    printf("Destroying engine...\n");
    ep_engine_destroy(engine);

    return 0;
}