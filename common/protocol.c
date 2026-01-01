#include "protocol.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>

int send_all(int fd, const void *buf, size_t n) {
    const char *p = (const char*)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t n) {
    char *p = (char*)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

int send_msg(int fd, uint16_t type, const void *payload, uint16_t len) {
    msg_hdr_t h;
    h.type = htons(type);
    h.len  = htons(len);
    if (send_all(fd, &h, sizeof(h)) < 0) return -1;
    if (len > 0 && payload != NULL) {
        if (send_all(fd, payload, len) < 0) return -1;
    }
    return 0;
}

int recv_hdr(int fd, msg_hdr_t *hdr) {
    return recv_all(fd, hdr, sizeof(*hdr));
}

