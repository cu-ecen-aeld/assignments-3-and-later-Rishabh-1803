#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUF_SIZE 1024
#define STORE_PATH "/var/tmp/aesdsocketdata"
#define SOCKET_PORT "9000"

bool exit_flag = false;

int sock_fd = -1;
int store_fd = -1;

static void on_signal(int sig_num) {
    if (sig_num == SIGTERM || sig_num == SIGINT) {
        syslog(LOG_DEBUG, "Caught signal, exiting");
        if (sock_fd > 0)
            close(sock_fd);
        if (store_fd > 0) {
            close(store_fd);
            remove(STORE_PATH);
        }
        exit(0);
    }
}

// initialize socket, start listen
int init_socket() {
    int r;
    struct addrinfo hints;
    struct addrinfo *addr, *rp;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    // Get addr options
    r = getaddrinfo(NULL, SOCKET_PORT, &hints, &addr);
    if (r) {
        syslog(LOG_ERR, "getaddrinfo fail: %s(%d)", strerror(r), r);
        return r;
    }

    for (rp = addr; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd == -1)
            continue;

        const int enable = 1;
        if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0 ||
            setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0)
            continue;

        r = bind(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (!r)
            break; /* Success */
            
        close(sock_fd);
    }

    freeaddrinfo(addr);

    // No address succeeded
    if (!rp) {
        syslog(LOG_ERR, "socket bind fail");
        return ENOTCONN;
    }

    if (listen(sock_fd, 50) == -1) {
        syslog(LOG_ERR, "getaddrinfo fail: %s(%d)", strerror(r), r);
        return r;
    }

    return 0;
}

void process_conn() {
    int r;
    int conn_fd;
    socklen_t addr_len;
    struct sockaddr_storage peer_addr;
    char host[NI_MAXHOST], service[NI_MAXSERV];
    char buf[BUF_SIZE];
    ssize_t nread, nwrite;
    struct stat store_stat;

    addr_len = sizeof(peer_addr);
    conn_fd = accept(sock_fd, (struct sockaddr *) &peer_addr, &addr_len);
    if (conn_fd == -1) {
        // socket marked as nonblocking so accept fails if there is no waiting connections
        return;
    }

    r = getnameinfo((struct sockaddr *) &peer_addr, addr_len, host, NI_MAXHOST,
                    service, NI_MAXSERV, NI_NUMERICSERV);
    if (r) {
        syslog(LOG_ERR, "getnameinfo() fail: %s (%d)", gai_strerror(r), r);
    } else {
        syslog(LOG_INFO, "Accepted connection from %s", host);
    }

    lseek(store_fd, 0, SEEK_END);
    do {
        memset(buf, 0, sizeof(buf));
        nread = recv(conn_fd, buf, BUF_SIZE, 0);
        if (nread == -1) {
            syslog(LOG_ERR, "recv() fail: %s(%d)", strerror(errno), errno);
            break;
        } else if (nread > 0) {
            syslog(LOG_INFO, "recv() got %s", buf);
            write(store_fd, buf, nread);
        }
    } while (nread > 0 && buf[strlen(buf)-1] != '\n');
    
    stat(STORE_PATH, &store_stat);
    lseek(store_fd, 0, SEEK_SET);
    
    do {
        memset(buf, 0, sizeof(buf));
        nread = read(store_fd, buf, BUF_SIZE);
        if (nread == -1) {
            syslog(LOG_ERR, "recv() fail: %s(%d)", strerror(errno), errno);
            break;
        }
        if (nread > 0) {
            nwrite = send(conn_fd, buf, nread, 0);
            if (nwrite != nread) {
                syslog(LOG_ERR, "send() fail: %s(%d)", strerror(errno), errno);
            } else {
                syslog(LOG_INFO, "send() passed %ld bytes to peer", nwrite);
            }
        }
    } while (nread > 0);

    close(conn_fd);
    syslog(LOG_INFO, "Closed connection from %s", host);
}

int main(int argc, char** argv) {
    int r;
    struct sigaction sig_act;

    // daemonize
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        if (fork()) {
            exit(EXIT_SUCCESS); /* parent */
        }
        /* child */
        setsid();
    }

    openlog(argv[0], LOG_PERROR, LOG_DEBUG);
    syslog(LOG_INFO, "Starting socket server on %s", SOCKET_PORT);

    memset(&sig_act, 0, sizeof(struct sigaction));
    sig_act.sa_handler = on_signal;

    if (sigaction(SIGINT, &sig_act, 0)
     || sigaction(SIGTERM, &sig_act, 0)) {
        syslog(LOG_ERR, "sigaction fail: %s(%d)", strerror(errno), errno);
        exit(EXIT_FAILURE);
    }

    r = init_socket();
    if (r) {
        syslog(LOG_ERR, "Failed to initialize socket: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }

    store_fd = open(STORE_PATH, O_RDWR | O_CREAT);

    for (;;) {
        syslog(LOG_DEBUG, "process_conn");
        process_conn();
    }

    syslog(LOG_DEBUG, "exit");
}
