#include "game.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int is_opposite_direction(direction_t a, direction_t b) {
    return (a == DIR_UP && b == DIR_DOWN) ||
           (a == DIR_DOWN && b == DIR_UP) ||
           (a == DIR_LEFT && b == DIR_RIGHT) ||
           (a == DIR_RIGHT && b == DIR_LEFT);
}

static void direction_delta(direction_t dir, int *dx, int *dy) {
    *dx = 0;
    *dy = 0;
    switch (dir) {
        case DIR_UP:    *dy = -1; break;
        case DIR_RIGHT: *dx =  1; break;
        case DIR_DOWN:  *dy =  1; break;
        case DIR_LEFT:  *dx = -1; break;
        default: break;
    }
}

static int is_inside_bounds(const game_state_t *g, int x, int y) {
    return x >= 0 && y >= 0 && x < (int)g->map_width && y < (int)g->map_height;
}

static game_pos_t wrap_position(const game_state_t *g, int x, int y) {
    int w = (int)g->map_width;
    int h = (int)g->map_height;

    int nx = x % w;
    int ny = y % h;
    if (nx < 0) nx += w;
    if (ny < 0) ny += h;

    game_pos_t p;
    p.x = (uint8_t)nx;
    p.y = (uint8_t)ny;
    return p;
}

static int cell_is_obstacle(const game_state_t *g, uint8_t x, uint8_t y) {
    if (x >= g->map_width || y >= g->map_height) return 1;
    return g->obstacle_map[(size_t)y * g->map_width + x] ? 1 : 0;
}

static int is_food_at(const game_state_t *g, game_pos_t p, int *out_food_index) {
    for (uint8_t i = 0; i < g->food_count; i++) {
        if (g->food_positions[i].x == p.x && g->food_positions[i].y == p.y) {
            if (out_food_index) *out_food_index = (int)i;
            return 1;
        }
    }
    return 0;
}

static void remove_food_at(game_state_t *g, int food_index) {
    if (food_index < 0 || food_index >= (int)g->food_count) return;
    uint8_t last = (uint8_t)(g->food_count - 1);
    g->food_positions[food_index] = g->food_positions[last];
    g->food_count = last;
}

static int is_occupied_except_tail(const game_state_t *g, int player_slot, game_pos_t p, int will_grow) {
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &g->players[s];
        if (!pl->has_joined) continue;
        if (!pl->is_alive) continue;

        for (uint16_t i = 0; i < pl->snake_len; i++) {
            if (s == player_slot && !will_grow && i == (uint16_t)(pl->snake_len - 1)) continue;
            if (pl->snake_body[i].x == p.x && pl->snake_body[i].y == p.y) return 1;
        }
    }
    return 0;
}

static int count_alive_snakes(const game_state_t *g) {
    int count = 0;
    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &g->players[s];
        if (pl->has_joined && pl->is_alive) count++;
    }
    return count;
}

static int cell_is_free_for_spawn(const game_state_t *g, game_pos_t p) {
    if (g->world_type == WORLD_FILE) {
        if (cell_is_obstacle(g, p.x, p.y)) return 0;
    }
    if (is_occupied_except_tail(g, -1, p, 1)) return 0;
    if (is_food_at(g, p, NULL)) return 0;
    return 1;
}

static int find_free_cell(const game_state_t *g, game_pos_t *out_pos) {
    int w = (int)g->map_width;
    int h = (int)g->map_height;

    for (int attempts = 0; attempts < 40000; attempts++) {
        game_pos_t p;
        p.x = (uint8_t)(rand() % w);
        p.y = (uint8_t)(rand() % h);
        if (!cell_is_free_for_spawn(g, p)) continue;
        *out_pos = p;
        return 0;
    }
    return -1;
}

static void ensure_food_count(game_state_t *g) {
    int alive = count_alive_snakes(g);
    if (alive < 0) alive = 0;
    if (alive > GAME_MAX_PLAYERS) alive = GAME_MAX_PLAYERS;

    while (g->food_count < (uint8_t)alive) {
        game_pos_t p;
        if (find_free_cell(g, &p) != 0) break;
        g->food_positions[g->food_count++] = p;
    }

    while (g->food_count > (uint8_t)alive) {
        g->food_count--;
    }
}

static void strip_line_endings(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

static int map_char_is_obstacle(char c) {
    if (c == ' ' || c == '.') return 0;
    return 1;
}

int game_load_map_from_file(game_state_t *g, const char *path) {
    if (!g || !path || path[0] == '\0') return -1;
    if (g->map_width == 0 || g->map_height == 0) return -1;
    if (g->map_width > STATE_MAX_WIDTH || g->map_height > STATE_MAX_HEIGHT) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(g->obstacle_map, 0, sizeof(g->obstacle_map));

    char line[1024];
    for (uint8_t y = 0; y < g->map_height; y++) {
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return -1;
        }

        strip_line_endings(line);

        size_t len = strlen(line);
        if (len < g->map_width) {
            fclose(f);
            return -1;
        }

        for (uint8_t x = 0; x < g->map_width; x++) {
            char c = line[x];
            g->obstacle_map[(size_t)y * g->map_width + x] = map_char_is_obstacle(c) ? 1 : 0;
        }
    }

    fclose(f);
    return 0;
}

void game_init(game_state_t *g,
               uint8_t map_width,
               uint8_t map_height,
               game_mode_t mode,
               uint32_t timed_duration_ms,
               world_type_t world_type) {
    memset(g, 0, sizeof(*g));

    if (map_width < 5) map_width = 5;
    if (map_height < 5) map_height = 5;
    if (map_width > STATE_MAX_WIDTH) map_width = STATE_MAX_WIDTH;
    if (map_height > STATE_MAX_HEIGHT) map_height = STATE_MAX_HEIGHT;

    g->map_width = map_width;
    g->map_height = map_height;

    g->world_type = world_type;
    memset(g->obstacle_map, 0, sizeof(g->obstacle_map));

    g->food_count = 0;

    g->game_mode = mode;
    g->start_time_ms = 0;
    g->timed_end_ms = 0;
    g->last_no_snakes_ms = 0;
    g->should_terminate = 0;

    g->global_freeze_until_ms = 0;
    g->global_pause_active = 0;
    g->global_pause_owner_name[0] = '\0';

    for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
        game_player_t *pl = &g->players[i];
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
        pl->snake_alive_start_ms = 0;
        pl->snake_time_ms = 0;
    }

    unsigned seed = (unsigned)timed_duration_ms;
    seed ^= (unsigned)(uintptr_t)g;
    seed ^= (unsigned)map_width << 8;
    seed ^= (unsigned)map_height << 16;
    srand(seed);
}

void game_mark_client_active(game_state_t *g, int player_slot) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;
    g->players[player_slot].is_active = 1;
}

void game_mark_client_inactive_keep_or_clear(game_state_t *g, int player_slot, int keep_player_state) {
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &g->players[player_slot];
    pl->is_active = 0;

    if (keep_player_state) return;

    pl->has_joined = 0;
    pl->is_alive = 0;
    pl->is_paused = 0;
    pl->snake_len = 0;
    pl->score = 0;
    pl->player_name[0] = '\0';
    pl->current_direction = DIR_RIGHT;
    pl->requested_direction = DIR_RIGHT;
    pl->freeze_until_ms = 0;
    pl->snake_alive_start_ms = 0;
    pl->snake_time_ms = 0;
}

int game_find_paused_player_by_name(const game_state_t *g, const char *player_name) {
    for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
        const game_player_t *pl = &g->players[i];
        if (!pl->has_joined) continue;
        if (!pl->is_paused) continue;
        if (strncmp(pl->player_name, player_name, GAME_MAX_NAME_LEN) == 0) return i;
    }
    return -1;
}

static game_pos_t step_in_world(const game_state_t *g, game_pos_t from, direction_t dir) {
    int dx, dy;
    direction_delta(dir, &dx, &dy);
    int nx = (int)from.x + dx;
    int ny = (int)from.y + dy;

    if (g->world_type == WORLD_EMPTY) {
        return wrap_position(g, nx, ny);
    }

    game_pos_t p;
    p.x = (uint8_t)nx;
    p.y = (uint8_t)ny;
    return p;
}

static int cell_is_safe_for_spawn_path(const game_state_t *g, game_pos_t p) {
    if (!is_inside_bounds(g, (int)p.x, (int)p.y)) return 0;
    if (g->world_type == WORLD_FILE && cell_is_obstacle(g, p.x, p.y)) return 0;
    if (is_occupied_except_tail(g, -1, p, 1)) return 0;
    if (is_food_at(g, p, NULL)) return 0;
    return 1;
}

static int spawn_is_safe(const game_state_t *g, game_pos_t head, direction_t start_dir) {
    if (!cell_is_safe_for_spawn_path(g, head)) return 0;

    game_pos_t back1 = step_in_world(g, head, (direction_t)((start_dir + 2) % 4));
    game_pos_t back2 = step_in_world(g, back1, (direction_t)((start_dir + 2) % 4));

    if (!cell_is_safe_for_spawn_path(g, back1)) return 0;
    if (!cell_is_safe_for_spawn_path(g, back2)) return 0;

    return 1;
}

static int pick_safe_spawn(const game_state_t *g, game_pos_t *out_head, direction_t *out_dir) {
    static const direction_t dirs[4] = { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

    for (int attempts = 0; attempts < 80000; attempts++) {
        game_pos_t head;
        if (find_free_cell(g, &head) != 0) return -1;

        int d0 = rand() % 4;
        for (int k = 0; k < 4; k++) {
            direction_t dir = dirs[(d0 + k) % 4];
            if (spawn_is_safe(g, head, dir)) {
                *out_head = head;
                *out_dir = dir;
                return 0;
            }
        }
    }
    return -1;
}

static void start_snake_life(game_player_t *pl, uint64_t now_ms) {
    pl->snake_alive_start_ms = now_ms;
}

static void end_snake_life(game_player_t *pl, uint64_t now_ms) {
    if (pl->snake_alive_start_ms != 0 && now_ms >= pl->snake_alive_start_ms) {
        pl->snake_time_ms += (now_ms - pl->snake_alive_start_ms);
    }
    pl->snake_alive_start_ms = 0;
}

int game_join_new_player(game_state_t *g, int player_slot, const char *player_name, uint64_t now_ms) {
    if (!g || !player_name) return -1;
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return -1;

    if (g->start_time_ms == 0) g->start_time_ms = now_ms;

    game_player_t *pl = &g->players[player_slot];
    if (!pl->is_active) return -1;

    strncpy(pl->player_name, player_name, GAME_MAX_NAME_LEN - 1);
    pl->player_name[GAME_MAX_NAME_LEN - 1] = '\0';

    pl->has_joined = 1;
    pl->is_alive = 1;
    pl->is_paused = 0;
    pl->score = 0;
    pl->snake_len = 0;
    pl->freeze_until_ms = 0;
    pl->snake_time_ms = 0;

    game_pos_t head;
    direction_t start_dir;
    if (pick_safe_spawn(g, &head, &start_dir) != 0) return -1;

    pl->current_direction = start_dir;
    pl->requested_direction = start_dir;

    pl->snake_len = 3;

    direction_t back_dir = (direction_t)((start_dir + 2) % 4);
    game_pos_t seg1 = step_in_world(g, head, back_dir);
    game_pos_t seg2 = step_in_world(g, seg1, back_dir);

    pl->snake_body[0] = head;
    pl->snake_body[1] = seg1;
    pl->snake_body[2] = seg2;

    start_snake_life(pl, now_ms);

    g->global_freeze_until_ms = now_ms + 3000ULL;

    ensure_food_count(g);
    return 0;
}

int game_resume_player(game_state_t *g, int player_slot, uint64_t now_ms) {
    (void)now_ms;
    if (!g) return -1;
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return -1;
    game_player_t *pl = &g->players[player_slot];
    if (!pl->has_joined) return -1;

    pl->is_paused = 0;
    ensure_food_count(g);
    return 0;
}

int game_respawn_player(game_state_t *g, int player_slot, uint64_t now_ms) {
    if (!g) return -1;
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return -1;

    game_player_t *pl = &g->players[player_slot];
    if (!pl->has_joined) return -1;
    if (!pl->is_active) return -1;
    if (pl->is_alive) return -1;
    if (pl->is_paused) return -1;

    game_pos_t head;
    direction_t start_dir;
    if (pick_safe_spawn(g, &head, &start_dir) != 0) return -1;

    pl->is_alive = 1;
    pl->freeze_until_ms = now_ms + 1000ULL;
    pl->current_direction = start_dir;
    pl->requested_direction = start_dir;

    pl->snake_len = 3;
    direction_t back_dir = (direction_t)((start_dir + 2) % 4);
    game_pos_t seg1 = step_in_world(g, head, back_dir);
    game_pos_t seg2 = step_in_world(g, seg1, back_dir);

    pl->snake_body[0] = head;
    pl->snake_body[1] = seg1;
    pl->snake_body[2] = seg2;

    start_snake_life(pl, now_ms);

    g->global_freeze_until_ms = now_ms + 3000ULL;

    ensure_food_count(g);
    return 0;
}

void game_handle_input(game_state_t *g, int player_slot, direction_t direction) {
    if (!g) return;
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &g->players[player_slot];
    if (!pl->has_joined || !pl->is_alive) return;
    if (pl->is_paused) return;

    if (is_opposite_direction(pl->current_direction, direction)) return;
    pl->requested_direction = direction;
}

void game_handle_pause(game_state_t *g, int player_slot) {
    if (!g) return;
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;
    game_player_t *pl = &g->players[player_slot];
    if (!pl->has_joined) return;

    g->global_pause_active = 1;
    strncpy(g->global_pause_owner_name, pl->player_name, GAME_MAX_NAME_LEN - 1);
    g->global_pause_owner_name[GAME_MAX_NAME_LEN - 1] = '\0';
    pl->is_paused = 1;
}

void game_handle_leave(game_state_t *g, int player_slot, uint64_t now_ms) {
    if (!g) return;
    if (player_slot < 0 || player_slot >= GAME_MAX_PLAYERS) return;

    game_player_t *pl = &g->players[player_slot];
    if (pl->has_joined && pl->is_alive) {
        end_snake_life(pl, now_ms);
    }

    if (g->global_pause_active && strncmp(g->global_pause_owner_name, pl->player_name, GAME_MAX_NAME_LEN) == 0) {
        g->global_pause_active = 0;
        g->global_pause_owner_name[0] = '\0';
    }

    game_mark_client_inactive_keep_or_clear(g, player_slot, 0);
    ensure_food_count(g);
}

static void update_game_termination(game_state_t *g, uint64_t now_ms) {
    int alive = count_alive_snakes(g);

    if (g->game_mode == GAME_MODE_TIMED) {
        if (g->timed_end_ms != 0 && now_ms >= g->timed_end_ms) {
            g->should_terminate = 1;
        }
        return;
    }

    if (alive == 0) {
        if (g->last_no_snakes_ms == 0) {
            g->last_no_snakes_ms = now_ms;
        } else if (now_ms - g->last_no_snakes_ms >= 10000ULL) {
            g->should_terminate = 1;
        }
    } else {
        g->last_no_snakes_ms = 0;
    }
}

void game_tick(game_state_t *g, uint64_t now_ms) {
    if (!g) return;

    if (g->start_time_ms == 0) g->start_time_ms = now_ms;

    g->tick_counter++;

    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        game_player_t *pl = &g->players[s];
        if (!pl->has_joined || !pl->is_alive) continue;
        if (pl->is_paused) continue;
        pl->current_direction = pl->requested_direction;
    }

    int global_frozen = 0;
    if (g->global_pause_active) global_frozen = 1;
    if (g->global_freeze_until_ms != 0 && now_ms < g->global_freeze_until_ms) global_frozen = 1;

    if (!global_frozen) {
        for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
            game_player_t *pl = &g->players[s];
            if (!pl->has_joined || !pl->is_alive) continue;
            if (pl->is_paused) continue;
            if (pl->freeze_until_ms != 0 && now_ms < pl->freeze_until_ms) continue;

            game_pos_t head = pl->snake_body[0];
            game_pos_t new_head = step_in_world(g, head, pl->current_direction);

            if (g->world_type == WORLD_FILE) {
                if (!is_inside_bounds(g, (int)new_head.x, (int)new_head.y)) {
                    pl->is_alive = 0;
                    end_snake_life(pl, now_ms);
                    continue;
                }
                if (cell_is_obstacle(g, new_head.x, new_head.y)) {
                    pl->is_alive = 0;
                    end_snake_life(pl, now_ms);
                    continue;
                }
            }

            int food_index = -1;
            int will_grow = is_food_at(g, new_head, &food_index);

            if (is_occupied_except_tail(g, s, new_head, will_grow)) {
                pl->is_alive = 0;
                end_snake_life(pl, now_ms);
                continue;
            }

            if (will_grow) {
                remove_food_at(g, food_index);
                pl->score = (uint16_t)(pl->score + 1);
                if (pl->snake_len < GAME_MAX_SNAKE_LEN) {
                    for (uint16_t i = pl->snake_len; i > 0; i--) {
                        pl->snake_body[i] = pl->snake_body[i - 1];
                    }
                    pl->snake_body[0] = new_head;
                    pl->snake_len++;
                } else {
                    for (uint16_t i = pl->snake_len - 1; i > 0; i--) {
                        pl->snake_body[i] = pl->snake_body[i - 1];
                    }
                    pl->snake_body[0] = new_head;
                }
            } else {
                for (uint16_t i = pl->snake_len - 1; i > 0; i--) {
                    pl->snake_body[i] = pl->snake_body[i - 1];
                }
                pl->snake_body[0] = new_head;
            }
        }
    }

    ensure_food_count(g);
    update_game_termination(g, now_ms);
}

static char head_char(int idx) {
    if (idx >= 0 && idx < 26) return (char)('A' + idx);
    idx -= 26;
    if (idx >= 0 && idx < 10) return (char)('0' + idx);
    return '@';
}

static char body_char(int idx) {
    if (idx >= 0 && idx < 26) return (char)('a' + idx);
    idx -= 26;
    if (idx >= 0 && idx < 10) return (char)('0' + idx);
    return 'o';
}

void game_build_ascii_map(const game_state_t *g, uint8_t *out_cells, size_t out_cells_len) {
    if (!g || !out_cells) return;

    for (size_t i = 0; i < out_cells_len; i++) out_cells[i] = (uint8_t)' ';

    uint8_t width = g->map_width;
    uint8_t height = g->map_height;
    size_t world_cells = (size_t)width * (size_t)height;
    if (world_cells > out_cells_len) world_cells = out_cells_len;

    if (g->world_type == WORLD_FILE) {
        for (uint8_t y = 0; y < height; y++) {
            for (uint8_t x = 0; x < width; x++) {
                size_t idx = (size_t)y * width + x;
                if (idx >= world_cells) continue;
                if (cell_is_obstacle(g, x, y)) out_cells[idx] = (uint8_t)'#';
            }
        }
    }

    for (uint8_t i = 0; i < g->food_count; i++) {
        game_pos_t f = g->food_positions[i];
        size_t idx = (size_t)f.y * width + f.x;
        if (idx < world_cells) out_cells[idx] = (uint8_t)'*';
    }

    for (int s = 0; s < GAME_MAX_PLAYERS; s++) {
        const game_player_t *pl = &g->players[s];
        if (!pl->has_joined) continue;

        if (!pl->is_alive && pl->snake_len > 0) {
            for (uint16_t i = 0; i < pl->snake_len; i++) {
                game_pos_t p = pl->snake_body[i];
                size_t idx = (size_t)p.y * width + p.x;
                if (idx < world_cells) out_cells[idx] = (uint8_t)'x';
            }
            continue;
        }

        if (pl->snake_len > 0) {
            game_pos_t h = pl->snake_body[0];
            size_t idx = (size_t)h.y * width + h.x;
            if (idx < world_cells) out_cells[idx] = (uint8_t)head_char(s);
        }
        for (uint16_t i = 1; i < pl->snake_len; i++) {
            game_pos_t p = pl->snake_body[i];
            size_t idx = (size_t)p.y * width + p.x;
            if (idx < world_cells) out_cells[idx] = (uint8_t)body_char(s);
        }
    }
}

uint32_t game_get_elapsed_ms(const game_state_t *g, uint64_t now_ms) {
    if (!g) return 0;
    if (g->start_time_ms == 0) return 0;
    uint64_t diff = now_ms - g->start_time_ms;
    if (diff > 0xFFFFFFFFULL) diff = 0xFFFFFFFFULL;
    return (uint32_t)diff;
}

uint32_t game_get_remaining_ms(const game_state_t *g, uint64_t now_ms) {
    if (!g) return 0;
    if (g->game_mode != GAME_MODE_TIMED) return 0;
    if (g->timed_end_ms == 0) return 0;
    if (now_ms >= g->timed_end_ms) return 0;
    uint64_t rem = g->timed_end_ms - now_ms;
    if (rem > 0xFFFFFFFFULL) rem = 0xFFFFFFFFULL;
    return (uint32_t)rem;
}

