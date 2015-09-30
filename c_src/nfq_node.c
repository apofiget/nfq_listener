
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <syscall.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include <netinet/in.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include "erl_interface.h"
#include "ei.h"

#include "nfq_node.h"

static int my_listen(int);

static void *erl_message_read_loop(void *);
static void *read_queue_loop(void *);

static int cb(struct nfq_q_handle *, struct nfgenmsg *, struct nfq_data *, void *);
static int push(m_stack_t *, void *, int);
static void *pop(m_stack_t *);

static int get_queue_idx(int, q_data_t **);
static int get_free_idx(q_data_t **);

static response_t create_queue(int, q_data_t **, thread_data_t *);
static response_t destroy_queue(int, q_data_t **, thread_data_t *);
static response_t queue_list(q_data_t **);
static response_t set_mode(int, ETERM *, q_data_t **, thread_data_t *);
static response_t set_queue_len(int, ETERM *, q_data_t **, thread_data_t *);
static response_t read_pkt_start(int, q_data_t **, thread_data_t *);
static response_t read_pkt_stop(int, q_data_t **, thread_data_t *);

int main(int argc, char **argv) {

    int port;                                // Listen port number
    int fd;                                  // Socket descriptor
    int listen;                              // Listen socket
    ErlConnect conn;                         // Connection data
    char *cookie;                            // Erlang magic cookie
    pid_t pid;
    int x;
    pthread_t thread;
    pthread_attr_t thread_attrs;
    thread_data_t *data;                     // Per thread data: thread ID,
                                             // connection node name
    static unsigned int tidx = 0;

    pid = fork();

    if (pid < 0)
        exit(EXIT_FAILURE);

    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0)
        exit(EXIT_FAILURE);

    umask(0);
    chdir("/");

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);

    if (argc == 3)
    {
        port = atoi(argv[1]);
        cookie = argv[2];
    } else {
        fprintf(stderr, "Usage: %s <port number> <erlang secret cookie>\n\r",argv[0]);
        exit(EXIT_FAILURE);
    }

    for (x = sysconf(_SC_OPEN_MAX); x>0; x--)
        close (x);

    setlogmask (LOG_UPTO (LOG_NOTICE));
    openlog("nfq_node", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);

    erl_init(NULL, 0);

    if (erl_connect_init(1, cookie, 0) == -1)
        erl_err_quit("erl_connect_init");

    if ((listen = my_listen(port)) <= 0)
        erl_err_quit("my_listen");

    if (erl_publish(port) == -1)
        erl_err_quit("erl_publish");

    if (pthread_attr_init(&thread_attrs)) {
        syslog(LOG_ERR, "error while init pthread attr struct\n\r");
        exit(EXIT_FAILURE);
    }

    if ((pthread_attr_setdetachstate(&thread_attrs, PTHREAD_CREATE_DETACHED))) {
        syslog(LOG_ERR, "error while set pthread attributes\n\r");
        exit(EXIT_FAILURE);
    }

    syslog(LOG_NOTICE,"Waiting for connections...\n\r");

    for(;;) {

        while((fd = erl_accept(listen, &conn)) == ERL_ERROR)
            syslog(LOG_ERR,"%s Connection error\n\r", argv[0]);

        if((data = (thread_data_t *)malloc(sizeof(thread_data_t))) == NULL) {
            syslog(LOG_ERR, "Memory allocation error\n\r");
            exit(EXIT_FAILURE);
        }

        if((data->node = (char *)malloc(strlen(conn.nodename)+1)) == NULL) {
            syslog(LOG_ERR, "Memory allocation error\n\r");
            exit(EXIT_FAILURE);
        }

        data->fd = fd;
        data->idx = tidx;
        strcpy(data->node, conn.nodename);

        if (pthread_create(&thread, &thread_attrs, erl_message_read_loop, data)) {
            syslog(LOG_ERR, "Thread create failure\n\r");
            exit(EXIT_FAILURE);
        }
        tidx++;
    }

    closelog();
    return 0;
}

void *erl_message_read_loop(void *arg_data) {

    thread_data_t *data = (thread_data_t *)arg_data;
    ErlMessage emsg;                           // Incoming message
    unsigned char buf[BUFSIZE];                // Buffer for incoming message
    int got;                                   // Result of receive
    ETERM *fromp, *tuplep, *fun, *arg, *resp;  // Erlang terms
    int loop = 1;                              // Loop flag
    char *f_name_atom;
    int q_num;
    int i = 0, temp;
    q_data_t *q_data[MAX_Q_NUMS];       // Per queue data
    response_t r;

    /*
      Exepected Erlang term:
      {from_pid(), {fun_name_atom, argument}}}
      or
      {from_pid(), {fun_name_atom, argument1, argument2}}}
      and returned term:
      {call_state, response}
    */

    syslog(LOG_NOTICE,"[%d] Connection with node: %s\n\r",data->idx, data->node);

    for(i = 0; i < MAX_Q_NUMS; i++) {
        q_data[i] = malloc(sizeof(q_data_t));
        q_data[i]->q_num = -1;
        q_data[i]->h = NULL;
        q_data[i]->qh = NULL;
        q_data[i]->pid = 0;
    }

    pthread_setname_np(pthread_self(), "msg_read_loop");

    while (loop) {

        got = erl_receive_msg(data->fd, buf, BUFSIZE, &emsg);

        switch (got) {
        case ERL_TICK:
            syslog(LOG_NOTICE,"[%d] %s tick\n\r",data->idx, data->node);
            break;
        case ERL_ERROR:
            syslog(LOG_NOTICE,"[%d] %s erl_receive_msg error or node down\n\r",data->idx, data->node);
            loop = 0;
            break;
        case ERL_MSG:
            if (emsg.type == ERL_REG_SEND || emsg.type ==  ERL_SEND) {

                fromp = erl_element(1, emsg.msg);
                tuplep = erl_element(2, emsg.msg);
                fun = erl_element(1, tuplep);
                arg = erl_element(2, tuplep);

                f_name_atom = ERL_ATOM_PTR(fun);

                syslog(LOG_NOTICE,"[%d] %s %s()\n\r", data->idx, data->node, f_name_atom);

                if(strcmp(f_name_atom, "create_queue") == 0) {

                    q_num = ERL_INT_VALUE(arg);
                    r = create_queue(q_num, q_data, data);

                } else if(strcmp(f_name_atom, "destroy_queue") == 0) {

                    q_num = ERL_INT_VALUE(arg);
                    r = destroy_queue(q_num, q_data, data);

                } else if(strcmp(f_name_atom, "queue_list") == 0) {

                    r = queue_list(q_data);

                } else if(strcmp(f_name_atom, "set_mode") == 0) {

                    temp = ERL_INT_VALUE(arg);
                    r = set_mode(temp, tuplep, q_data, data);

                }  else if(strcmp(f_name_atom, "set_queue_len") == 0) {

                    temp = ERL_INT_VALUE(arg);
                    r = set_queue_len(temp, tuplep, q_data, data);

                } else if(strcmp(f_name_atom, "read_pkt_start") == 0) {
                    q_num = ERL_INT_VALUE(arg);
                    r = read_pkt_start(q_num, q_data, data);

                } else if(strcmp(f_name_atom, "read_pkt_stop") == 0) {
                    q_num = ERL_INT_VALUE(arg);
                    r = read_pkt_stop(q_num, q_data, data);

                }
                else {
                    r.cs = erl_mk_atom("error");
                    r.rsp = erl_mk_estring("no such function", strlen("no such function"));
                }

                if ((resp = erl_format("{cnode, {reply, {~w, ~w}}}", r.cs, r.rsp)) != NULL) {
                    if(!erl_send(data->fd, fromp, resp)) {
                        syslog(LOG_ERR,"[%d] %s send reply error, exit loop\n\r", data->idx, data->node);
                        loop = 0;
                    }
                } else {
                    syslog(LOG_ERR,"[%d] %s term format error\n\r", data->idx, data->node);
                    loop = 0;
                }

                erl_free_term(emsg.from);
                erl_free_term(emsg.msg);
                erl_free_term(fromp);
                erl_free_term(tuplep);
                erl_free_term(fun);
                erl_free_term(arg);
                erl_free_term(resp);
                erl_free_term(r.rsp);
                erl_free_term(r.cs);
            }
            break;
        default:
            syslog(LOG_ERR,"[%d] %s something wrong! :(\n\r", data->idx, data->node);
            loop = 0;
            break;
        }
    } /* while */

    for(i = 0; i < MAX_Q_NUMS; i++) {

        if(q_data[i]->pid > 0)
            pthread_cancel(q_data[i]->thread);

        if (q_data[i]->q_num >= 0) {
            nfq_destroy_queue(q_data[i]->qh);
            nfq_close(q_data[i]->h);
        }
        free(q_data[i]);
    }

    syslog(LOG_ERR,"[%d] %s thread stop\n\r", data->idx, data->node);

    free(data->node);
    free(data);

    pthread_exit(NULL);
}

static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa, void *data)
{
    struct nfqnl_msg_packet_hdr *ph;
    //m_stack_t *st = (m_stack_t *)data;
    unsigned char *packet;
    u_int32_t id, ret;

    ph = nfq_get_msg_packet_hdr(nfa);
    id = ntohl(ph->packet_id);

    if ((ret = nfq_get_payload(nfa, &packet)) >= 0){
        syslog(LOG_NOTICE, "packet catched id: %d, payload_len=%d\n\r", id, ret);

    }

    return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
}

void *read_queue_loop(void *arg) {

    q_data_t *data = (q_data_t *)arg;
    int rv, fd;
    char buf[4096] __attribute__ ((aligned));

    data->pid = syscall(SYS_gettid);

    fd = nfq_fd(data->h);

    syslog(LOG_NOTICE, "Packet read thread started, queue: %d, TID: %d...\n\r", data->q_num, data->pid);

    while ((rv = recv(fd, buf, sizeof(buf), 0)) && rv >= 0) {
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        nfq_handle_packet(data->h, buf, rv);
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }

    pthread_exit(NULL);
}

int my_listen(int port) {

    int listen_fd;
    struct sockaddr_in addr;
    int on = 1;

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset((void*) &addr, 0, (size_t) sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0)
        return (-1);

    listen(listen_fd, 5);
    return listen_fd;
}

// Get free array element index
int get_free_idx(q_data_t **data) {

    int idx = 0;

    while(data[idx]->q_num >= 0 && idx < MAX_Q_NUMS)
        idx++;
    return (idx  == MAX_Q_NUMS) ? -1 : idx;
}

// Get array element index for given queue number
int get_queue_idx(int q, q_data_t **data) {

    int idx = 0;

    while(idx != MAX_Q_NUMS && data[idx]->q_num != q)
        idx++;
    return (idx  == MAX_Q_NUMS) ? -1 : idx;
}

// Stack operation
int push(m_stack_t *st, void *data, int size) {

    if(st->idx <= STACKSIZE) {
        st->data[st->idx] = (stack_element_t *)malloc(sizeof(sizeof(stack_element_t)));
        st->data[st->idx]->ptr = malloc(size);
        st->data[st->idx]->b_size = size;
        memmove(st->data[st->idx]->ptr, data, size);
        st->idx++;
        return 0;
    }

    return -1;
}

void *pop(m_stack_t *st) {

    void *res;

    if(st->idx > 0) {
        res = malloc(st->data[st->idx-1]->b_size);
        memmove(res, st->data[st->idx-1]->ptr, st->data[st->idx-1]->b_size);
        free(st->data[st->idx-1]->ptr);
        free(st->data[st->idx-1]);
        st->idx--;
        return res;
    }
    return (void *)NULL;
}

// NF_QUEUE operations
response_t create_queue(int q_num, q_data_t **q_data, thread_data_t *data) {

    response_t r;
    int cq_idx;

    if(get_queue_idx(q_num, q_data) == -1 && (cq_idx = get_free_idx(q_data)) >=0 ) {

        q_data[cq_idx]->h = nfq_open();

        if (!q_data[cq_idx]->h)
            syslog(LOG_ERR, "[%d] error during nfq_open()\n\r", data->idx);

        if (nfq_bind_pf(q_data[cq_idx]->h, AF_INET) < 0)
            syslog(LOG_ERR, "[%d] error during nfq_bind_pf()\n\r", data->idx);

        q_data[cq_idx]->qh = nfq_create_queue(q_data[cq_idx]->h, q_num, &cb, NULL);

        syslog(LOG_NOTICE,"[%d] q_num: %d\n\r", data->idx, q_num);

        if (!q_data[cq_idx]->qh) {
            r.cs = erl_mk_atom("error");
            r.rsp = erl_mk_estring("failed to create queue", strlen("failed to create queue"));
        } else {
            q_data[cq_idx]->q_num = q_num;
            r.cs = erl_mk_atom("ok");
            r.rsp = erl_mk_atom("ok");
        }

    } else {
        r.cs = erl_mk_atom("error");
        r.rsp = erl_mk_estring("queue in use", strlen("queue in use"));
    }
    return r;
}

response_t destroy_queue(int q_num, q_data_t **q_data, thread_data_t *data) {

    int cq_idx;
    response_t r;

    syslog(LOG_NOTICE,"[%d] q_num: %d\n\r", data->idx, q_num);

    if((cq_idx = get_queue_idx(q_num, q_data)) >= 0) {

        if(q_data[cq_idx]->pid > 0)
            pthread_cancel(q_data[cq_idx]->thread);

        nfq_destroy_queue(q_data[cq_idx]->qh);
        nfq_unbind_pf(q_data[cq_idx]->h, AF_INET);
        nfq_close(q_data[cq_idx]->h);

        q_data[cq_idx]->pid = 0;
        q_data[cq_idx]->q_num = -1;
        q_data[cq_idx]->qh = NULL;
        q_data[cq_idx]->h = NULL;

        r.cs = erl_mk_atom("ok");
        r.rsp = erl_mk_atom("ok");

    } else {
        r.cs = erl_mk_atom("error");
        r.rsp = erl_mk_estring("no such queue", strlen("no such queue"));
    }
    return r;
}

response_t queue_list(q_data_t **q_data) {

    int temp = 0, i;
    response_t r;
    ETERM **list;

    for(i = 0; i < MAX_Q_NUMS; i++)
        if(q_data[i]->q_num >=0) temp++;

    r.cs = erl_mk_atom("ok");

    if(temp > 0 && (list = calloc(temp, sizeof(ETERM *))) != NULL) {

        temp = 0;
        for(i = 0; i < MAX_Q_NUMS; i++) {
            if(q_data[i]->q_num >= 0 ) {
                list[temp] = erl_mk_int(q_data[i]->q_num);
                temp++;
            }
        }

        r.rsp = erl_mk_list(list, temp);

        for(i = 0; i < temp; i++)
            erl_free_term(list[i]);
        free(list);

    } else {
        r.rsp = erl_mk_empty_list();
    }
    return r;
}

static response_t set_mode(int t_mode, ETERM *tuplep, q_data_t **q_data, thread_data_t *data) {

    int cq_idx, q_num;
    ETERM *arg;
    u_int8_t mode = NFQNL_COPY_NONE;
    response_t r;

    if(erl_size(tuplep) > 2) {

        arg = erl_element(3, tuplep);
        q_num = ERL_INT_VALUE(arg);
        erl_free_term(arg);

        syslog(LOG_NOTICE,"[%d] q_num: %d, mode: %d\n\r", data->idx, q_num, t_mode);

        if((cq_idx = get_queue_idx(q_num, q_data)) >= 0) {
            switch(t_mode) {
            case 0:
                mode = NFQNL_COPY_NONE;
                break;
            case 1:
                mode = NFQNL_COPY_META;
                break;
            case 2:
                mode = NFQNL_COPY_PACKET;
                break;
            default:
                mode = NFQNL_COPY_NONE;
                break;
            }

            if (nfq_set_mode(q_data[cq_idx]->qh, mode, 0xffff) < 0) {
                r.cs = erl_mk_atom("error");
                r.rsp = erl_mk_estring("failed to set mode", strlen("failed to set mode"));
            } else {
                r.cs = erl_mk_atom("ok");
                r.rsp = erl_mk_atom("ok");
            }

        } else {
            r.cs = erl_mk_atom("error");
            r.rsp = erl_mk_estring("no such queue", strlen("no such queue"));
        }

    } else {
        r.cs = erl_mk_atom("error");
        r.rsp = erl_mk_estring("argument missing", strlen("argument missing"));
    }
    return r;
}

response_t set_queue_len(int len, ETERM *tuplep, q_data_t **q_data, thread_data_t *data) {

    int q_num, cq_idx;
    ETERM *arg;
    response_t r;

    if(erl_size(tuplep) > 2) {

        arg = erl_element(3, tuplep);
        q_num = ERL_INT_VALUE(arg);
        erl_free_term(arg);

        syslog(LOG_NOTICE,"[%d] q_num: %d, len: %d\n\r", data->idx, q_num, len);

        if((cq_idx = get_queue_idx(q_num, q_data)) >= 0) {

            if(nfq_set_queue_maxlen(q_data[cq_idx]->qh, len) >= 0) {
                r.cs = erl_mk_atom("ok");
                r.rsp = erl_mk_atom("ok");
            } else {
                r.cs = erl_mk_atom("error");
                r.rsp = erl_mk_estring("failed to set queue max length", strlen("failed to set queue max length"));
            }

        } else {
            r.cs = erl_mk_atom("error");
            r.rsp = erl_mk_estring("no such queue", strlen("no such queue"));
        }

    } else {
        r.cs = erl_mk_atom("error");
        r.rsp = erl_mk_estring("argument missing", strlen("argument missing"));
    }
    return r;
}

response_t read_pkt_start(int q_num, q_data_t **q_data, thread_data_t *data) {

    int cq_idx;
    response_t r;
    pthread_attr_t thread_attrs;

    if((cq_idx = get_queue_idx(q_num, q_data)) >= 0) {

        if(q_data[cq_idx]->pid <= 0) {

            pthread_attr_init(&thread_attrs);
            pthread_attr_setdetachstate(&thread_attrs, PTHREAD_CREATE_DETACHED);

            if (pthread_create(&(q_data[cq_idx]->thread), &thread_attrs, read_queue_loop, q_data[cq_idx])) {
                r.cs = erl_mk_atom("error");
                r.rsp = erl_mk_estring("failure to create thread", strlen("failure to create thread"));
            } else {
                r.cs = erl_mk_atom("ok");
                r.rsp = erl_mk_atom("ok");
            }

            pthread_attr_destroy(&thread_attrs);
        } else {
            r.cs = erl_mk_atom("error");
            r.rsp = erl_mk_estring("thread already started", strlen("thread already started"));
        }

    } else {
        r.cs = erl_mk_atom("error");
        r.rsp = erl_mk_estring("no such queue", strlen("no such queue"));
    }
    return r;
}

response_t read_pkt_stop(int q_num, q_data_t **q_data, thread_data_t *data) {
    int cq_idx;
    response_t r;

    if((cq_idx = get_queue_idx(q_num, q_data)) >= 0) {
        if(q_data[cq_idx]->pid > 0) {
            if(!pthread_cancel(q_data[cq_idx]->thread)) {
                q_data[cq_idx]->pid = 0;
                r.cs = erl_mk_atom("ok");
                r.rsp = erl_mk_atom("ok");
            } else {
                r.cs = erl_mk_atom("error");
                r.rsp = erl_mk_estring("failed to stop thread", strlen("failed to stop thread"));
            }

        } else {
            r.cs = erl_mk_atom("error");
            r.rsp = erl_mk_estring("no such thread", strlen("no such thread"));
        }
    } else {
        r.cs = erl_mk_atom("error");
        r.rsp = erl_mk_estring("no such queue", strlen("no such queue"));
    }
    return r;
}
