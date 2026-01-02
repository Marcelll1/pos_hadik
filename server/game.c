#include "game.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

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

static int is_occupied_except_tail(const game_state_t *game_state, int player_slot, game_pos_t p, int will_grow) {
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &game_state->players[s];
        if (!pl->is_active || !pl->has_joined || !pl->is_alive) continue;

        for (uint16_t i = 0; i < pl->snake_len; i++) {
            if (s == player_slot && !will_grow && i == (uint16_t)(pl->snake_len - 1)) {
                continue;
            }
            if (pl->snake_body[i].x == p.x && pl->snake_body[i].y == p.y) return 1;
        }
    }
    return 0;
}

static int find_free_cell(const game_state_t *game_state, game_pos_t *out_pos) {
    for (int attempts = 0; attempts < 2000; attempts++) {
        uint8_t x = (uint8_t)(1 + (rand() % (game_state->map_width - 2)));
        uint8_t y = (uint8_t)(1 + (rand() % (game_state->map_height - 2)));
        game_pos_t p = {x, y};

        if (is_wall(game_state, p)) continue;
        if (is_occupied_except_tail(game_state, -1, p, 1)) continue;
        if (game_state->has_food && game_state->food_pos.x == x && game_state->food_pos.y == y) continue;

        *out_pos = p;
        return 0;
    }
    return -1;
}

static void ensure_food(game_state_t *game_state) {
    if (game_state->has_food) return;

    game_pos_t p;
    if (find_free_cell(game_state, &p) == 0) {
        game_state->food_pos = p;
        game_state->has_food = 1;
    }
}

void game_init(game_state_t *game_state, uint8_t map_width, uint8_t map_height) {
    memset(game_state, 0, sizeof(*game_state));
    game_state->map_width = map_width;
    game_state->map_height = map_height;

    srand((unsigned)time(NULL));

    for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
        game_state->players[i].is_active = 0;
        game_state->players[i].has_joined = 0;
        game_state->players[i].is_alive = 0;
        game_state->players[i].snake_len = 0;
        game_state->players[i].score = 0;
        game_state->players[i].current_direction = DIR_RIGHT;
        game_state->players[i].requested_direction = DIR_RIGHT;
        game_state->players[i].player_name[0] = '\0';
    }

    game_state->has_food = 0;
    ensure_food(game_state);
}

void game_mark_client_active(game_state_t *game_state, int player_slot) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;
    game_state->players[player_slot].is_active = 1;
}

void game_mark_client_inactive(game_state_t *game_state, int player_slot) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &game_state->players[player_slot];
    pl->is_active = 0;
    pl->has_joined = 0;
    pl->is_alive = 0;
    pl->snake_len = 0;
    pl->score = 0;
    pl->player_name[0] = '\0';
    pl->current_direction = DIR_RIGHT;
    pl->requested_direction = DIR_RIGHT;
}

int game_handle_join(game_state_t *game_state, int player_slot, const char *player_name) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return -1;

    game_player_t *pl = &game_state->players[player_slot];
    if (!pl->is_active) return -1;

    // meno
    strncpy(pl->player_name, player_name, GAME_MAX_NAME_LEN - 1);
    pl->player_name[GAME_MAX_NAME_LEN - 1] = '\0';

    // init hada: 3 clanky nahodna pozicia smer doprava
    pl->has_joined = 1;
    pl->is_alive = 1;
    pl->score = 0;

    pl->current_direction = DIR_RIGHT;
    pl->requested_direction = DIR_RIGHT;

    pl->snake_len = 3;

    game_pos_t head;
    if (find_free_cell(game_state, &head) != 0) return -1;

    pl->snake_body[0] = head;
    pl->snake_body[1] = (game_pos_t){ (uint8_t)(head.x - 1), head.y };
    pl->snake_body[2] = (game_pos_t){ (uint8_t)(head.x - 2), head.y };

    ensure_food(game_state);
    return 0;
}

void game_handle_input(game_state_t *game_state, int player_slot, direction_t direction) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &game_state->players[player_slot];
    if (!pl->is_active || !pl->has_joined || !pl->is_alive) return;

    // neda sa otocit do opacneho smeru predosleho
    if (is_opposite_direction(pl->current_direction, direction)) return;

    pl->requested_direction = direction;
}

void game_tick(game_state_t *game_state) {
    game_state->tick_counter++;
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        game_player_t *pl = &game_state->players[s];
        if (!pl->is_active || !pl->has_joined || !pl->is_alive) continue;
        pl->current_direction = pl->requested_direction;
    }

    //pohyb
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        game_player_t *pl = &game_state->players[s];
        if (!pl->is_active || !pl->has_joined || !pl->is_alive) continue;

        game_pos_t current_head = pl->snake_body[0];
        game_pos_t new_head = next_position(current_head, pl->current_direction);

        int will_grow = (game_state->has_food &&
                         new_head.x == game_state->food_pos.x &&
                         new_head.y == game_state->food_pos.y);

        // kolizia so stenou
        if (is_wall(game_state, new_head)) {
            pl->is_alive = 0;
            continue;
        }

        // kolizia s inymi hadmi a so sebou
        if (is_occupied_except_tail(game_state, s, new_head, will_grow)) {
            pl->is_alive = 0;
            continue;
        }

        // posun tela
        if (will_grow) {
            if (pl->snake_len < GAME_MAX_SNAKE_LEN) {
                pl->snake_len++;
            }
            pl->score++;
            game_state->has_food = 0; // zjedol
        }

        // shift doprava
        for (uint16_t i = (uint16_t)(pl->snake_len - 1); i > 0; i--) {
            pl->snake_body[i] = pl->snake_body[i - 1];
        }
        pl->snake_body[0] = new_head;
    }

    ensure_food(game_state);
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
    // vyplnenie medzerami
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

    // jedlo
    if (game_state->has_food) {
        out_cells[game_state->food_pos.y * game_state->map_width + game_state->food_pos.x] = (uint8_t)'*';
    }

    // hady
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &game_state->players[s];
        if (!pl->is_active || !pl->has_joined) continue;

        if (!pl->is_alive && pl->snake_len > 0) {
            // mŕtvy had - nechaj jeho telo ako 'x'
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

