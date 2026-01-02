#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t message_type_net;  // network byte order
    uint16_t payload_len_net;   // network byte order
} __attribute__((packed)) message_header_t;

enum {
    MSG_JOIN    = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_ERROR   = 4,

    MSG_INPUT   = 10,
    MSG_STATE   = 11
};

typedef enum {
    DIR_UP = 0,
    DIR_RIGHT = 1,
    DIR_DOWN = 2,
    DIR_LEFT = 3
} direction_t;

typedef struct {
    uint8_t direction; // direction_t
} __attribute__((packed)) input_message_t;


#define STATE_MAP_WIDTH  40
#define STATE_MAP_HEIGHT 20
#define STATE_MAP_CELLS  (STATE_MAP_WIDTH * STATE_MAP_HEIGHT)

typedef struct {
    uint32_t tick_counter_net; // network byte order
    uint8_t width;
    uint8_t height;
    uint8_t cells[STATE_MAP_CELLS]; // ASCII znaky bez \n
} __attribute__((packed)) state_message_t;

int send_all_bytes(int socket_fd, const void *buffer, size_t byte_count);
int recv_all_bytes(int socket_fd, void *buffer, size_t byte_count);

int send_message(int socket_fd, uint16_t message_type_host, const void *payload, uint16_t payload_len_host);
int recv_message_header(int socket_fd, message_header_t *out_header_net);

#endif

