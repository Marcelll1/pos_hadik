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

#include "../common/protocol.h"
#include "game.h"

#define MAX_CLIENTS 64
#define MAX_JOIN_NAME 256

typedef struct {
    int client_socket_fd;
} server_client_slot_t;

typedef struct {
    int listen_socket_fd;

    server_client_slot_t client_slots[MAX_CLIENTS];

    pthread_mutex_t state_mutex;
    int is_running;

    uint32_t tick_interval_ms;
    game_state_t game_state;

    game_mode_t game_mode;
    uint32_t timed_duration_ms;

    world_type_t world_type;
    char map_file_path[256];
} server_context_t;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int create_listen_socket(uint16_t port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        close(listen_fd);
        return -1;
    }

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

static void server_init(server_context_t *server_ctx, int listen_fd, game_mode_t mode, uint32_t timed_duration_ms, world_type_t world_type, const char *map_file_path) {
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

    game_init(&server_ctx->game_state, STATE_MAP_WIDTH, STATE_MAP_HEIGHT, mode, timed_duration_ms, world_type);

    uint64_t now = monotonic_ms();
    server_ctx->game_state.start_time_ms = now;
    if (mode == GAME_MODE_TIMED) {
        server_ctx->game_state.timed_end_ms = now + (uint64_t)timed_duration_ms;
    }
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

static int socket_is_disconnected(int socket_fd) {
    char one_byte;
    ssize_t peek_rc = recv(socket_fd, &one_byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peek_rc == 0) return 1;
    return 0;
}

static void drain_payload_if_any(int client_fd, uint16_t payload_len) {
    char tmp[256];
    uint16_t remaining = payload_len;
    while (remaining > 0) {
        uint16_t chunk = remaining > (uint16_t)sizeof(tmp) ? (uint16_t)sizeof(tmp) : remaining;
        if (recv_all_bytes(client_fd, tmp, chunk) < 0) return;
        remaining = (uint16_t)(remaining - chunk);
    }
}

static int find_slot_by_fd(const server_context_t *server_ctx, int client_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server_ctx->client_slots[i].client_socket_fd == client_fd) return i;
    }
    return -1;
}

static void fill_state_player_list(const game_state_t *game_state, state_player_info_t *out_players) {
    for (int i = 0; i < STATE_MAX_PLAYERS; i++) {
        state_player_info_t *p = &out_players[i];
        memset(p, 0, sizeof(*p));

        const game_player_t *gp = &game_state->players[i];

        p->is_used = (uint8_t)(gp->has_joined ? 1 : 0);
        p->has_joined = (uint8_t)(gp->has_joined ? 1 : 0);
        p->is_alive = (uint8_t)(gp->is_alive ? 1 : 0);
        p->is_paused = (uint8_t)(gp->is_paused ? 1 : 0);
        p->score_net = htons(gp->score);

        if (gp->player_name[0] != '\0') {
            strncpy((char*)p->name, gp->player_name, STATE_NAME_MAX - 1);
            p->name[STATE_NAME_MAX - 1] = '\0';
        } else {
            p->name[0] = '\0';
        }
    }
}

static void migrate_client_slot(server_context_t *server_ctx, int from_slot, int to_slot) {
    if (from_slot == to_slot) return;
    int fd = server_ctx->client_slots[from_slot].client_socket_fd;
    server_ctx->client_slots[from_slot].client_socket_fd = -1;
    server_ctx->client_slots[to_slot].client_socket_fd = fd;
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
        game_mark_client_inactive_keep_or_clear(&server_ctx->game_state, slot_index, 1);
        pthread_mutex_unlock(&server_ctx->state_mutex);
        return;
    }

    if (message_type == MSG_LEAVE) {
        drain_payload_if_any(client_fd, payload_len);
        pthread_mutex_lock(&server_ctx->state_mutex);
        server_close_slot_fd(server_ctx, slot_index);
        game_handle_leave(&server_ctx->game_state, slot_index);
        pthread_mutex_unlock(&server_ctx->state_mutex);
        return;
    }

    if (message_type == MSG_JOIN) {
        char player_name[MAX_JOIN_NAME];

        if (payload_len == 0 || payload_len >= MAX_JOIN_NAME) {
            const char *error_text = "bad JOIN length";
            send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
            shutdown(client_fd, SHUT_RDWR);
            return;
        }
        if (recv_all_bytes(client_fd, player_name, payload_len) < 0) {
            shutdown(client_fd, SHUT_RDWR);
            return;
        }
        player_name[payload_len] = '\0';

        uint64_t now = monotonic_ms();

        pthread_mutex_lock(&server_ctx->state_mutex);

        int paused_slot = game_find_paused_player_by_name(&server_ctx->game_state, player_name);
        if (paused_slot >= 0 && paused_slot != slot_index) {
            migrate_client_slot(server_ctx, slot_index, paused_slot);
            game_mark_client_inactive_keep_or_clear(&server_ctx->game_state, slot_index, 0);

            slot_index = paused_slot;
            game_mark_client_active(&server_ctx->game_state, slot_index);
            (void)game_resume_player(&server_ctx->game_state, slot_index, now);

            pthread_mutex_unlock(&server_ctx->state_mutex);

            const char *welcome_text = "RESUMED (3s freeze) | WASD move | p pause | q leave";
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

        const char *welcome_text = "WELCOME | WASD move | p pause | q leave";
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
        state_message.width = STATE_MAP_WIDTH;
        state_message.height = STATE_MAP_HEIGHT;
        state_message.game_mode = (uint8_t)server_ctx->game_mode;

        pthread_mutex_lock(&server_ctx->state_mutex);

        game_tick(&server_ctx->game_state, now);

        if (server_ctx->game_state.should_terminate) {
            server_ctx->is_running = 0;
        }

        state_message.tick_counter_net = htonl(server_ctx->game_state.tick_counter);
        state_message.elapsed_ms_net = htonl(game_get_elapsed_ms(&server_ctx->game_state, now));
        state_message.remaining_ms_net = htonl(game_get_remaining_ms(&server_ctx->game_state, now));

        fill_state_player_list(&server_ctx->game_state, state_message.players);
        game_build_ascii_map(&server_ctx->game_state, state_message.cells);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = server_ctx->client_slots[i].client_socket_fd;
            if (fd >= 0) {
                send_message(fd, MSG_STATE, &state_message, (uint16_t)sizeof(state_message));
            }
        }

        pthread_mutex_unlock(&server_ctx->state_mutex);
    }

    return NULL;
}

int main(int argc, char **argv) {
    uint16_t port = 23456;
    game_mode_t mode = GAME_MODE_STANDARD;
    uint32_t timed_seconds = 60;
    world_type_t world_type = WORLD_EMPTY;
    const char *map_file_path = NULL;

    if (argc >= 2) port = (uint16_t)atoi(argv[1]);
    if (argc >= 3) mode = (game_mode_t)atoi(argv[2]);
    if (argc >= 4) timed_seconds = (uint32_t)atoi(argv[3]);
    if (argc >= 5) world_type = (world_type_t)atoi(argv[4]);
    if (argc >= 6) map_file_path = argv[5];

    uint32_t timed_ms = timed_seconds * 1000U;

    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
        return 1;
    }

    server_context_t server_ctx;
    server_init(&server_ctx, listen_fd, mode, timed_ms, world_type, map_file_path);
    if (world_type == WORLD_FILE) {
        const char *path = server_ctx.map_file_path[0] ? server_ctx.map_file_path : map_file_path;
        if (!path || path[0] == '\0') {
            fprintf(stderr, "server: WORLD_FILE requires map path\n");
            close(server_ctx.listen_socket_fd);
            return 1;
        }
        if (game_load_map_from_file(&server_ctx.game_state, path) != 0) {
            fprintf(stderr, "server: failed to load map: %s\n", path);
            close(server_ctx.listen_socket_fd);
            return 1;
        }
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

            int client_fd = accept(server_ctx.listen_socket_fd,
                                   (struct sockaddr*)&client_addr, &client_addr_len);
            if (client_fd >= 0) {
                pthread_mutex_lock(&server_ctx.state_mutex);

                int slot_index = -1;
                int add_rc = server_add_client(&server_ctx, client_fd, &slot_index);
                if (add_rc == 0) {
                    game_mark_client_active(&server_ctx.game_state, slot_index);
                }

                pthread_mutex_unlock(&server_ctx.state_mutex);

                if (add_rc < 0) {
                    const char *error_text = "server full";
                    send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
                    close(client_fd);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds_snapshot[i];
            if (fd >= 0 && FD_ISSET(fd, &read_fds)) {
                handle_client_message(&server_ctx, fd);

                if (socket_is_disconnected(fd)) {
                    pthread_mutex_lock(&server_ctx.state_mutex);

                    int slot_index = find_slot_by_fd(&server_ctx, fd);
                    if (slot_index >= 0) {
                        int keep = server_ctx.game_state.players[slot_index].is_paused ? 1 : 0;
                        server_close_slot_fd(&server_ctx, slot_index);
                        game_mark_client_inactive_keep_or_clear(&server_ctx.game_state, slot_index, keep);
                    }

                    pthread_mutex_unlock(&server_ctx.state_mutex);
                }
            }
        }
    }

    server_ctx.is_running = 0;
    pthread_join(tick_thread, NULL);

    pthread_mutex_lock(&server_ctx.state_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        server_close_slot_fd(&server_ctx, i);
    }
    pthread_mutex_unlock(&server_ctx.state_mutex);

    close(server_ctx.listen_socket_fd);
    pthread_mutex_destroy(&server_ctx.state_mutex);

    return 0;
}

