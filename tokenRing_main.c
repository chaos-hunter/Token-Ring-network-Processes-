/* David Entonu
 * The program simulates a Token Ring LAN by forking off a process
 * for each LAN node, that communicate via shared memory, instead
 * of network cables. To keep the implementation simple, it jiggles
 * out bytes instead of bits.
 *
 * It keeps a count of packets sent and received for each node.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

#include "tokenRing.h"

int main(int argc, char *argv[]) {
    key_t key;
    int shmId, semId;
    struct shared_ring *shm_ptr;
    int i;
    pid_t pid;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <keynum>\n", argv[0]);
        exit(1);
    }
    long keynum = atol(argv[1]);
    key = (key_t)(keynum << 9);

    /* Create shared memory */
    shmId = shmget(key, sizeof(struct shared_ring), IPC_CREAT | IPC_EXCL | 0600);
    int created = 1;
    if (shmId < 0) {
        if (errno == EEXIST) {
            created = 0;
            shmId = shmget(key, sizeof(struct shared_ring), 0);
            if (shmId < 0) {
                perror("shmget attach");
                exit(1);
            }
        } else {
            perror("shmget");
            exit(1);
        }
    }
    shm_ptr = (struct shared_ring *)shmat(shmId, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("shmat");
        exit(1);
    }
    semId = semget(key, NUM_EDGE_SEMS, IPC_CREAT | 0600);
    if (semId < 0) {
        perror("semget");
        exit(1);
    }
    /* Initialize each EMPTY semaphore to 1 and each FULL semaphore to 0 */
    for (i = 0; i < N_NODES; i++) {
        union semun {
            int val;
        } sem_arg;
        sem_arg.val = 1;
        if (semctl(semId, EMPTY_SEM(i), SETVAL, sem_arg) < 0) {
            perror("semctl EMPTY init");
            exit(1);
        }
        sem_arg.val = 0;
        if (semctl(semId, FULL_SEM(i), SETVAL, sem_arg) < 0) {
            perror("semctl FULL init");
            exit(1);
        }
    }

    /* Initialize shared memory fields */
    memset(shm_ptr, 0, sizeof(struct shared_ring));
    for (i = 0; i < N_NODES; i++) {
        shm_ptr->pending_valid[i] = 0;
        shm_ptr->sent_count[i] = 0;
        shm_ptr->received_count[i] = 0;
    }
    shm_ptr->total_packets_transmitted = 0;
    shm_ptr->terminate = 0;

    sem_wait_op(semId, EMPTY_SEM(N_NODES - 1));
    shm_ptr->ring[N_NODES - 1] = TOKEN;
    sem_signal_op(semId, FULL_SEM(N_NODES - 1));

    /* Fork N_NODES child processes (each representing a node) */
    for (i = 0; i < N_NODES; i++) {
        pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            node_main(i, shm_ptr, semId);
            exit(0); 
        }
    }

    /* Generate random packets for nodes to send */
    srand(time(NULL));
    int packets_generated = 0;
    while (packets_generated < SIM_PACKETS) {
        int node = random_between(0, N_NODES - 1);
        if (shm_ptr->pending_valid[node] == 0) {
            struct data_pkt pkt;
            pkt.token_flag = 0x00; 

            int dest;
            do {
                dest = random_between(0, N_NODES - 1);
            } while (dest == node);
            pkt.to = (char)dest;
            pkt.from = (char)node;
            pkt.length = (unsigned char)random_between(1, 10); /* use length 1-10 for demo */
            for (i = 0; i < pkt.length; i++) {
                pkt.data[i] = 'A' + random_between(0, 25);
            }
            memcpy(&shm_ptr->pending[node], &pkt, sizeof(pkt));
            shm_ptr->pending_valid[node] = 1;
            packets_generated++;
            printf("Parent: Generated packet from node %d to node %d (length %d)\n",
                   node, dest, pkt.length);
        }
        usleep(500000); 
    }

    /* Wait until all generated packets have been transmitted */
    while (shm_ptr->total_packets_transmitted < SIM_PACKETS) {
        usleep(100000);
    }
    shm_ptr->terminate = 1;
    for (i = 0; i < N_NODES; i++) {
        wait(NULL);
    }

    /* Print simulation statistics */
    printf("Simulation complete. Total packets transmitted: %d\n",
           shm_ptr->total_packets_transmitted);
    for (i = 0; i < N_NODES; i++) {
        printf("Node %d: Sent %d, Received %d\n",
               i, shm_ptr->sent_count[i], shm_ptr->received_count[i]);
    }

    /* Cleanup shared memory and semaphores */
    shmdt(shm_ptr);
    shmctl(shmId, IPC_RMID, NULL);
    semctl(semId, 0, IPC_RMID);

    return 0;
}
