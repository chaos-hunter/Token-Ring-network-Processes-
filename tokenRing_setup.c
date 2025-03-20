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

/* 
wait for up to timeout_secs seconds 
*/

int sem_timed_wait_op(int semId, int sem_index, int timeout_secs) {
    struct sembuf ops;
    ops.sem_num = sem_index;
    ops.sem_op  = -1;
    ops.sem_flg = 0;

    struct timespec ts;
    ts.tv_sec  = timeout_secs;
    ts.tv_nsec = 0;

    int ret = semtimedop(semId, &ops, 1, &ts);
    if (ret < 0) {
        if (errno == EAGAIN) {
            // Timeout occurred.
            return -1;
        } else {
            perror("semtimedop");
            exit(1);
        }
    }
    return 0;
}

/*
 Decrements (locks) a semaphore. Exits on failure.
 */
int sem_wait_op(int semId, int sem_index) {
    struct sembuf ops;
    ops.sem_num = sem_index;
    ops.sem_op  = -1;
    ops.sem_flg = 0;
    if (semop(semId, &ops, 1) < 0) {
        perror("semop wait");
        exit(1);
    }
    return 0;
}
/*
 Increments (unlocks) a semaphore. Exits on failure.
 */
void sem_signal_op(int semId, int sem_index) {
    struct sembuf ops;
    ops.sem_num = sem_index;
    ops.sem_op  = 1;
    ops.sem_flg = 0;
    if (semop(semId, &ops, 1) < 0) {
        perror("semop signal");
        exit(1);
    }
}

/* 
Return a random integer between min and max 
*/
int random_between(int min, int max) {
    return min + rand() % (max - min + 1);
}

/* 
Each node determines its incoming edge and its outgoing edge.
It loops forever, until termination is signaled,
processing one byte at a time.
*/
void node_main(int node_id, struct shared_ring *shm_ptr, int semId) {
    int incoming_edge = (node_id - 1 + N_NODES) % N_NODES;
    int outgoing_edge = node_id;
    int sending = 0;
    struct data_pkt current_pkt;
    int pkt_index = 0;

    // Buffer to store incoming packet bytes
    unsigned char pkt_buffer[20];
    int pkt_bytes_received = 0;

    while (!shm_ptr->terminate) {
        if (sending) {
            /* In sending mode: send one byte of the packet */
            int total_length = 4 + current_pkt.length;
            if (pkt_index < total_length) {
                unsigned char pkt_byte;
                if (pkt_index == 0)
                    pkt_byte = current_pkt.token_flag;  
                else if (pkt_index == 1)
                    pkt_byte = current_pkt.to;
                else if (pkt_index == 2)
                    pkt_byte = current_pkt.from;
                else if (pkt_index == 3)
                    pkt_byte = current_pkt.length;
                else
                    pkt_byte = current_pkt.data[pkt_index - 4];
    
                if (sem_timed_wait_op(semId, EMPTY_SEM(outgoing_edge), 1) < 0) {
                    if (shm_ptr->terminate)
                        break;
                    continue;
                }
                shm_ptr->ring[outgoing_edge] = pkt_byte;
                sem_signal_op(semId, FULL_SEM(outgoing_edge));
                pkt_index++;
            } else {
                shm_ptr->sent_count[node_id]++;
                shm_ptr->total_packets_transmitted++;
                sending = 0;
                if (sem_timed_wait_op(semId, EMPTY_SEM(outgoing_edge), 1) < 0) {
                    if (shm_ptr->terminate)
                        break;
                    continue;
                }
                shm_ptr->ring[outgoing_edge] = TOKEN;
                sem_signal_op(semId, FULL_SEM(outgoing_edge));
                printf("Node %d: Finished sending packet; token forwarded.\n", node_id);
            }
            /* try to receive a byte using a timed wait */
        } else {
            if (sem_timed_wait_op(semId, FULL_SEM(incoming_edge), 1) < 0) {
                if (shm_ptr->terminate)
                    break;
                continue;
            }
            // Process the received byte
            unsigned char byte = shm_ptr->ring[incoming_edge];
            sem_signal_op(semId, EMPTY_SEM(incoming_edge));
    
            if (byte == TOKEN) {
                if (shm_ptr->pending_valid[node_id]) {
                    current_pkt = shm_ptr->pending[node_id];
                    shm_ptr->pending_valid[node_id] = 0;
                    sending = 1;
                    pkt_index = 0;
                    printf("Node %d: Received token and beginning to send packet to %d (length %d)\n",
                           node_id, current_pkt.to, current_pkt.length);
                     /* No pending packet—forward the token */
                    } else {
                    if (sem_timed_wait_op(semId, EMPTY_SEM(outgoing_edge), 1) < 0) {
                        if (shm_ptr->terminate)
                            break;
                        continue;
                    }
                    shm_ptr->ring[outgoing_edge] = TOKEN;
                    sem_signal_op(semId, FULL_SEM(outgoing_edge));
                }
                /* Handle received data byte */
            } else {
                pkt_buffer[pkt_bytes_received++] = byte;
                if (pkt_bytes_received == 4 + pkt_buffer[3]) {
                    struct data_pkt received_pkt;
                    received_pkt.token_flag = pkt_buffer[0];
                    received_pkt.to = pkt_buffer[1];
                    received_pkt.from = pkt_buffer[2];
                    received_pkt.length = pkt_buffer[3];
                    memcpy(received_pkt.data, &pkt_buffer[4], received_pkt.length);

                    if (received_pkt.to == node_id && received_pkt.from != node_id) {
                        printf("Node %d: Received packet from node %d (length %d)\n",
                               node_id, received_pkt.from, received_pkt.length);
                        shm_ptr->received_count[node_id]++;
                    }

                    pkt_bytes_received = 0; 
                }

                /* Forward data byte */
                if (sem_timed_wait_op(semId, EMPTY_SEM(outgoing_edge), 1) < 0) {
                    if (shm_ptr->terminate)
                        break;
                    continue;
                }
                shm_ptr->ring[outgoing_edge] = byte;
                sem_signal_op(semId, FULL_SEM(outgoing_edge));
            }
        }
        usleep(1000); 
    }
    
    printf("Node %d terminating. Sent: %d, Received: %d\n",
           node_id, shm_ptr->sent_count[node_id], shm_ptr->received_count[node_id]);
    exit(0);
}



