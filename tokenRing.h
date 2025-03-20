#ifndef TOKEN_RING_H
#define TOKEN_RING_H

#define N_NODES 4
#define SIM_PACKETS 10
#define TOKEN 0xFF
#define NUM_EDGE_SEMS (2 * N_NODES)
#define EMPTY_SEM(i) (2 * (i))
#define FULL_SEM(i) (2 * (i) + 1)

struct data_pkt {
    char token_flag;
    char to;
    char from;
    unsigned char length;
    char data[250];
};

struct shared_ring {
    unsigned char ring[N_NODES];
    struct data_pkt pending[N_NODES];
    int pending_valid[N_NODES];
    int sent_count[N_NODES];
    int received_count[N_NODES];
    int total_packets_transmitted;
    int terminate;
};

typedef struct {
    int shmId;
    int semId;
    struct shared_ring *shm_ptr;
} TokenRingData;

/* Function declarations */
int sem_timed_wait_op(int semId, int sem_index, int timeout_secs);
int sem_wait_op(int semId, int sem_index);
void sem_signal_op(int semId, int sem_index);
int random_between(int min, int max);
void node_main(int node_id, struct shared_ring *shm_ptr, int semId);
TokenRingData *setupSystem();
int runSimulation(TokenRingData *simulationData, int numPackets);
int cleanupSystem(TokenRingData *simulationData);

#endif // TOKEN_RING_H