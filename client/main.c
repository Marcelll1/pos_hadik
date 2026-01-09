#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

#include "../common/protocol.h"

static int connect_to_server(const char *server_ip, uint16_t server_port) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return -1;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        close(socket_fd);
        errno = EINVAL;
        return -1;
    }

    if (connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
    nanosleep(&t, NULL);
}

static int recv_next_message(int server_socket_fd, uint16_t *out_type, void *payload_buf, size_t payload_buf_cap, uint16_t *out_payload_len) {
    message_header_t header_net;
    if (recv_message_header(server_socket_fd, &header_net) < 0) return -1;

    uint16_t message_type = ntohs(header_net.message_type_net);
    uint16_t payload_len  = ntohs(header_net.payload_len_net);

    if (payload_len > payload_buf_cap) {
        size_t to_read = payload_buf_cap;
        if (to_read > 0) {
            if (recv_all_bytes(server_socket_fd, payload_buf, to_read) < 0) return -1;
        }
        uint16_t rem = (uint16_t)(payload_len - to_read);
        char dump[256];
        while (rem > 0) {
            uint16_t c = rem > (uint16_t)sizeof(dump) ? (uint16_t)sizeof(dump) : rem;
            if (recv_all_bytes(server_socket_fd, dump, c) < 0) return -1;
            rem = (uint16_t)(rem - c);
        }
        *out_type = message_type;
        *out_payload_len = (uint16_t)to_read;
        return 0;
    }

    if (payload_len > 0) {
        if (recv_all_bytes(server_socket_fd, payload_buf, payload_len) < 0) return -1;
    }

    *out_type = message_type;
    *out_payload_len = payload_len;
    return 0;
}

static void trim_player_name_inplace(char *name) {
    size_t n = strlen(name);
    while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t' || name[n - 1] == '\r' || name[n - 1] == '\n')) {
        name[n - 1] = '\0';
        n--;
    }
    if (n >= STATE_NAME_MAX) name[STATE_NAME_MAX - 1] = '\0';
}

static char player_label_char(int idx) {
    if (idx >= 0 && idx < 26) return (char)('A' + idx);
    idx -= 26;
    if (idx >= 0 && idx < 10) return (char)('0' + idx);
    return '?';
}

static void render_scoreboard(const state_message_t *state) {
    printf("players:\n");
    for (int i = 0; i < STATE_MAX_PLAYERS; i++) {
        const state_player_info_t *p = &state->players[i];
        if (!p->has_joined) continue;

        unsigned score = (unsigned)ntohs(p->score_net);
        const char *alive = p->is_alive ? "alive" : "dead";
        const char *paused = p->is_paused ? "paused" : "run";
        printf("  %c name=%s score=%u %s %s\n", player_label_char(i), (const char*)p->name, score, alive, paused);
    }
}

static void print_horizontal_border(uint8_t width) {
    putchar('+');
    for (uint8_t i = 0; i < width; i++) putchar('-');
    putchar('+');
    putchar('\n');
}

static void render_state(const state_message_t *state) {
    uint32_t tick = ntohl(state->tick_counter_net);
    uint32_t elapsed_ms = ntohl(state->elapsed_ms_net);
    uint32_t remaining_ms = ntohl(state->remaining_ms_net);

    unsigned elapsed_s = elapsed_ms / 1000U;
    unsigned rem_s = remaining_ms / 1000U;

    printf("\033[H\033[J");
    if (state->game_mode == GAME_MODE_TIMED) {
        printf("tick=%u | time=%us | remaining=%us | WASD move | p pause | q leave | r respawn\n",
               (unsigned)tick, elapsed_s, rem_s);
    } else {
        printf("tick=%u | time=%us | STANDARD | WASD move | p pause | q leave | r respawn\n",
               (unsigned)tick, elapsed_s);
    }

    render_scoreboard(state);
    printf("\n");

    uint8_t width = state->width;
    uint8_t height = state->height;

    int show_border = (state->world_type == 0) ? 1 : 0;

    if (show_border) print_horizontal_border(width);

    for (uint8_t y = 0; y < height; y++) {
        if (show_border) putchar('|');
        for (uint8_t x = 0; x < width; x++) {
            putchar((char)state->cells[(size_t)y * width + x]);
        }
        if (show_border) putchar('|');
        putchar('\n');
    }

    if (show_border) print_horizontal_border(width);

    fflush(stdout);
}

static void render_game_over(const game_over_message_t *msg) {
    uint32_t elapsed_ms = ntohl(msg->elapsed_ms_net);
    unsigned elapsed_s = elapsed_ms / 1000U;

    printf("\033[H\033[J");
    printf("GAME OVER\n");
    printf("total game time: %us\n\n", elapsed_s);

    printf("results:\n");
    for (uint8_t i = 0; i < msg->player_count && i < STATE_MAX_PLAYERS; i++) {
        const game_over_player_entry_t *e = &msg->players[i];
        if (!e->has_joined) continue;
        unsigned score = (unsigned)ntohs(e->score_net);
        uint32_t snake_time_ms = ntohl(e->snake_time_ms_net);
        unsigned snake_s = snake_time_ms / 1000U;
        printf("  name=%s score=%u snake_time=%us\n", (const char*)e->name, score, snake_s);
    }

    printf("\npress ENTER to return to menu\n");
    fflush(stdout);
}

static int send_input_direction(int server_socket_fd, direction_t direction) {
    input_message_t input_msg;
    input_msg.direction = (uint8_t)direction;
    return send_message(server_socket_fd, MSG_INPUT, &input_msg, (uint16_t)sizeof(input_msg));
}

static void enable_raw_mode(struct termios *out_old) {
    tcgetattr(STDIN_FILENO, out_old);

    struct termios raw = *out_old;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void restore_terminal(const struct termios *old) {
    tcsetattr(STDIN_FILENO, TCSANOW, old);
}

static void read_line(char *buf, size_t cap) {
    if (cap == 0) return;
    if (!fgets(buf, (int)cap, stdin)) {
        buf[0] = '\0';
        return;
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
}

static int prompt_int(const char *label, int default_value) {
    char line[128];
    printf("%s (default %d): ", label, default_value);
    fflush(stdout);
    read_line(line, sizeof(line));
    if (line[0] == '\0') return default_value;
    return atoi(line);
}

static void prompt_string(const char *label, const char *default_value, char *out, size_t cap) {
    printf("%s (default %s): ", label, default_value);
    fflush(stdout);
    read_line(out, cap);
    if (out[0] == '\0') {
        strncpy(out, default_value, cap - 1);
        out[cap - 1] = '\0';
    }
}

static int start_server_process(uint16_t port, game_mode_t mode, uint32_t timed_seconds, int world_type, uint8_t map_width, uint8_t map_height, const char *map_file_path) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        (void)setsid();
        signal(SIGHUP, SIG_IGN);

        char port_str[16];
        char mode_str[16];
        char timed_str[16];
        char world_str[16];
        char width_str[16];
        char height_str[16];

        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        snprintf(mode_str, sizeof(mode_str), "%u", (unsigned)mode);
        snprintf(timed_str, sizeof(timed_str), "%u", (unsigned)timed_seconds);
        snprintf(world_str, sizeof(world_str), "%u", (unsigned)world_type);
        snprintf(width_str, sizeof(width_str), "%u", (unsigned)map_width);
        snprintf(height_str, sizeof(height_str), "%u", (unsigned)map_height);

        if (world_type == 1 && map_file_path && map_file_path[0] != '\0') {
            execl("./server_bin", "server_bin", port_str, mode_str, timed_str, world_str, width_str, height_str, map_file_path, (char*)NULL);
        } else {
            execl("./server_bin", "server_bin", port_str, mode_str, timed_str, world_str, width_str, height_str, (char*)NULL);
        }

        perror("exec server_bin failed");
        _exit(127);
    }

    return 0;
}

static int request_server_shutdown(const char *server_ip, uint16_t server_port) {
    int fd = connect_to_server(server_ip, server_port);
    if (fd < 0) {
        fprintf(stderr, "client: connect failed: %s\n", strerror(errno));
        return -1;
    }
    if (send_message(fd, MSG_SHUTDOWN, NULL, 0) < 0) {
        fprintf(stderr, "client: send shutdown failed\n");
        close(fd);
        return -1;
    }
    close(fd);
    printf("client: shutdown request sent\n");
    return 0;
}

typedef struct {
    int has_paused_session;
    char server_ip[64];
    uint16_t server_port;
    char player_name[64];
} paused_session_t;

static int run_game_session(const char *server_ip, uint16_t server_port, const char *player_name_raw, paused_session_t *paused_session) {
    char player_name[64];
    strncpy(player_name, player_name_raw, sizeof(player_name) - 1);
    player_name[sizeof(player_name) - 1] = '\0';
    trim_player_name_inplace(player_name);

    int server_socket_fd = connect_to_server(server_ip, server_port);
    if (server_socket_fd < 0) {
        fprintf(stderr, "client: connect failed: %s\n", strerror(errno));
        return -1;
    }

    if (send_message(server_socket_fd, MSG_JOIN, player_name, (uint16_t)strlen(player_name)) < 0) {
        fprintf(stderr, "client: send JOIN failed\n");
        close(server_socket_fd);
        return -1;
    }

    struct termios old_term;
    enable_raw_mode(&old_term);

    int is_running = 1;
    int did_pause = 0;
    int got_game_over = 0;
    game_over_message_t game_over;
    memset(&game_over, 0, sizeof(game_over));

    while (is_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket_fd, &read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        int max_fd = server_socket_fd > STDIN_FILENO ? server_socket_fd : STDIN_FILENO;

        int rc = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n == 1) {
                if (ch == 'q' || ch == 'Q') {
                    (void)send_message(server_socket_fd, MSG_LEAVE, NULL, 0);
                    is_running = 0;
                } else if (ch == 'p' || ch == 'P') {
                    (void)send_message(server_socket_fd, MSG_PAUSE, NULL, 0);
                    did_pause = 1;
                    is_running = 0;
                } else if (ch == 'r' || ch == 'R') {
                    (void)send_message(server_socket_fd, MSG_RESPAWN, NULL, 0);
                } else if (ch == 'w' || ch == 'W') {
                    send_input_direction(server_socket_fd, DIR_UP);
                } else if (ch == 'd' || ch == 'D') {
                    send_input_direction(server_socket_fd, DIR_RIGHT);
                } else if (ch == 's' || ch == 'S') {
                    send_input_direction(server_socket_fd, DIR_DOWN);
                } else if (ch == 'a' || ch == 'A') {
                    send_input_direction(server_socket_fd, DIR_LEFT);
                }
            }
        }

        if (FD_ISSET(server_socket_fd, &read_fds)) {
            uint16_t msg_type = 0;
            uint16_t payload_len = 0;

            uint8_t payload_buf[sizeof(state_message_t) > sizeof(game_over_message_t) ? sizeof(state_message_t) : sizeof(game_over_message_t)];

            if (recv_next_message(server_socket_fd, &msg_type, payload_buf, sizeof(payload_buf), &payload_len) < 0) {
                break;
            }

            if (msg_type == MSG_STATE) {
                if (payload_len == sizeof(state_message_t)) {
                    state_message_t state;
                    memcpy(&state, payload_buf, sizeof(state));
                    render_state(&state);
                }
            } else if (msg_type == MSG_GAME_OVER) {
                if (payload_len == sizeof(game_over_message_t)) {
                    memcpy(&game_over, payload_buf, sizeof(game_over));
                    got_game_over = 1;
                    is_running = 0;
                }
            }
        }
    }

    restore_terminal(&old_term);
    close(server_socket_fd);

    if (got_game_over) {
        render_game_over(&game_over);
        char line[8];
        read_line(line, sizeof(line));
        paused_session->has_paused_session = 0;
        return 0;
    }

    if (did_pause) {
        paused_session->has_paused_session = 1;
        strncpy(paused_session->server_ip, server_ip, sizeof(paused_session->server_ip) - 1);
        paused_session->server_ip[sizeof(paused_session->server_ip) - 1] = '\0';
        paused_session->server_port = server_port;
        strncpy(paused_session->player_name, player_name, sizeof(paused_session->player_name) - 1);
        paused_session->player_name[sizeof(paused_session->player_name) - 1] = '\0';
        printf("\nclient: paused -> back to menu\n");
    } else {
        paused_session->has_paused_session = 0;
        printf("\nclient: session ended\n");
    }

    return 0;
}

static void print_menu(const paused_session_t *paused) {
    printf("\n=== MENU ===\n");
    printf("1) Nova hra (spusti server)\n");
    printf("2) Pripojit sa na existujuci server\n");
    if (paused->has_paused_session) printf("3) Pokracovat v hre (resume)\n");
    else printf("3) Pokracovat v hre (resume) [nie je dostupne]\n");
    printf("4) Koniec\n");
    printf("5) Ukoncit server (shutdown)\n");
    printf("Vyber: ");
    fflush(stdout);
}

int main(void) {
    paused_session_t paused;
    memset(&paused, 0, sizeof(paused));

    while (1) {
        print_menu(&paused);

        char choice_line[32];
        read_line(choice_line, sizeof(choice_line));
        int choice = atoi(choice_line);

        if (choice == 1) {
            char player_name[64];
            char server_ip[64];

            prompt_string("Meno hraca", "player1", player_name, sizeof(player_name));
            trim_player_name_inplace(player_name);

            prompt_string("IP (pre local server daj 127.0.0.1)", "127.0.0.1", server_ip, sizeof(server_ip));

            int port_i = prompt_int("Port", 23456);
            if (port_i <= 0 || port_i > 65535) {
                printf("Zly port.\n");
                continue;
            }
            uint16_t port = (uint16_t)port_i;

            int mode_i = prompt_int("Rezim (0=standard 10s, 1=casovy)", 0);
            game_mode_t mode = (mode_i == 1) ? GAME_MODE_TIMED : GAME_MODE_STANDARD;

            uint32_t timed_seconds = 60;
            if (mode == GAME_MODE_TIMED) {
                int t = prompt_int("Dlzka hry v sekundach", 60);
                if (t <= 0) t = 60;
                timed_seconds = (uint32_t)t;
            }

            int world_i = prompt_int("Svet (0=empty wrap, 1=prekazky zo suboru)", 0);
            int world_type = (world_i == 1) ? 1 : 0;

            int width_i = prompt_int("Sirka mapy (5-80)", 40);
            int height_i = prompt_int("Vyska mapy (5-40)", 20);

            if (width_i < 5) width_i = 5;
            if (height_i < 5) height_i = 5;
            if (width_i > STATE_MAX_WIDTH) width_i = STATE_MAX_WIDTH;
            if (height_i > STATE_MAX_HEIGHT) height_i = STATE_MAX_HEIGHT;

            uint8_t map_width = (uint8_t)width_i;
            uint8_t map_height = (uint8_t)height_i;

            char map_path[256];
            map_path[0] = '\0';
            const char *map_file_path = NULL;
            if (world_type == 1) {
                prompt_string("Cesta k mape", "maps/world1.map", map_path, sizeof(map_path));
                map_file_path = map_path;
            }

            printf("Spustam server na porte %u...\n", (unsigned)port);
            if (start_server_process(port, mode, timed_seconds, world_type, map_width, map_height, map_file_path) < 0) {
                printf("Nepodarilo sa spustit server (fork/exec).\n");
                continue;
            }

            sleep_ms(200);
            (void)run_game_session(server_ip, port, player_name, &paused);

        } else if (choice == 2) {
            char player_name[64];
            char server_ip[64];

            prompt_string("Meno hraca", "player1", player_name, sizeof(player_name));
            trim_player_name_inplace(player_name);

            prompt_string("IP servera", "127.0.0.1", server_ip, sizeof(server_ip));

            int port_i = prompt_int("Port", 23456);
            if (port_i <= 0 || port_i > 65535) {
                printf("Zly port.\n");
                continue;
            }
            uint16_t port = (uint16_t)port_i;

            (void)run_game_session(server_ip, port, player_name, &paused);

        } else if (choice == 3) {
            if (!paused.has_paused_session) {
                printf("Nie je co pokracovat (nebola pauza).\n");
                continue;
            }
            (void)run_game_session(paused.server_ip, paused.server_port, paused.player_name, &paused);

        } else if (choice == 4) {
            break;

        } else if (choice == 5) {
            char server_ip[64];
            prompt_string("IP servera", "127.0.0.1", server_ip, sizeof(server_ip));

            int port_i = prompt_int("Port", 23456);
            if (port_i <= 0 || port_i > 65535) {
                printf("Zly port.\n");
                continue;
            }
            uint16_t port = (uint16_t)port_i;

            (void)request_server_shutdown(server_ip, port);

        } else {
            printf("Zly vyber.\n");
        }
    }

    return 0;
}

