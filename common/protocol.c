#include "protocol.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

int send_all_bytes(int socket_fd, const void *buffer, size_t byte_count) {
    const char *byte_ptr = (const char*)buffer;
    size_t bytes_sent_total = 0;

    while (bytes_sent_total < byte_count) {
        ssize_t bytes_sent = send(socket_fd, byte_ptr + bytes_sent_total, byte_count - bytes_sent_total, 0);
        if (bytes_sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (bytes_sent == 0) return -1;
        bytes_sent_total += (size_t)bytes_sent;
    }
    return 0;
}

int recv_all_bytes(int socket_fd, void *buffer, size_t byte_count) {
    char *byte_ptr = (char*)buffer;
    size_t bytes_recv_total = 0;

    while (bytes_recv_total < byte_count) {
        ssize_t bytes_recv = recv(socket_fd, byte_ptr + bytes_recv_total, byte_count - bytes_recv_total, 0);
        if (bytes_recv < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (bytes_recv == 0) return -1;
        bytes_recv_total += (size_t)bytes_recv;
    }
    return 0;
}

int send_message(int socket_fd, uint16_t message_type_host, const void *payload, uint16_t payload_len_host) {
    message_header_t header_net;
    header_net.message_type_net = htons(message_type_host);
    header_net.payload_len_net  = htons(payload_len_host);

    if (send_all_bytes(socket_fd, &header_net, sizeof(header_net)) < 0) return -1;

    if (payload_len_host > 0 && payload != NULL) {
        if (send_all_bytes(socket_fd, payload, payload_len_host) < 0) return -1;
    }
    return 0;
}

int recv_message_header(int socket_fd, message_header_t *out_header_net) {
    return recv_all_bytes(socket_fd, out_header_net, sizeof(*out_header_net));
}


