#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../common/protocol.h"

#define MAX_PAYLOAD 512

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

static int recv_one_message_and_print(int server_socket_fd) {
    message_header_t header_net;
    if (recv_message_header(server_socket_fd, &header_net) < 0) {
        return -1;
    }

    uint16_t message_type = ntohs(header_net.message_type_net);
    uint16_t payload_len  = ntohs(header_net.payload_len_net);

    if (message_type == MSG_STATE) {
        if (payload_len != sizeof(state_message_t)) return -1;

        state_message_t state_message;
        if (recv_all_bytes(server_socket_fd, &state_message, sizeof(state_message)) < 0) return -1;

        uint32_t tick_counter = ntohl(state_message.tick_counter_net);
        printf("client: got STATE tick=%u\n", (unsigned)tick_counter);
        fflush(stdout);
        return 0;
    }

    if (message_type == MSG_WELCOME || message_type == MSG_ERROR || message_type == MSG_TEXT) {
        char payload_buffer[MAX_PAYLOAD + 1];
        if (payload_len > MAX_PAYLOAD) return -1;

        if (payload_len > 0) {
            if (recv_all_bytes(server_socket_fd, payload_buffer, payload_len) < 0) return -1;
            payload_buffer[payload_len] = '\0';
        } else {
            payload_buffer[0] = '\0';
        }

        printf("client: got msg type=%u payload='%s'\n", (unsigned)message_type, payload_buffer);
        fflush(stdout);
        return 0;
    }

    // neznamy typ: len zahod payload
    if (payload_len > 0) {
        char tmp[256];
        uint16_t remaining = payload_len;
        while (remaining > 0) {
            uint16_t chunk = remaining > (uint16_t)sizeof(tmp) ? (uint16_t)sizeof(tmp) : remaining;
            if (recv_all_bytes(server_socket_fd, tmp, chunk) < 0) return -1;
            remaining = (uint16_t)(remaining - chunk);
        }
    }

    return 0;
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

    // cakat na WELCOME
    if (recv_one_message_and_print(server_socket_fd) < 0) {
        fprintf(stderr, "client: failed to receive WELCOME\n");
        close(server_socket_fd);
        return 1;
    }

    // poslat INPUT (DIR_LEFT) po chvilke
    sleep(1);
    input_message_t input_message;
    input_message.direction = DIR_LEFT;

    if (send_message(server_socket_fd, MSG_INPUT, &input_message, (uint16_t)sizeof(input_message)) < 0) {
        fprintf(stderr, "client: send INPUT failed\n");
        close(server_socket_fd);
        return 1;
    }

    printf("client: sent INPUT DIR_LEFT, now receiving STATE...\n");
    fflush(stdout);

    // prijimat STATE spravy donekonecna
    while (1) {
        if (recv_one_message_and_print(server_socket_fd) < 0) {
            fprintf(stderr, "client: disconnected or recv failed\n");
            break;
        }
    }

    close(server_socket_fd);
    return 0;
}

