#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>

#include "../common/protocol.h"

#define MAX_CLIENTS 64

static int make_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void close_client(int *clients, int i) {
    if (clients[i] >= 0) {
        close(clients[i]);
        clients[i] = -1;
    }
}

static int add_client(int *clients, int cfd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] < 0) {
            clients[i] = cfd;
            return 0;
        }
    }
    return -1;
}

static void handle_client_msg(int cfd) {
    msg_hdr_t h;
    if (recv_hdr(cfd, &h) < 0) {
        // disconnect alebo chyba
        shutdown(cfd, SHUT_RDWR);
        return;
    }

    uint16_t type = ntohs(h.type);
    uint16_t len  = ntohs(h.len);

    char payload[512];
    if (len > 0) {
        if (len >= sizeof(payload)) {
            const char *err = "payload too big";
            send_msg(cfd, MSG_ERROR, err, (uint16_t)strlen(err));
            shutdown(cfd, SHUT_RDWR);
            return;
        }
        if (recv_all(cfd, payload, len) < 0) {
            shutdown(cfd, SHUT_RDWR);
            return;
        }
        payload[len] = '\0';
    } else {
        payload[0] = '\0';
    }

    // vypis a odpoved txt
    printf("server: fd=%d type=%u len=%u payload='%s'\n", cfd, type, len, payload);

    if (type == MSG_JOIN) {
        const char *msg = "WELCOME";
        send_msg(cfd, MSG_WELCOME, msg, (uint16_t)strlen(msg));
    } else {
        const char *msg = "OK";
        send_msg(cfd, MSG_TEXT, msg, (uint16_t)strlen(msg));
    }
}

int main(int argc, char **argv) {
    uint16_t port = 12345;
    if (argc >= 2) port = (uint16_t)atoi(argv[1]);

    int lfd = make_listen_socket(port);
    if (lfd < 0) {
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
        return 1;
    }
    printf("server: listening on port %u\n", port);

    int clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = -1;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0) {
                FD_SET(clients[i], &rfds);
                if (clients[i] > maxfd) maxfd = clients[i];
            }
        }

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "server: select failed: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(lfd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(lfd, (struct sockaddr*)&cli, &clen);
            if (cfd >= 0) {
                char ip[64];
                inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
                printf("server: new client fd=%d from %s:%u\n", cfd, ip, ntohs(cli.sin_port));

                if (add_client(clients, cfd) < 0) {
                    const char *err = "server full";
                    send_msg(cfd, MSG_ERROR, err, (uint16_t)strlen(err));
                    close(cfd);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int cfd = clients[i];
            if (cfd >= 0 && FD_ISSET(cfd, &rfds)) {
                handle_client_msg(cfd);

                //ak je socket zavrety, recv vrati chybu, vycisti sa
                char tmp;
                ssize_t r = recv(cfd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
                if (r == 0) {
                    printf("server: client fd=%d disconnected\n", cfd);
                    close_client(clients, i);
                }
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) close_client(clients, i);
    close(lfd);
    return 0;
}

