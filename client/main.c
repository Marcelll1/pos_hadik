#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../common/protocol.h"

static int connect_to(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    uint16_t port = 12345;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = (uint16_t)atoi(argv[2]);

    int fd = connect_to(host, port);
    if (fd < 0) {
        fprintf(stderr, "client: connect failed: %s\n", strerror(errno));
        return 1;
    }

    // join
    const char *name = "player1";
    if (send_msg(fd, MSG_JOIN, name, (uint16_t)strlen(name)) < 0) {
        fprintf(stderr, "client: send JOIN failed\n");
        close(fd);
        return 1;
    }

    // welcome
    msg_hdr_t h;
    if (recv_hdr(fd, &h) < 0) {
        fprintf(stderr, "client: recv header failed\n");
        close(fd);
        return 1;
    }

    uint16_t type = ntohs(h.type);
    uint16_t len  = ntohs(h.len);

    char payload[512];
    if (len > 0) {
        if (len >= sizeof(payload)) {
            fprintf(stderr, "client: payload too big\n");
            close(fd);
            return 1;
        }
        if (recv_all(fd, payload, len) < 0) {
            fprintf(stderr, "client: recv payload failed\n");
            close(fd);
            return 1;
        }
        payload[len] = '\0';
    } else {
        payload[0] = '\0';
    }

    printf("client: got msg type=%u len=%u payload='%s'\n", type, len, payload);
    fflush(stdout);

    // posle input
    sleep(1);
    const char *cmd = "LEFT";
    if (send_msg(fd, MSG_TEXT, cmd, (uint16_t)strlen(cmd)) < 0) {
        fprintf(stderr, "client: send LEFT failed\n");
        close(fd);
        return 1;
    }

    printf("client: sent LEFT, staying connected...\n");
    fflush(stdout);

    // zostane pripojeny
    while (1) {
        sleep(1);
    }
}

