#include "protocol.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

int send_all_bytes(int socket_fd, const void *buffer, size_t byte_count) {
    const unsigned char *byte_ptr = (const unsigned char*)buffer;
    size_t sent_total = 0;

    while (sent_total < byte_count) {
        ssize_t sent_now = send(socket_fd, byte_ptr + sent_total, byte_count - sent_total, 0);
        if (sent_now < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent_now == 0) return -1;
        sent_total += (size_t)sent_now;
    }

    return 0;
}

int recv_all_bytes(int socket_fd, void *buffer, size_t byte_count) {
    unsigned char *byte_ptr = (unsigned char*)buffer;
    size_t recv_total = 0;

    while (recv_total < byte_count) {
        ssize_t recv_now = recv(socket_fd, byte_ptr + recv_total, byte_count - recv_total, 0);
        if (recv_now < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (recv_now == 0) return -1;
        recv_total += (size_t)recv_now;
    }

    return 0;
}

int send_message(int socket_fd, uint16_t message_type_host, const void *payload, uint16_t payload_len_host) {
    message_header_t header_net;
    header_net.message_type_net = htons(message_type_host);
    header_net.payload_len_net = htons(payload_len_host);

    if (send_all_bytes(socket_fd, &header_net, sizeof(header_net)) < 0) return -1;

    if (payload_len_host > 0 && payload != NULL) {
        if (send_all_bytes(socket_fd, payload, payload_len_host) < 0) return -1;
    }

    return 0;
}

int recv_message_header(int socket_fd, message_header_t *out_header_net) {
    if (!out_header_net) return -1;
    if (recv_all_bytes(socket_fd, out_header_net, sizeof(*out_header_net)) < 0) return -1;
    return 0;
}

