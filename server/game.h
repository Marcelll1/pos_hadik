#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include "../common/protocol.h"

#define GAME_MAX_PLAYERS   64
#define GAME_MAX_NAME_LEN  32
#define GAME_MAX_SNAKE_LEN 64

typedef enum {
    WORLD_EMPTY = 0,
    WORLD_FILE  = 1
} world_type_t;

typedef struct {
    uint8_t x;
    uint8_t y;
} game_pos_t;

typedef struct {
    int is_active;
    int has_joined;
    int is_alive;
    int is_paused;

    char player_name[GAME_MAX_NAME_LEN];

    uint16_t score;

    direction_t current_direction;
    direction_t requested_direction;

    uint16_t snake_len;
    game_pos_t snake_body[GAME_MAX_SNAKE_LEN];

    uint64_t freeze_until_ms;
    uint64_t snake_alive_start_ms;
    uint64_t snake_time_ms;
} game_player_t;

typedef struct {
    uint32_t tick_counter;

    uint8_t map_width;
    uint8_t map_height;

    world_type_t world_type;
    uint8_t obstacle_map[STATE_MAX_CELLS];

    uint8_t food_count;
    game_pos_t food_positions[GAME_MAX_PLAYERS];

    game_mode_t game_mode;

    uint64_t start_time_ms;
    uint64_t timed_end_ms;
    uint64_t last_no_snakes_ms;
    int should_terminate;

    uint64_t global_freeze_until_ms;

    int global_pause_active;
    char global_pause_owner_name[GAME_MAX_NAME_LEN];

    game_player_t players[GAME_MAX_PLAYERS];
} game_state_t;

int game_load_map_from_file(game_state_t *game_state, const char *path);

void game_init(game_state_t *game_state,
               uint8_t map_width,
               uint8_t map_height,
               game_mode_t mode,
               uint32_t timed_duration_ms,
               world_type_t world_type);

void game_mark_client_active(game_state_t *game_state, int player_slot);
void game_mark_client_inactive_keep_or_clear(game_state_t *game_state, int player_slot, int keep_player_state);

int  game_find_paused_player_by_name(const game_state_t *game_state, const char *player_name);

int  game_join_new_player(game_state_t *game_state, int player_slot, const char *player_name, uint64_t now_ms);
int  game_resume_player(game_state_t *game_state, int player_slot, uint64_t now_ms);
int  game_respawn_player(game_state_t *game_state, int player_slot, uint64_t now_ms);

void game_handle_input(game_state_t *game_state, int player_slot, direction_t direction);

void game_handle_pause(game_state_t *game_state, int player_slot);
void game_handle_leave(game_state_t *game_state, int player_slot, uint64_t now_ms);

void game_tick(game_state_t *game_state, uint64_t now_ms);

void game_build_ascii_map(const game_state_t *game_state, uint8_t *out_cells, size_t out_cells_len);

uint32_t game_get_elapsed_ms(const game_state_t *game_state, uint64_t now_ms);
uint32_t game_get_remaining_ms(const game_state_t *game_state, uint64_t now_ms);

#endif


