#include "game.h"

#include <string.h>
#include <stdlib.h>

static int is_opposite_direction(direction_t a, direction_t b) {
    return (a == DIR_UP && b == DIR_DOWN) ||
           (a == DIR_DOWN && b == DIR_UP) ||
           (a == DIR_LEFT && b == DIR_RIGHT) ||
           (a == DIR_RIGHT && b == DIR_LEFT);
}

static game_pos_t next_position(game_pos_t head, direction_t dir) {
    game_pos_t p = head;
    switch (dir) {
        case DIR_UP:    p.y--; break;
        case DIR_RIGHT: p.x++; break;
        case DIR_DOWN:  p.y++; break;
        case DIR_LEFT:  p.x--; break;
        default: break;
    }
    return p;
}

static int is_wall(const game_state_t *game_state, game_pos_t p) {
    return (p.x == 0 || p.y == 0 ||
            p.x == (uint8_t)(game_state->map_width - 1) ||
            p.y == (uint8_t)(game_state->map_height - 1));
}

static int is_food_at(const game_state_t *game_state, game_pos_t p, int *out_food_index) {
    for (uint8_t i = 0; i < game_state->food_count; i++) {
        if (game_state->food_positions[i].x == p.x && game_state->food_positions[i].y == p.y) {
            if (out_food_index) *out_food_index = (int)i;
            return 1;
        }
    }
    return 0;
}

static void remove_food_at(game_state_t *game_state, int food_index) {
    if (food_index < 0 || food_index >= (int)game_state->food_count) return;
    uint8_t last = (uint8_t)(game_state->food_count - 1);
    game_state->food_positions[food_index] = game_state->food_positions[last];
    game_state->food_count = last;
}

static int is_occupied_except_tail(const game_state_t *game_state, int player_slot, game_pos_t p, int will_grow) {
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &game_state->players[s];
        if (!pl->has_joined) continue;
        if (!pl->is_alive) continue;

        for (uint16_t i = 0; i < pl->snake_len; i++) {
            if (s == player_slot && !will_grow && i == (uint16_t)(pl->snake_len - 1)) continue;
            if (pl->snake_body[i].x == p.x && pl->snake_body[i].y == p.y) return 1;
        }
    }
    return 0;
}

static int count_alive_snakes(const game_state_t *game_state) {
    int count = 0;
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &game_state->players[s];
        if (pl->has_joined && pl->is_alive) count++;
    }
    return count;
}

static int find_free_cell(const game_state_t *game_state, game_pos_t *out_pos) {
    uint8_t min_x = 3;
    uint8_t max_x = (uint8_t)(game_state->map_width - 2);
    uint8_t min_y = 1;
    uint8_t max_y = (uint8_t)(game_state->map_height - 2);

    if (max_x <= min_x || max_y <= min_y) return -1;

    for (int attempts = 0; attempts < 4000; attempts++) {
        uint8_t x = (uint8_t)(min_x + (rand() % (max_x - min_x + 1)));
        uint8_t y = (uint8_t)(min_y + (rand() % (max_y - min_y + 1)));
        game_pos_t p = {x, y};

        if (is_wall(game_state, p)) continue;
        if (is_occupied_except_tail(game_state, -1, p, 1)) continue;
        if (is_food_at(game_state, p, NULL)) continue;

        *out_pos = p;
        return 0;
    }
    return -1;
}

static void ensure_food_count(game_state_t *game_state) {
    int alive = count_alive_snakes(game_state);
    if (alive < 0) alive = 0;
    if (alive > GAME_MAX_PLAYERS) alive = GAME_MAX_PLAYERS;

    while (game_state->food_count < (uint8_t)alive) {
        game_pos_t p;
        if (find_free_cell(game_state, &p) != 0) break;
        game_state->food_positions[game_state->food_count++] = p;
    }

    // ak hady ubudli, zredukuj (najjednoduchsie: orezi)
    while (game_state->food_count > (uint8_t)alive) {
        game_state->food_count--;
    }
}

void game_init(game_state_t *game_state, uint8_t map_width, uint8_t map_height, game_mode_t mode, uint32_t timed_duration_ms) {
    memset(game_state, 0, sizeof(*game_state));
    game_state->map_width = map_width;
    game_state->map_height = map_height;

    game_state->food_count = 0;

    game_state->game_mode = mode;
    game_state->start_time_ms = 0;
    game_state->timed_end_ms = 0;
    game_state->last_no_snakes_ms = 0;
    game_state->should_terminate = 0;

    for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
        game_player_t *pl = &game_state->players[i];
        pl->is_active = 0;
        pl->has_joined = 0;
        pl->is_alive = 0;
        pl->is_paused = 0;
        pl->score = 0;
        pl->snake_len = 0;
        pl->current_direction = DIR_RIGHT;
        pl->requested_direction = DIR_RIGHT;
        pl->player_name[0] = '\0';
        pl->freeze_until_ms = 0;
    }
    
    srand((unsigned) (uintptr_t)game_state);

    (void)timed_duration_ms;
}

void game_mark_client_active(game_state_t *game_state, int player_slot) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;
    game_state->players[player_slot].is_active = 1;
}

void game_mark_client_inactive_keep_or_clear(game_state_t *game_state, int player_slot, int keep_player_state) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &game_state->players[player_slot];
    pl->is_active = 0;

    if (keep_player_state) {
        return;
    }

    pl->has_joined = 0;
    pl->is_alive = 0;
    pl->is_paused = 0;
    pl->snake_len = 0;
    pl->score = 0;
    pl->player_name[0] = '\0';
    pl->current_direction = DIR_RIGHT;
    pl->requested_direction = DIR_RIGHT;
    pl->freeze_until_ms = 0;
}

int game_find_paused_player_by_name(const game_state_t *game_state, const char *player_name) {
    for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
        const game_player_t *pl = &game_state->players[i];
        if (!pl->has_joined) continue;
        if (!pl->is_paused) continue;
        if (strncmp(pl->player_name, player_name, GAME_MAX_NAME_LEN) == 0) return i;
    }
    return -1;
}

int game_join_new_player(game_state_t *game_state, int player_slot, const char *player_name, uint64_t now_ms) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return -1;

    if (game_state->start_time_ms == 0) {
        game_state->start_time_ms = now_ms;
        if (game_state->game_mode == GAME_MODE_TIMED) {
        }
    }

    game_player_t *pl = &game_state->players[player_slot];
    if (!pl->is_active) return -1;

    strncpy(pl->player_name, player_name, GAME_MAX_NAME_LEN - 1);
    pl->player_name[GAME_MAX_NAME_LEN - 1] = '\0';

    pl->has_joined = 1;
    pl->is_alive = 1;
    pl->is_paused = 0;
    pl->score = 0;
    pl->freeze_until_ms = 0;

    pl->current_direction = DIR_RIGHT;
    pl->requested_direction = DIR_RIGHT;

    pl->snake_len = 3;

    game_pos_t head;
    if (find_free_cell(game_state, &head) != 0) return -1;

    pl->snake_body[0] = head;
    pl->snake_body[1] = (game_pos_t){ (uint8_t)(head.x - 1), head.y };
    pl->snake_body[2] = (game_pos_t){ (uint8_t)(head.x - 2), head.y };

    ensure_food_count(game_state);
    return 0;
}

int game_resume_player(game_state_t *game_state, int player_slot, uint64_t now_ms) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return -1;
    game_player_t *pl = &game_state->players[player_slot];
    if (!pl->has_joined) return -1;

    pl->is_paused = 0;
    pl->freeze_until_ms = now_ms + 3000ULL;
    pl->is_alive = pl->is_alive ? 1 : 0;

    ensure_food_count(game_state);
    return 0;
}

void game_handle_input(game_state_t *game_state, int player_slot, direction_t direction) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &game_state->players[player_slot];
    if (!pl->has_joined || !pl->is_alive) return;
    if (pl->is_paused) return;

    if (is_opposite_direction(pl->current_direction, direction)) return;
    pl->requested_direction = direction;
}

void game_handle_pause(game_state_t *game_state, int player_slot) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;
    game_player_t *pl = &game_state->players[player_slot];
    if (!pl->has_joined) return;

    pl->is_paused = 1;
}

void game_handle_leave(game_state_t *game_state, int player_slot) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_mark_client_inactive_keep_or_clear(game_state, player_slot, 0);
    ensure_food_count(game_state);
}

static void update_game_termination(game_state_t *game_state, uint64_t now_ms) {
    int alive = count_alive_snakes(game_state);

    if (game_state->game_mode == GAME_MODE_TIMED) {
        if (game_state->timed_end_ms != 0 && now_ms >= game_state->timed_end_ms) {
            game_state->should_terminate = 1;
        }
        return;
    }

    // STANDARD
    if (alive == 0) {
        if (game_state->last_no_snakes_ms == 0) {
            game_state->last_no_snakes_ms = now_ms;
        } else {
            if (now_ms - game_state->last_no_snakes_ms >= 10000ULL) {
                game_state->should_terminate = 1;
            }
        }
    } else {
        game_state->last_no_snakes_ms = 0;
    }
}

void game_tick(game_state_t *game_state, uint64_t now_ms) {
    if (game_state->start_time_ms == 0) {
        game_state->start_time_ms = now_ms;
    }

    game_state->tick_counter++;

    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        game_player_t *pl = &game_state->players[s];
        if (!pl->has_joined || !pl->is_alive) continue;
        if (pl->is_paused) continue;
        pl->current_direction = pl->requested_direction;
    }

    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        game_player_t *pl = &game_state->players[s];
        if (!pl->has_joined || !pl->is_alive) continue;
        if (pl->is_paused) continue;
        if (pl->freeze_until_ms != 0 && now_ms < pl->freeze_until_ms) continue;

        game_pos_t current_head = pl->snake_body[0];
        game_pos_t new_head = next_position(current_head, pl->current_direction);

        int eaten_food_index = -1;
        int will_grow = is_food_at(game_state, new_head, &eaten_food_index);

        if (is_wall(game_state, new_head)) {
            pl->is_alive = 0;
            continue;
        }

        if (is_occupied_except_tail(game_state, s, new_head, will_grow)) {
            pl->is_alive = 0;
            continue;
        }

        if (will_grow) {
            if (pl->snake_len < GAME_MAX_SNAKE_LEN) pl->snake_len++;
            pl->score++;
            remove_food_at(game_state, eaten_food_index);
        }

        for (uint16_t i = (uint16_t)(pl->snake_len - 1); i > 0; i--) {
            pl->snake_body[i] = pl->snake_body[i - 1];
        }
        pl->snake_body[0] = new_head;
    }

    ensure_food_count(game_state);
    update_game_termination(game_state, now_ms);
}

static char player_head_char(int slot) {
    if (slot >= 0 && slot < 26) return (char)('A' + slot);
    slot -= 26;
    if (slot >= 0 && slot < 10) return (char)('0' + slot);
    return '@';
}

static char player_body_char(int slot) {
    if (slot >= 0 && slot < 26) return (char)('a' + slot);
    slot -= 26;
    if (slot >= 0 && slot < 10) return (char)('0' + slot);
    return '+';
}

void game_build_ascii_map(const game_state_t *game_state, uint8_t *out_cells) {
    for (int i = 0; i < STATE_MAP_CELLS; i++) out_cells[i] = (uint8_t)' ';

    // steny
    for (uint8_t x = 0; x < game_state->map_width; x++) {
        out_cells[0 * game_state->map_width + x] = (uint8_t)'#';
        out_cells[(game_state->map_height - 1) * game_state->map_width + x] = (uint8_t)'#';
    }
    for (uint8_t y = 0; y < game_state->map_height; y++) {
        out_cells[y * game_state->map_width + 0] = (uint8_t)'#';
        out_cells[y * game_state->map_width + (game_state->map_width - 1)] = (uint8_t)'#';
    }

    // food
    for (uint8_t i = 0; i < game_state->food_count; i++) {
        game_pos_t f = game_state->food_positions[i];
        out_cells[f.y * game_state->map_width + f.x] = (uint8_t)'*';
    }

    // hady
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &game_state->players[s];
        if (!pl->has_joined) continue;

        if (!pl->is_alive && pl->snake_len > 0) {
            for (uint16_t i = 0; i < pl->snake_len; i++) {
                game_pos_t p = pl->snake_body[i];
                out_cells[p.y * game_state->map_width + p.x] = (uint8_t)'x';
            }
            continue;
        }

        if (pl->snake_len > 0) {
            game_pos_t head = pl->snake_body[0];
            out_cells[head.y * game_state->map_width + head.x] = (uint8_t)player_head_char(s);
        }
        for (uint16_t i = 1; i < pl->snake_len; i++) {
            game_pos_t p = pl->snake_body[i];
            out_cells[p.y * game_state->map_width + p.x] = (uint8_t)player_body_char(s);
        }
    }
}

uint32_t game_get_elapsed_ms(const game_state_t *game_state, uint64_t now_ms) {
    if (game_state->start_time_ms == 0) return 0;
    uint64_t diff = now_ms - game_state->start_time_ms;
    if (diff > 0xFFFFFFFFULL) diff = 0xFFFFFFFFULL;
    return (uint32_t)diff;
}

uint32_t game_get_remaining_ms(const game_state_t *game_state, uint64_t now_ms) {
    if (game_state->game_mode != GAME_MODE_TIMED) return 0;
    if (game_state->timed_end_ms == 0) return 0;
    if (now_ms >= game_state->timed_end_ms) return 0;
    uint64_t rem = game_state->timed_end_ms - now_ms;
    if (rem > 0xFFFFFFFFULL) rem = 0xFFFFFFFFULL;
    return (uint32_t)rem;
}

