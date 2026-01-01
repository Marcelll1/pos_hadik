#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t type;   // network byte order
    uint16_t len;    // network byte order
} __attribute__((packed)) msg_hdr_t;

enum {
    MSG_JOIN    = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_ERROR   = 4
};

int send_all(int fd, const void *buf, size_t n);
int recv_all(int fd, void *buf, size_t n);
int send_msg(int fd, uint16_t type, const void *payload, uint16_t len);
int recv_hdr(int fd, msg_hdr_t *hdr);

#endif

