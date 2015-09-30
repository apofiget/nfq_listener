#define BUFSIZE 100
#define STACKSIZE 1000
#define MAX_Q_NUMS 100

typedef struct nfq_q_handle nfq_q_h_t;
typedef struct nfq_handle nfq_h_t;

// Stack element
typedef struct _stack_element_t {
    int b_size;              // element size
    void *ptr;               // pointer to element data
} stack_element_t;

// Stack storage
typedef struct _stack_struct_t {
    int idx;                 // current index
    stack_element_t *data[]; //array of pointers to data stack elements
} m_stack_t;

typedef struct _q_data_t {
    nfq_h_t *h;             // Library handler
    nfq_q_h_t *qh;          // Queue handler
    int q_num;              // Queue number
    pthread_t thread;       // Packet process thread
    pid_t pid;              // Packet read thread system PID
} q_data_t;

typedef struct _thread_data_t {
    int fd;
    unsigned int idx;
    char *node;
} thread_data_t;

typedef struct _nfq_node_response_t {
    ETERM *cs;             // Call state
    ETERM *rsp;            // Call response
} response_t;
