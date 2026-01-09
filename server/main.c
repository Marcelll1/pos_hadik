#define D_POSIX_C_SOURCE = 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "game.h"
#include "../common/protocol.h"

#define MAX_CLIENTS GAME_MAX_PLAYERS

typedef struct {
    int client_socket_fd;
} client_slot_t;

typedef struct {
    int listen_socket_fd;
    client_slot_t client_slots[MAX_CLIENTS];

    pthread_mutex_t state_mutex;
    int is_running;

    int tick_interval_ms;
    game_mode_t game_mode;
    uint32_t timed_duration_ms;

    world_type_t world_type;
    char map_file_path[256];

    game_state_t game_state;
    int game_over_sent;
} server_context_t;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int create_listen_socket(uint16_t port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 16) < 0) {
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

static void server_init(server_context_t *server_ctx,
                        int listen_fd,
                        uint8_t map_width,
                        uint8_t map_height,
                        game_mode_t mode,
                        uint32_t timed_duration_ms,
                        world_type_t world_type,
                        const char *map_file_path) {
    memset(server_ctx, 0, sizeof(*server_ctx));

    server_ctx->listen_socket_fd = listen_fd;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        server_ctx->client_slots[i].client_socket_fd = -1;
    }

    pthread_mutex_init(&server_ctx->state_mutex, NULL);
    server_ctx->is_running = 1;

    server_ctx->tick_interval_ms = 200;
    server_ctx->game_mode = mode;
    server_ctx->timed_duration_ms = timed_duration_ms;

    server_ctx->world_type = world_type;
    server_ctx->map_file_path[0] = '\0';
    if (map_file_path && map_file_path[0] != '\0') {
        strncpy(server_ctx->map_file_path, map_file_path, sizeof(server_ctx->map_file_path) - 1);
        server_ctx->map_file_path[sizeof(server_ctx->map_file_path) - 1] = '\0';
    }

    game_init(&server_ctx->game_state, map_width, map_height, mode, timed_duration_ms, world_type);

    uint64_t now = monotonic_ms();
    server_ctx->game_state.start_time_ms = now;
    if (mode == GAME_MODE_TIMED) server_ctx->game_state.timed_end_ms = now + (uint64_t)timed_duration_ms;

    server_ctx->game_over_sent = 0;
}

static int server_add_client(server_context_t *server_ctx, int client_fd, int *out_slot_index) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server_ctx->client_slots[i].client_socket_fd < 0) {
            server_ctx->client_slots[i].client_socket_fd = client_fd;
            *out_slot_index = i;
            return 0;
        }
    }
    return -1;
}

static void server_close_slot_fd(server_context_t *server_ctx, int slot_index) {
    int client_fd = server_ctx->client_slots[slot_index].client_socket_fd;
    if (client_fd >= 0) {
        close(client_fd);
        server_ctx->client_slots[slot_index].client_socket_fd = -1;
    }
}

static int find_slot_by_fd(server_context_t *server_ctx, int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server_ctx->client_slots[i].client_socket_fd == fd) return i;
    }
    return -1;
}

static void drain_payload_if_any(int fd, uint16_t payload_len) {
    char tmp[256];
    uint16_t rem = payload_len;
    while (rem > 0) {
        uint16_t chunk = rem > (uint16_t)sizeof(tmp) ? (uint16_t)sizeof(tmp) : rem;
        if (recv_all_bytes(fd, tmp, chunk) < 0) return;
        rem = (uint16_t)(rem - chunk);
    }
}

static void migrate_client_slot(server_context_t *server_ctx, int from_slot, int to_slot) {
    if (from_slot == to_slot) return;
    int fd = server_ctx->client_slots[from_slot].client_socket_fd;
    server_ctx->client_slots[from_slot].client_socket_fd = -1;
    server_ctx->client_slots[to_slot].client_socket_fd = fd;
}

static void fill_state_player_list(const server_context_t *server_ctx, state_player_info_t *out_players) {
    const game_state_t *g = &server_ctx->game_state;
    int global_paused = g->global_pause_active ? 1 : 0;
    int global_frozen = 0;
    uint64_t now = monotonic_ms();
    if (g->global_freeze_until_ms != 0 && now < g->global_freeze_until_ms) global_frozen = 1;

    for (int i = 0; i < STATE_MAX_PLAYERS; i++) {
        state_player_info_t *p = &out_players[i];
        const game_player_t *gp = &g->players[i];

        memset(p, 0, sizeof(*p));
        p->is_used = gp->is_active ? 1 : 0;
        p->has_joined = gp->has_joined ? 1 : 0;
        p->is_alive = gp->is_alive ? 1 : 0;
        p->is_paused = (gp->is_paused || global_paused || global_frozen) ? 1 : 0;
        p->score_net = htons(gp->score);

        if (gp->player_name[0] != '\0') {
            strncpy((char*)p->name, gp->player_name, STATE_NAME_MAX - 1);
            p->name[STATE_NAME_MAX - 1] = '\0';
        } else {
            p->name[0] = '\0';
        }
    }
}

static void build_game_over_payload(const game_state_t *g, uint64_t now_ms, game_over_message_t *out_msg) {
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->elapsed_ms_net = htonl(game_get_elapsed_ms(g, now_ms));

    uint8_t count = 0;
    for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
        const game_player_t *pl = &g->players[i];
        if (!pl->has_joined) continue;

        game_over_player_entry_t *e = &out_msg->players[count];
        memset(e, 0, sizeof(*e));
        e->has_joined = 1;
        e->score_net = htons(pl->score);

        uint64_t snake_time_ms = pl->snake_time_ms;
        if (pl->is_alive && pl->snake_alive_start_ms != 0 && now_ms >= pl->snake_alive_start_ms) {
            snake_time_ms += (now_ms - pl->snake_alive_start_ms);
        }
        if (snake_time_ms > 0xFFFFFFFFULL) snake_time_ms = 0xFFFFFFFFULL;
        e->snake_time_ms_net = htonl((uint32_t)snake_time_ms);

        strncpy((char*)e->name, pl->player_name, STATE_NAME_MAX - 1);
        e->name[STATE_NAME_MAX - 1] = '\0';

        count++;
        if (count >= STATE_MAX_PLAYERS) break;
    }

    out_msg->player_count = count;
}

static void send_game_over_to_all(server_context_t *server_ctx) {
    if (server_ctx->game_over_sent) return;

    uint64_t now = monotonic_ms();
    game_over_message_t msg;

    pthread_mutex_lock(&server_ctx->state_mutex);
    build_game_over_payload(&server_ctx->game_state, now, &msg);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = server_ctx->client_slots[i].client_socket_fd;
        if (fd >= 0) send_message(fd, MSG_GAME_OVER, &msg, (uint16_t)sizeof(msg));
    }

    server_ctx->game_over_sent = 1;
    pthread_mutex_unlock(&server_ctx->state_mutex);
}

static int validate_player_name_len(uint16_t payload_len) {
    if (payload_len == 0) return -1;
    if (payload_len >= GAME_MAX_NAME_LEN) return -1;
    return 0;
}

static void handle_client_message(server_context_t *server_ctx, int client_fd) {
    message_header_t header_net;
    if (recv_message_header(client_fd, &header_net) < 0) {
        shutdown(client_fd, SHUT_RDWR);
        return;
    }

    uint16_t message_type = ntohs(header_net.message_type_net);
    uint16_t payload_len  = ntohs(header_net.payload_len_net);

    int slot_index = find_slot_by_fd(server_ctx, client_fd);
    if (slot_index < 0) {
        drain_payload_if_any(client_fd, payload_len);
        shutdown(client_fd, SHUT_RDWR);
        return;
    }

    if (message_type == MSG_SHUTDOWN) {
        drain_payload_if_any(client_fd, payload_len);
        server_ctx->is_running = 0;
        return;
    }

    if (message_type == MSG_PAUSE) {
        drain_payload_if_any(client_fd, payload_len);

        pthread_mutex_lock(&server_ctx->state_mutex);

        game_handle_pause(&server_ctx->game_state, slot_index);
        server_close_slot_fd(server_ctx, slot_index);
        server_ctx->game_state.players[slot_index].is_active = 0;

        pthread_mutex_unlock(&server_ctx->state_mutex);
        return;
    }

    if (message_type == MSG_LEAVE) {
        drain_payload_if_any(client_fd, payload_len);
        uint64_t now = monotonic_ms();

        pthread_mutex_lock(&server_ctx->state_mutex);
        server_close_slot_fd(server_ctx, slot_index);
        game_handle_leave(&server_ctx->game_state, slot_index, now);
        pthread_mutex_unlock(&server_ctx->state_mutex);
        return;
    }

    if (message_type == MSG_JOIN) {
        if (validate_player_name_len(payload_len) != 0) {
            drain_payload_if_any(client_fd, payload_len);
            const char *error_text = "bad player name length (max 31)";
            send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
            shutdown(client_fd, SHUT_RDWR);
            return;
        }

        char player_name[GAME_MAX_NAME_LEN];
        memset(player_name, 0, sizeof(player_name));

        if (recv_all_bytes(client_fd, player_name, payload_len) < 0) {
            shutdown(client_fd, SHUT_RDWR);
            return;
        }
        player_name[payload_len] = '\0';

        uint64_t now = monotonic_ms();

        pthread_mutex_lock(&server_ctx->state_mutex);

        int paused_slot = game_find_paused_player_by_name(&server_ctx->game_state, player_name);
        if (paused_slot >= 0) {
            if (paused_slot != slot_index) {
                migrate_client_slot(server_ctx, slot_index, paused_slot);
                game_mark_client_inactive_keep_or_clear(&server_ctx->game_state, slot_index, 0);
                slot_index = paused_slot;
            }

            game_mark_client_active(&server_ctx->game_state, slot_index);
            (void)game_resume_player(&server_ctx->game_state, slot_index, now);

            if (server_ctx->game_state.global_pause_active &&
                strncmp(server_ctx->game_state.global_pause_owner_name, player_name, GAME_MAX_NAME_LEN) == 0) {
                server_ctx->game_state.global_pause_active = 0;
                server_ctx->game_state.global_pause_owner_name[0] = '\0';
                server_ctx->game_state.global_freeze_until_ms = now + 3000ULL;
            } else {
                server_ctx->game_state.global_freeze_until_ms = now + 3000ULL;
            }

            pthread_mutex_unlock(&server_ctx->state_mutex);

            const char *welcome_text = "RESUMED | WASD move | p pause | q leave | r respawn";
            send_message(client_fd, MSG_WELCOME, welcome_text, (uint16_t)strlen(welcome_text));
            return;
        }

        int join_rc = game_join_new_player(&server_ctx->game_state, slot_index, player_name, now);

        pthread_mutex_unlock(&server_ctx->state_mutex);

        if (join_rc < 0) {
            const char *error_text = "JOIN failed";
            send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
            shutdown(client_fd, SHUT_RDWR);
            return;
        }

        const char *welcome_text = "WELCOME | WASD move | p pause | q leave | r respawn";
        send_message(client_fd, MSG_WELCOME, welcome_text, (uint16_t)strlen(welcome_text));
        return;
    }

    if (message_type == MSG_INPUT) {
        if (payload_len != sizeof(input_message_t)) {
            const char *error_text = "bad INPUT length";
            send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
            shutdown(client_fd, SHUT_RDWR);
            return;
        }

        input_message_t input_message;
        if (recv_all_bytes(client_fd, &input_message, sizeof(input_message)) < 0) {
            shutdown(client_fd, SHUT_RDWR);
            return;
        }

        direction_t direction = (direction_t)input_message.direction;

        pthread_mutex_lock(&server_ctx->state_mutex);
        game_handle_input(&server_ctx->game_state, slot_index, direction);
        pthread_mutex_unlock(&server_ctx->state_mutex);
        return;
    }

    if (message_type == MSG_RESPAWN) {
        drain_payload_if_any(client_fd, payload_len);
        uint64_t now = monotonic_ms();
        pthread_mutex_lock(&server_ctx->state_mutex);
        (void)game_respawn_player(&server_ctx->game_state, slot_index, now);
        pthread_mutex_unlock(&server_ctx->state_mutex);
        return;
    }

    drain_payload_if_any(client_fd, payload_len);
    {
        const char *error_text = "unknown message type";
        send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
    }
}

static void *server_tick_thread(void *arg) {
    server_context_t *server_ctx = (server_context_t*)arg;

    while (server_ctx->is_running) {
        struct timespec sleep_time;
        sleep_time.tv_sec = server_ctx->tick_interval_ms / 1000;
        sleep_time.tv_nsec = (long)(server_ctx->tick_interval_ms % 1000) * 1000L * 1000L;
        nanosleep(&sleep_time, NULL);

        uint64_t now = monotonic_ms();

        state_message_t state_message;
        memset(&state_message, 0, sizeof(state_message));

        pthread_mutex_lock(&server_ctx->state_mutex);

        state_message.width = server_ctx->game_state.map_width;
        state_message.height = server_ctx->game_state.map_height;
        state_message.game_mode = (uint8_t)server_ctx->game_mode;
        state_message.world_type = (uint8_t)server_ctx->world_type;

        game_tick(&server_ctx->game_state, now);

        if (server_ctx->game_state.should_terminate) {
            pthread_mutex_unlock(&server_ctx->state_mutex);
            send_game_over_to_all(server_ctx);
            pthread_mutex_lock(&server_ctx->state_mutex);
            server_ctx->is_running = 0;
        }

        state_message.tick_counter_net = htonl(server_ctx->game_state.tick_counter);
        state_message.elapsed_ms_net = htonl(game_get_elapsed_ms(&server_ctx->game_state, now));
        state_message.remaining_ms_net = htonl(game_get_remaining_ms(&server_ctx->game_state, now));

        fill_state_player_list(server_ctx, state_message.players);
        game_build_ascii_map(&server_ctx->game_state, state_message.cells, sizeof(state_message.cells));

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = server_ctx->client_slots[i].client_socket_fd;
            if (fd >= 0) send_message(fd, MSG_STATE, &state_message, (uint16_t)sizeof(state_message));
        }

        pthread_mutex_unlock(&server_ctx->state_mutex);
    }

    return NULL;
}

static int load_map_and_set_size(game_state_t *game_state, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    int width = -1;
    int height = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0) continue;

        if (width < 0) width = (int)len;
        if ((int)len != width) {
            fclose(f);
            return -1;
        }
        height++;
    }

    fclose(f);

    if (width < 5 || height < 5) return -1;
    if (width > STATE_MAX_WIDTH || height > STATE_MAX_HEIGHT) return -1;

    game_state->map_width = (uint8_t)width;
    game_state->map_height = (uint8_t)height;

    memset(game_state->obstacle_map, 0, sizeof(game_state->obstacle_map));

    return game_load_map_from_file(game_state, path);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    uint16_t port = 23456;
    game_mode_t mode = GAME_MODE_STANDARD;
    uint32_t timed_seconds = 60;
    world_type_t world_type = WORLD_EMPTY;

    uint8_t map_width = 40;
    uint8_t map_height = 20;
    const char *map_file_path = NULL;

    if (argc >= 2) port = (uint16_t)atoi(argv[1]);
    if (argc >= 3) mode = (game_mode_t)atoi(argv[2]);
    if (argc >= 4) timed_seconds = (uint32_t)atoi(argv[3]);
    if (argc >= 5) world_type = (world_type_t)atoi(argv[4]);

    if (world_type == WORLD_FILE) {
        if (argc >= 6) map_file_path = argv[5];
    } else {
        if (argc >= 6) map_width = (uint8_t)atoi(argv[5]);
        if (argc >= 7) map_height = (uint8_t)atoi(argv[6]);
        if (argc >= 8) map_file_path = argv[7];
    }

    if (map_width < 5) map_width = 5;
    if (map_height < 5) map_height = 5;
    if (map_width > STATE_MAX_WIDTH) map_width = STATE_MAX_WIDTH;
    if (map_height > STATE_MAX_HEIGHT) map_height = STATE_MAX_HEIGHT;

    uint32_t timed_ms = timed_seconds * 1000U;

    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
        return 1;
    }

    server_context_t server_ctx;
    server_init(&server_ctx, listen_fd, map_width, map_height, mode, timed_ms, world_type, map_file_path);

    if (world_type == WORLD_FILE) {
        if (!map_file_path || map_file_path[0] == '\0') {
            fprintf(stderr, "server: WORLD_FILE requires map path\n");
            close(server_ctx.listen_socket_fd);
            return 1;
        }
        if (load_map_and_set_size(&server_ctx.game_state, map_file_path) != 0) {
            fprintf(stderr, "server: failed to load map: %s\n", map_file_path);
            close(server_ctx.listen_socket_fd);
            return 1;
        }
        server_ctx.world_type = WORLD_FILE;
        server_ctx.game_state.world_type = WORLD_FILE;
    }

    pthread_t tick_thread;
    if (pthread_create(&tick_thread, NULL, server_tick_thread, &server_ctx) != 0) {
        fprintf(stderr, "server: pthread_create failed\n");
        close(server_ctx.listen_socket_fd);
        return 1;
    }

    while (server_ctx.is_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        FD_SET(server_ctx.listen_socket_fd, &read_fds);
        int max_fd = server_ctx.listen_socket_fd;

        int client_fds_snapshot[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) client_fds_snapshot[i] = -1;

        pthread_mutex_lock(&server_ctx.state_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_fds_snapshot[i] = server_ctx.client_slots[i].client_socket_fd;
        }
        pthread_mutex_unlock(&server_ctx.state_mutex);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds_snapshot[i];
            if (fd >= 0) {
                FD_SET(fd, &read_fds);
                if (fd > max_fd) max_fd = fd;
            }
        }

        int select_rc = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (select_rc < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "server: select failed: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(server_ctx.listen_socket_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);

            int client_fd = accept(server_ctx.listen_socket_fd, (struct sockaddr*)&client_addr, &client_addr_len);
            if (client_fd >= 0) {
                pthread_mutex_lock(&server_ctx.state_mutex);

                int slot_index = -1;
                int add_rc = server_add_client(&server_ctx, client_fd, &slot_index);
                if (add_rc == 0) {
                    game_mark_client_active(&server_ctx.game_state, slot_index);
                } else {
                    const char *error_text = "server full";
                    send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
                    close(client_fd);
                }

                pthread_mutex_unlock(&server_ctx.state_mutex);
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds_snapshot[i];
            if (fd >= 0 && FD_ISSET(fd, &read_fds)) handle_client_message(&server_ctx, fd);
        }

        pthread_mutex_lock(&server_ctx.state_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = server_ctx.client_slots[i].client_socket_fd;
            if (fd < 0) continue;

            char ping;
            ssize_t rc = recv(fd, &ping, 1, MSG_PEEK | MSG_DONTWAIT);
            if (rc == 0) {
                shutdown(fd, SHUT_RDWR);
                server_close_slot_fd(&server_ctx, i);
                game_mark_client_inactive_keep_or_clear(&server_ctx.game_state, i, 0);
            }
        }
        pthread_mutex_unlock(&server_ctx.state_mutex);
    }

    send_game_over_to_all(&server_ctx);

    pthread_join(tick_thread, NULL);

    pthread_mutex_lock(&server_ctx.state_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        server_close_slot_fd(&server_ctx, i);
        game_mark_client_inactive_keep_or_clear(&server_ctx.game_state, i, 0);
    }
    pthread_mutex_unlock(&server_ctx.state_mutex);

    close(server_ctx.listen_socket_fd);
    return 0;
}


