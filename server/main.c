#define _POSIX_C_SOURCE 200809L

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


#define MAX_CLIENTS 64
#define MAX_JOIN_NAME 256

typedef struct {
    int client_socket_fd;
    direction_t last_direction;
} client_slot_t;

typedef struct {
    int listen_socket_fd;

    client_slot_t client_slots[MAX_CLIENTS];
    pthread_mutex_t client_slots_mutex;

    int is_running;
    uint32_t tick_counter;
    uint32_t tick_interval_ms;
} server_context_t;

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

static void server_init(server_context_t *server_ctx, int listen_fd) {
    server_ctx->listen_socket_fd = listen_fd;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        server_ctx->client_slots[i].client_socket_fd = -1;
        server_ctx->client_slots[i].last_direction = DIR_RIGHT;
    }

    pthread_mutex_init(&server_ctx->client_slots_mutex, NULL);

    server_ctx->is_running = 1;
    server_ctx->tick_counter = 0;
    server_ctx->tick_interval_ms = 200;
}

static int server_add_client(server_context_t *server_ctx, int client_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server_ctx->client_slots[i].client_socket_fd < 0) {
            server_ctx->client_slots[i].client_socket_fd = client_fd;
            server_ctx->client_slots[i].last_direction = DIR_RIGHT;
            return 0;
        }
    }
    return -1;
}

static void server_remove_client(server_context_t *server_ctx, int slot_index) {
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

static void handle_client_message(server_context_t *server_ctx, int client_fd) {
    message_header_t header_net;
    if (recv_message_header(client_fd, &header_net) < 0) {
        shutdown(client_fd, SHUT_RDWR);
        return;
    }

    uint16_t message_type = ntohs(header_net.message_type_net);
    uint16_t payload_len  = ntohs(header_net.payload_len_net);

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

        printf("server: fd=%d JOIN name='%s'\n", client_fd, player_name);

        const char *welcome_text = "WELCOME";
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

        pthread_mutex_lock(&server_ctx->client_slots_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (server_ctx->client_slots[i].client_socket_fd == client_fd) {
                server_ctx->client_slots[i].last_direction = (direction_t)input_message.direction;
                printf("server: fd=%d INPUT direction=%u\n", client_fd, (unsigned)input_message.direction);
                break;
            }
        }
        pthread_mutex_unlock(&server_ctx->client_slots_mutex);
        return;
    }

    // neznamy typ payload zahod a odpovedz errorom
    if (payload_len > 0) {
        drain_payload_if_any(client_fd, payload_len);
    }
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

        pthread_mutex_lock(&server_ctx->client_slots_mutex);

        server_ctx->tick_counter++;

        state_message_t state_message;
        state_message.tick_counter_net = htonl(server_ctx->tick_counter);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int client_fd = server_ctx->client_slots[i].client_socket_fd;
            if (client_fd >= 0) {
                send_message(client_fd, MSG_STATE, &state_message, (uint16_t)sizeof(state_message));
            }
        }

        pthread_mutex_unlock(&server_ctx->client_slots_mutex);
    }

    return NULL;
}

int main(int argc, char **argv) {
    uint16_t port = 12345;
    if (argc >= 2) port = (uint16_t)atoi(argv[1]);

    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
        return 1;
    }

    server_context_t server_ctx;
    server_init(&server_ctx, listen_fd);

    printf("server: listening on port %u\n", port);

    pthread_t tick_thread;
    if (pthread_create(&tick_thread, NULL, server_tick_thread, &server_ctx) != 0) {
        fprintf(stderr, "server: pthread_create failed\n");
        close(server_ctx.listen_socket_fd);
        return 1;
    }

    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        FD_SET(server_ctx.listen_socket_fd, &read_fds);
        int max_fd = server_ctx.listen_socket_fd;

               int client_fds_snapshot[MAX_CLIENTS];
        pthread_mutex_lock(&server_ctx.client_slots_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_fds_snapshot[i] = server_ctx.client_slots[i].client_socket_fd;
        }
        pthread_mutex_unlock(&server_ctx.client_slots_mutex);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int client_fd = client_fds_snapshot[i];
            if (client_fd >= 0) {
                FD_SET(client_fd, &read_fds);
                if (client_fd > max_fd) max_fd = client_fd;
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
                char client_ip[64];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                printf("server: new client fd=%d from %s:%u\n",
                       client_fd, client_ip, ntohs(client_addr.sin_port));

                pthread_mutex_lock(&server_ctx.client_slots_mutex);
                int add_rc = server_add_client(&server_ctx, client_fd);
                pthread_mutex_unlock(&server_ctx.client_slots_mutex);

                if (add_rc < 0) {
                    const char *error_text = "server full";
                    send_message(client_fd, MSG_ERROR, error_text, (uint16_t)strlen(error_text));
                    close(client_fd);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int client_fd = client_fds_snapshot[i];
            if (client_fd >= 0 && FD_ISSET(client_fd, &read_fds)) {
                handle_client_message(&server_ctx, client_fd);

                if (socket_is_disconnected(client_fd)) {
                    printf("server: client fd=%d disconnected\n", client_fd);
                    pthread_mutex_lock(&server_ctx.client_slots_mutex);
                    // najdi slot podla fd (bezpecne, lebo snapshot mohol byt stary)
                    for (int s = 0; s < MAX_CLIENTS; s++) {
                        if (server_ctx.client_slots[s].client_socket_fd == client_fd) {
                            server_remove_client(&server_ctx, s);
                            break;
                        }
                    }
                    pthread_mutex_unlock(&server_ctx.client_slots_mutex);
                }
            }
        }
    }

    server_ctx.is_running = 0;
       
    pthread_mutex_lock(&server_ctx.client_slots_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        server_remove_client(&server_ctx, i);
    }
    pthread_mutex_unlock(&server_ctx.client_slots_mutex);

    close(server_ctx.listen_socket_fd);
    pthread_mutex_destroy(&server_ctx.client_slots_mutex);
    return 0;
}

