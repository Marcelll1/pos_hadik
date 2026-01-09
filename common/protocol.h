
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t message_type_net;
    uint16_t payload_len_net;
} __attribute__((packed)) message_header_t;

enum {
    MSG_JOIN      = 1,
    MSG_WELCOME   = 2,
    MSG_TEXT      = 3,
    MSG_ERROR     = 4,

    MSG_INPUT     = 10,
    MSG_STATE     = 11,
    MSG_SHUTDOWN  = 12,

    MSG_PAUSE     = 13,
    MSG_LEAVE     = 14,
    MSG_RESPAWN   = 15,

    MSG_GAME_OVER = 16
};

typedef enum {
    DIR_UP = 0,
    DIR_RIGHT = 1,
    DIR_DOWN = 2,
    DIR_LEFT = 3
} direction_t;

typedef struct {
    uint8_t direction;
} __attribute__((packed)) input_message_t;

#define STATE_MAX_WIDTH   80
#define STATE_MAX_HEIGHT  40
#define STATE_MAX_CELLS   (STATE_MAX_WIDTH * STATE_MAX_HEIGHT)

#define STATE_MAX_PLAYERS 64
#define STATE_NAME_MAX    32
#define PLAYER_NAME_MAX   STATE_NAME_MAX

typedef enum {
    GAME_MODE_STANDARD = 0,
    GAME_MODE_TIMED    = 1
} game_mode_t;

typedef struct {
    uint8_t is_used;
    uint8_t has_joined;
    uint8_t is_alive;
    uint8_t is_paused;

    uint16_t score_net;
    uint8_t name[STATE_NAME_MAX];
} __attribute__((packed)) state_player_info_t;

typedef struct {
    uint32_t tick_counter_net;

    uint8_t width;
    uint8_t height;
    uint8_t game_mode;
    uint8_t world_type;

    uint32_t elapsed_ms_net;
    uint32_t remaining_ms_net;

    state_player_info_t players[STATE_MAX_PLAYERS];
    uint8_t cells[STATE_MAX_CELLS];
} __attribute__((packed)) state_message_t;

typedef struct {
    uint8_t has_joined;
    uint8_t reserved0;
    uint16_t score_net;
    uint32_t snake_time_ms_net;
    uint8_t name[STATE_NAME_MAX];
} __attribute__((packed)) game_over_player_entry_t;

typedef struct {
    uint32_t elapsed_ms_net;
    uint8_t player_count;
    uint8_t reserved1;
    uint16_t reserved2;
    game_over_player_entry_t players[STATE_MAX_PLAYERS];
} __attribute__((packed)) game_over_message_t;

int send_all_bytes(int socket_fd, const void *buffer, size_t byte_count);
int recv_all_bytes(int socket_fd, void *buffer, size_t byte_count);

int send_message(int socket_fd, uint16_t message_type_host, const void *payload, uint16_t payload_len_host);
int recv_message_header(int socket_fd, message_header_t *out_header_net);

static inline int send_msg(int socket_fd, uint16_t message_type_host, const void *payload, uint16_t payload_len_host) {
    return send_message(socket_fd, message_type_host, payload, payload_len_host);
}

static inline int recv_hdr(int socket_fd, message_header_t *out_header_net) {
    return recv_message_header(socket_fd, out_header_net);
}

static inline int recv_all(int socket_fd, void *buffer, size_t byte_count) {
    return recv_all_bytes(socket_fd, buffer, byte_count);
}

#endif

