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

static int recv_state_message(int server_socket_fd, state_message_t *out_state) {
    message_header_t header_net;
    if (recv_message_header(server_socket_fd, &header_net) < 0) return -1;

    uint16_t message_type = ntohs(header_net.message_type_net);
    uint16_t payload_len  = ntohs(header_net.payload_len_net);

    if (message_type == MSG_STATE) {
        if (payload_len != sizeof(state_message_t)) return -1;
        if (recv_all_bytes(server_socket_fd, out_state, sizeof(state_message_t)) < 0) return -1;
        return 1;
    }

    if (payload_len > 0) {
        char tmp[512];
        uint16_t n = payload_len > (uint16_t)(sizeof(tmp) - 1) ? (uint16_t)(sizeof(tmp) - 1) : payload_len;
        if (recv_all_bytes(server_socket_fd, tmp, n) < 0) return -1;
        tmp[n] = '\0';

        if (payload_len > n) {
            uint16_t rem = (uint16_t)(payload_len - n);
            char dump[256];
            while (rem > 0) {
                uint16_t c = rem > (uint16_t)sizeof(dump) ? (uint16_t)sizeof(dump) : rem;
                if (recv_all_bytes(server_socket_fd, dump, c) < 0) return -1;
                rem = (uint16_t)(rem - c);
            }
        }

        printf("client: server msg type=%u: %s\n", (unsigned)message_type, tmp);
        fflush(stdout);
        return 0;
    }

    return 0;
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
        if (!p->is_used || !p->has_joined) continue;

        unsigned score = (unsigned)ntohs(p->score_net);
        const char *status = p->is_alive ? "alive" : "dead";
        printf("  %c  name=%s  score=%u  %s\n", player_label_char(i), (const char*)p->name, score, status);
    }
}

static void render_state(const state_message_t *state) {
    uint32_t tick = ntohl(state->tick_counter_net);

    printf("\033[H\033[J");
    printf("tick=%u | ovladanie: WASD, q=quit\n", (unsigned)tick);
    render_scoreboard(state);
    printf("\n");

    uint8_t width = state->width;
    uint8_t height = state->height;

    for (uint8_t y = 0; y < height; y++) {
        for (uint8_t x = 0; x < width; x++) {
            putchar((char)state->cells[y * width + x]);
        }
        putchar('\n');
    }
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

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
    nanosleep(&t, NULL);
}

static int start_server_process(uint16_t port) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        (void)setsid();
        signal(SIGHUP, SIG_IGN);

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

        execl("./server_bin", "server_bin", port_str, (char*)NULL);

        perror("exec server_bin failed");
        _exit(127);
    }

    return 0;
}

static int run_game_session(const char *server_ip, uint16_t server_port, const char *player_name) {
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
                    is_running = 0;
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
            state_message_t state;
            int got_state = recv_state_message(server_socket_fd, &state);
            if (got_state < 0) {
                fprintf(stderr, "client: disconnected\n");
                break;
            }
            if (got_state == 1) {
                render_state(&state);
            }
        }
    }

    restore_terminal(&old_term);
    close(server_socket_fd);
    printf("\nclient: session ended\n");
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

static void print_menu(void) {
    printf("\n=== MENU ===\n");
    printf("1) Nova hra (spusti server)\n");
    printf("2) Pripojit sa na existujuci server\n");
    printf("3) Koniec\n");
    printf("4) Ukoncit server (posle shutdown)\n");
    printf("Vyber: ");
    fflush(stdout);
}

int main(void) {
    while (1) {
        print_menu();

        char choice_line[32];
        read_line(choice_line, sizeof(choice_line));
        int choice = atoi(choice_line);

        if (choice == 1) {
            char player_name[64];
            char server_ip[64];

            prompt_string("Meno hraca", "player1", player_name, sizeof(player_name));
            prompt_string("IP (pre local server daj 127.0.0.1)", "127.0.0.1", server_ip, sizeof(server_ip));

            int port_i = prompt_int("Port", 23456);
            if (port_i <= 0 || port_i > 65535) {
                printf("Zly port.\n");
                continue;
            }
            uint16_t port = (uint16_t)port_i;

            printf("Spustam server na porte %u...\n", (unsigned)port);
            if (start_server_process(port) < 0) {
                printf("Nepodarilo sa spustit server (fork/exec).\n");
                continue;
            }

            sleep_ms(200);

            (void)run_game_session(server_ip, port, player_name);
        } else if (choice == 2) {
            char player_name[64];
            char server_ip[64];

            prompt_string("Meno hraca", "player1", player_name, sizeof(player_name));
            prompt_string("IP servera", "127.0.0.1", server_ip, sizeof(server_ip));

            int port_i = prompt_int("Port", 23456);
            if (port_i <= 0 || port_i > 65535) {
                printf("Zly port.\n");
                continue;
            }
            uint16_t port = (uint16_t)port_i;

            (void)run_game_session(server_ip, port, player_name);
        } else if (choice == 3) {
            printf("Koniec.\n");
            break;
        } else if (choice == 4) {
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


