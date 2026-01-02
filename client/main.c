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

    // textove spravy
    if (payload_len > 0) {
        char tmp[512];
        uint16_t n = payload_len > (uint16_t)(sizeof(tmp) - 1) ? (uint16_t)(sizeof(tmp) - 1) : payload_len;
        if (recv_all_bytes(server_socket_fd, tmp, n) < 0) return -1;
        tmp[n] = '\0';
        // ak bola správa dlhšia, zvyšok zahod
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

static void render_state(const state_message_t *state) {
    uint32_t tick = ntohl(state->tick_counter_net);

    // clear screen + home cursor
    printf("\033[H\033[J");
    printf("tick=%u | ovladanie: WASD, q=quit | server state\n", (unsigned)tick);

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

int main(int argc, char **argv) {
    const char *server_ip = "127.0.0.1";
    uint16_t server_port = 12345;

    if (argc >= 2) server_ip = argv[1];
    if (argc >= 3) server_port = (uint16_t)atoi(argv[2]);

    int server_socket_fd = connect_to_server(server_ip, server_port);
    if (server_socket_fd < 0) {
        fprintf(stderr, "client: connect failed: %s\n", strerror(errno));
        return 1;
    }

    // JOIN
    const char *player_name = "player1";
    if (send_message(server_socket_fd, MSG_JOIN, player_name, (uint16_t)strlen(player_name)) < 0) {
        fprintf(stderr, "client: send JOIN failed\n");
        close(server_socket_fd);
        return 1;
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
    printf("\nclient: bye\n");
    return 0;
}

