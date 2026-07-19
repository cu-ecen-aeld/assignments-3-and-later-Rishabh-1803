#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <unistd.h>

#define POLL_PERIOD_MS 100
#define POLL_PERIOD_US (1000 * POLL_PERIOD_MS)
#define POLL_PERIOD_NS (1000 * POLL_PERIOD_US)

#define BUF_SIZE 1024
#define STORE_PATH "/var/tmp/aesdsocketdata"
#define SOCKET_PORT "9000"

typedef struct context {
    bool exit_flag;

    int sock_fd;
    int conn_fd;

    timer_t timer_id;
    pthread_mutex_t mtx;
} context_t;

static context_t ctx;

// SIGTERM and SIGINT signals handler
static void on_signal(int sig_num) {
    int r;

    if (sig_num == SIGTERM || sig_num == SIGINT) {
        syslog(LOG_DEBUG, "Caught signal, exiting");
        r = pthread_mutex_lock(&ctx.mtx);
        if (r) {
            syslog(LOG_ERR, "pthread_mutex_lock() fail: %s (%d)", strerror(r), r);
            return;
        }

        ctx.exit_flag = true;
        r = pthread_mutex_unlock(&ctx.mtx);
        if (r) {
            syslog(LOG_ERR, "pthread_mutex_unlock() fail: %s (%d)", strerror(r), r);
        }
    }
}

// init context variables with defaults
static void init(context_t* ctx) {
    ctx->exit_flag = false;
    ctx->sock_fd = -1;
    ctx->conn_fd = -1;
    ctx->timer_id = NULL;
    pthread_mutex_init(&ctx->mtx, NULL);
}

// release aquired resources 
static void clean(context_t* ctx) {
    if (ctx->sock_fd != -1) {
        close(ctx->sock_fd);
    }
    if (ctx->conn_fd != -1) {
        close(ctx->conn_fd);
    }
    if (ctx->timer_id) {
        timer_delete(ctx->timer_id);
    }
    pthread_mutex_destroy(&ctx->mtx);

    if (remove(STORE_PATH) == 0) {
        printf("File deleted successfully.\n");
    } else {
        printf("Error: Unable to delete the file.\n");
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
        ctx.sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (ctx.sock_fd == -1)
            continue;

        // where socketfd is the socket you want to make non-blocking
        r = fcntl(ctx.sock_fd, F_SETFL, fcntl(ctx.sock_fd, F_GETFL, 0) | O_NONBLOCK);
        if (r == -1){
            syslog(LOG_ERR, "fcntl fail: %s(%d)", strerror(errno), errno);
        }

        r = bind(ctx.sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (!r)
            break; /* Success */
            
        close(ctx.sock_fd);
    }

    freeaddrinfo(addr);

    // No address succeeded
    if (!rp) {
        syslog(LOG_ERR, "socket bind fail");
        return ENOTCONN;
    }

    if (listen(ctx.sock_fd, 50) == -1) {
        syslog(LOG_ERR, "getaddrinfo fail: %s(%d)", strerror(r), r);
        return r;
    }

    return 0;
}

// try accept connection
void try_accept(context_t* ctx) {
    int r;
    socklen_t peer_addr_len;
    struct sockaddr_storage peer_addr;
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];

    peer_addr_len = sizeof(peer_addr);
    ctx->conn_fd = accept(ctx->sock_fd, (struct sockaddr *) &peer_addr, &peer_addr_len);
    if (ctx->conn_fd == -1) {
        // socket marked as nonblocking so accept fails if there is no waiting connections
        return;
    }

    r = getnameinfo((struct sockaddr *) &peer_addr, peer_addr_len, host, NI_MAXHOST,
                    service, NI_MAXSERV, NI_NUMERICSERV);
    if (r) {
        syslog(LOG_ERR, "getnameinfo() fail: %s (%d)", gai_strerror(r), r);
    } else {
        syslog(LOG_INFO, "Accepted connection from %s", host);
    }
}

// try read incoming data
void try_read(context_t* ctx) {
    char buf[BUF_SIZE];
    ssize_t nread;
    
    FILE *fp; 
    
    fp = fopen(STORE_PATH, "a");
    nread = 1;
    while (nread > 0) {
        nread = recv(ctx->conn_fd, buf, BUF_SIZE, 0);
        fwrite(buf, 1, nread, fp);
    }
    fclose(fp);

    memset(buf, 0, sizeof(buf));

    fp = fopen(STORE_PATH, "r");
    while (fgets(buf, BUF_SIZE, fp)) {
        syslog(LOG_DEBUG, "Sending response: %s", buf);
        ssize_t n = send(ctx->sock_fd, buf, strlen(buf), 0);
        if (n == -1) {
            syslog(LOG_ERR, "Error sending response: %s (%d)", strerror(errno), errno);
            break;
        } else {
            syslog(LOG_INFO, "Sent %ld bytes", n);
        }
    }
    fclose(fp);

    close(ctx->conn_fd);
    ctx->conn_fd = -1;
}

// function to call by timer thread
static void timer_thread(union sigval sigval) {
    int r;
    context_t* ctx = (context_t*) sigval.sival_ptr;

    if (pthread_mutex_trylock(&ctx->mtx)) {
        return;
    }

    if (!ctx->exit_flag) {
        if (ctx->conn_fd == -1) {
            try_accept(ctx);
        } else {
            try_read(ctx);
        }
    }

    r = pthread_mutex_unlock(&ctx->mtx);
    if (r) {
        syslog(LOG_ERR, "pthread_mutex_unlock() fail: %s (%d)", strerror(r), r);
    }
}

// setup POSIX timer thread
int setup_loop(context_t* ctx) {
    struct itimerspec its;
    struct sigevent sev;
    memset(&sev, 0, sizeof(struct sigevent));

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = ctx;
    sev.sigev_notify_function = timer_thread;

    if (timer_create(CLOCK_MONOTONIC, &sev, &ctx->timer_id)) {
        syslog(LOG_ERR, "timer_create() fail: %s (%d)", strerror(errno), errno);
        return errno;
    }

    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = POLL_PERIOD_NS;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = POLL_PERIOD_NS;

    if (timer_settime(ctx->timer_id, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "timer_settime() fail: %s (%d)", strerror(errno), errno);
        return errno;
    }

    return 0;
}

void wait_for_exit(context_t* ctx) {
    bool exit = false;
    int r;

    while(!exit) {
        usleep(POLL_PERIOD_US);
        r = pthread_mutex_lock(&ctx->mtx);
        if (r) {
            syslog(LOG_ERR, "pthread_mutex_lock() fail: %s (%d)", strerror(r), r);
            continue;   
        }

        exit = ctx->exit_flag;
        r = pthread_mutex_unlock(&ctx->mtx);
        if (r) {
            syslog(LOG_ERR, "pthread_mutex_unlock() fail: %s (%d)", strerror(r), r);
        }
    }
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

    init(&ctx);

    memset(&sig_act, 0, sizeof(struct sigaction));
    sig_act.sa_handler = on_signal;

    if (sigaction(SIGINT, &sig_act, 0)
     || sigaction(SIGTERM, &sig_act, 0)) {
        syslog(LOG_ERR, "sigaction fail: %s(%d)", strerror(errno), errno);
        exit(EXIT_FAILURE);
    }

    r = init_socket(&ctx);
    if (r) {
        syslog(LOG_ERR, "Failed to initialize socket: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }

    r = setup_loop(&ctx);
    if (r) {
        syslog(LOG_ERR, "Failed to setup main loop: %s(%d)", strerror(r), r);
        clean(&ctx);
        exit(EXIT_FAILURE);
    }

    wait_for_exit(&ctx);

    clean(&ctx);

    return 0;
}
