#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include "../common/protocol.h"

#define GAME_MAX_PLAYERS   64
#define GAME_MAX_NAME_LEN  32
#define GAME_MAX_SNAKE_LEN 64

typedef struct {
    uint8_t x;
    uint8_t y;
} game_pos_t;

typedef struct {
    int is_active;                 // slot obsadený (ma klienta)
    int has_joined;                // poslal JOIN
    int is_alive;
    char player_name[GAME_MAX_NAME_LEN];

    uint16_t score;
    direction_t current_direction; // smer kam sa hybe had
    direction_t requested_direction; // posledny klientov inpuy

    uint16_t snake_len;
    game_pos_t snake_body[GAME_MAX_SNAKE_LEN]; // [0] = hlava
} game_player_t;

typedef struct {
    uint32_t tick_counter;

    uint8_t map_width;
    uint8_t map_height;

    int has_food;
    game_pos_t food_pos;

    game_player_t players[GAME_MAX_PLAYERS];
} game_state_t;

void game_init(game_state_t *game_state, uint8_t map_width, uint8_t map_height);

void game_mark_client_active(game_state_t *game_state, int player_slot);
void game_mark_client_inactive(game_state_t *game_state, int player_slot);

int  game_handle_join(game_state_t *game_state, int player_slot, const char *player_name);
void game_handle_input(game_state_t *game_state, int player_slot, direction_t direction);

void game_tick(game_state_t *game_state);

void game_build_ascii_map(const game_state_t *game_state, uint8_t *out_cells);

#endif

