#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t message_type_net;
    uint16_t payload_len_net; 
} __attribute__((packed)) message_header_t;

enum {
    MSG_JOIN    = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_ERROR   = 4,

    MSG_INPUT = 10,
    MSG_STATE = 11
};

typedef enum {
    DIR_UP = 0,
    DIR_RIGHT = 1,
    DIR_DOWN = 2,
    DIR_LEFT = 3
} direction_t;

typedef struct {
    uint8_t direction; //smer
} __attribute__((packed)) input_message_t;

typedef struct {
    uint32_t tick_counter_net;
} __attribute__((packed)) state_message_t;


int send_all_bytes(int socket_fd, const void *buffer, size_t byte_count);
int recv_all_bytes(int socket_fd, void *buffer, size_t byte_count);

int send_message(int socket_fd, uint16_t message_type_host, const void *payload, uint16_t payload_len_host);
int recv_message_header(int socket_fd, message_header_t *out_header_net);

#endif

