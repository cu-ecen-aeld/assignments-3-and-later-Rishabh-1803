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
#include <sys/queue.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024
#define STORE_PATH "/var/tmp/aesdsocketdata"
#define SOCKET_PORT "9000"

#define TIMER_PERIOD_SEC 10

/// @brief connection info struct for linked list
typedef struct conn_ctx {
    pthread_t thread_id;
    int conn_fd;
    char host[NI_MAXHOST];
    struct conn_ctx* next;
} conn_ctx_t;

/// @brief context struct for global variables
typedef struct context {
    timer_t timer_id;
    int sock_fd;
    int store_fd;
    pthread_mutex_t mtx_store;
    conn_ctx_t* conns;
} context_t;

/// @brief global server context
static context_t ctx;

/// @brief print current connections list
void conns_log() {
    int pos = 0;
    conn_ctx_t *cur;
    char buf[1024];

    cur = ctx.conns;
    while (cur)
    {
        pos = pos + sprintf(&buf[pos], "%ld(%d) -> ", cur->thread_id, cur->conn_fd);
        cur = cur->next;
    }

    sprintf(&buf[pos], "NULL");
    syslog(LOG_INFO, "conns: %s", buf);
}

/// @brief release allocated connection thread resources
void conn_ctx_clean(conn_ctx_t* conn) {
    int r;

    // connection is still active
    if (conn->conn_fd != -1) {
        r = pthread_cancel(conn->thread_id);
        if (r) {
            syslog(LOG_ERR, "pthread_cancel fail: %s(%d)", strerror(r), r);
            exit(EXIT_FAILURE);
        }
        close(conn->conn_fd);
    }

    r = pthread_join(conn->thread_id, NULL);
    if (r) {
        syslog(LOG_ERR, "pthread_join fail: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }
}

/// @brief helper to clean connections contexts
/// @param forced only closed connections will be if false 
static void conns_clean(bool forced) {
    conn_ctx_t *curr, *prev, *temp;

    prev = NULL;
    curr = ctx.conns;
    while (curr) {
        if (forced || curr->conn_fd == -1) {
            conn_ctx_clean(curr);
            temp = curr->next;
            if (prev) {
                prev->next = temp;
            } else {
                ctx.conns = temp;
            }
            free(curr);
            curr = temp;
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}

/// @brief SIGTERM and SIGINT signals handler
/// @param sig_num 
static void on_signal(int sig_num) {
    if (sig_num != SIGTERM && sig_num != SIGINT) {
        syslog(LOG_WARNING, "Caught unexpected signal %d, ignore", sig_num);
        return;
    }
    
    syslog(LOG_INFO, "Caught signal %d, exiting", sig_num);

    conns_clean(true);
    timer_delete(ctx.timer_id);
    close(ctx.sock_fd);
    pthread_mutex_destroy(&ctx.mtx_store);
    close(ctx.store_fd);
    remove(STORE_PATH);
    
    exit(0);
}

/// @brief initialize socket, start listen
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

        const int enable = 1;
        if (setsockopt(ctx.sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0 ||
            setsockopt(ctx.sock_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0)
            continue;

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

static void * process_conn(void* arg) {
    int r;
    char buf[BUF_SIZE];
    ssize_t nread, nwrite;
    conn_ctx_t* conn_ctx = (conn_ctx_t*) arg;

    do {
        memset(buf, 0, sizeof(buf));
        nread = recv(conn_ctx->conn_fd, buf, BUF_SIZE, 0);
        if (nread == -1) {
            syslog(LOG_ERR, "recv() fail: %s(%d)", strerror(errno), errno);
            break;
        } else if (nread > 0) {
            r = pthread_mutex_lock(&ctx.mtx_store);
            if (r) {
                syslog(LOG_ERR, "pthread_mutex_lock fail: %s(%d)", strerror(r), r);
                exit(EXIT_FAILURE);
            }
            lseek(ctx.store_fd, 0, SEEK_END);
            nwrite = write(ctx.store_fd, buf, nread);
            if (nwrite != nread) {
                syslog(LOG_ERR, "unexpected nwrite %ld, should be %ld ", nwrite, nread);
                exit(EXIT_FAILURE);
            }

            r = pthread_mutex_unlock(&ctx.mtx_store);
            if (r) {
                syslog(LOG_ERR, "pthread_mutex_unlock fail: %s(%d)", strerror(r), r);
                exit(EXIT_FAILURE);
            }
        }
    } while (nread > 0 && buf[strlen(buf)-1] != '\n');
    
    r = pthread_mutex_lock(&ctx.mtx_store);
    if (r) {
        syslog(LOG_ERR, "pthread_mutex_lock fail: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }
    lseek(ctx.store_fd, 0, SEEK_SET);

    do {
        memset(buf, 0, sizeof(buf));
        nread = read(ctx.store_fd, buf, BUF_SIZE);
        
        if (nread == -1) {
            syslog(LOG_ERR, "recv() fail: %s(%d)", strerror(errno), errno);
            break;
        }

        if (nread > 0) {
            nwrite = send(conn_ctx->conn_fd, buf, nread, 0);
            if (nwrite != nread) {
                syslog(LOG_ERR, "send() fail: %s(%d)", strerror(errno), errno);
            }
        }
    } while (nread > 0);

    r = pthread_mutex_unlock(&ctx.mtx_store);
    if (r) {
        syslog(LOG_ERR, "pthread_mutex_unlock fail: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }

    close(conn_ctx->conn_fd);
    syslog(LOG_INFO, "Closed connection from %s", conn_ctx->host);

    conn_ctx->conn_fd = -1;

    return NULL;
}

void listen_socket() {
    int r;
    int conn_fd;
    conn_ctx_t* conn_ctx;
    socklen_t addr_len;
    char service[NI_MAXSERV];
    struct sockaddr_storage peer_addr;

    addr_len = sizeof(peer_addr);
    conn_fd = accept(ctx.sock_fd, (struct sockaddr *) &peer_addr, &addr_len);
    if (conn_fd == -1) {
        // socket marked as nonblocking so accept fails if there is no waiting connections
        return;
    }

    conn_ctx = malloc(sizeof(conn_ctx_t));
    if (!conn_ctx) {
        syslog(LOG_ERR, "malloc() fail: %s (%d)", strerror(errno), errno);
        goto malloc_fail;
    }

    r = getnameinfo((struct sockaddr *) &peer_addr, addr_len, conn_ctx->host, NI_MAXHOST,
                    service, NI_MAXSERV, NI_NUMERICSERV);
    if (r) {
        syslog(LOG_ERR, "getnameinfo() fail: %s (%d)", gai_strerror(r), r);
    } else {
        syslog(LOG_INFO, "Accepted connection from %s (%d)", conn_ctx->host, conn_fd);
    }

    conn_ctx->conn_fd = conn_fd;
    
    r = pthread_create(&conn_ctx->thread_id, NULL, &process_conn, conn_ctx);
    if (r) {
        syslog(LOG_ERR, "pthread_create() fail: %s (%d)", strerror(r), r);
        goto thread_fail;
    }

    conn_ctx->next = ctx.conns;
    ctx.conns = conn_ctx;

    return;

thread_fail:
    free(conn_ctx);

malloc_fail:
    close(conn_fd);
}

void write_timestamp() {
    int r;
    time_t now;
    ssize_t bufsz, nwrite;
    struct tm *local_tm; 
    char time_str[64];
    char write_buf[128];

    now = time(NULL);
    local_tm = localtime(&now);
    if (local_tm == NULL) {
        syslog(LOG_ERR, "failed to get local time");
        return;
    }

    memset(time_str, 0, sizeof(time_str));
    memset(write_buf, 0, sizeof(write_buf));
    
    if (strftime(time_str, sizeof(time_str), "%a, %d %b %Y %T %z", local_tm) == 0) {
        syslog(LOG_ERR, "strftime returned 0");
        return;
    }

    snprintf(write_buf, sizeof(write_buf),
         "timestamp:%s\n", time_str);

    r = pthread_mutex_lock(&ctx.mtx_store);
    if (r) {
        syslog(LOG_ERR, "pthread_mutex_lock fail: %s(%d)", strerror(r), r);
        return;
    }

    lseek(ctx.store_fd, 0, SEEK_END);
    bufsz = strlen(write_buf);
    nwrite = write(ctx.store_fd, write_buf, bufsz);    
    if (nwrite != bufsz) {
        syslog(LOG_WARNING, "written bytes %ld != %ld expected", nwrite, bufsz);
    }

    r = pthread_mutex_unlock(&ctx.mtx_store);
    if (r) {
        syslog(LOG_ERR, "pthread_mutex_unlock fail: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }
}

static void on_timer(union sigval sv)
{
    (void)sv;
    write_timestamp();
}

// setup POSIX timer thread
int init_timer() {
    struct itimerspec its;
    struct sigevent sev;

    memset(&sev, 0, sizeof(struct sigevent));

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = NULL;
    sev.sigev_notify_function = on_timer;

    if (timer_create(CLOCK_MONOTONIC, &sev, &ctx.timer_id)) {
        syslog(LOG_ERR, "timer_create() fail: %s (%d)", strerror(errno), errno);
        return errno;
    }

    its.it_value.tv_sec = TIMER_PERIOD_SEC;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = TIMER_PERIOD_SEC;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(ctx.timer_id, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "timer_settime() fail: %s (%d)", strerror(errno), errno);
        return errno;
    }

    write_timestamp();

    return 0;
}

int main(int argc, char** argv) {
    int r;
    struct sigaction sig_act;

    // daemonize
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        r = fork();
        if (r) {
            /* parent */
            exit(EXIT_SUCCESS);
        }
        /* child */
        setsid();
    }

    openlog(argv[0], LOG_PERROR, LOG_INFO);
    syslog(LOG_INFO, "Starting socket server on %s", SOCKET_PORT);

    memset(&sig_act, 0, sizeof(struct sigaction));
    sig_act.sa_handler = on_signal;

    if (sigaction(SIGINT, &sig_act, 0)
     || sigaction(SIGTERM, &sig_act, 0)) {
        syslog(LOG_ERR, "sigaction fail: %s(%d)", strerror(errno), errno);
        exit(EXIT_FAILURE);
    }

    ctx.conns = NULL;

    pthread_mutex_init(&ctx.mtx_store, NULL);
    ctx.store_fd = open(STORE_PATH, O_RDWR | O_CREAT, 0600);
    if (ctx.store_fd == -1) {
        syslog(LOG_ERR, "Failed to open file %s", STORE_PATH);
        exit(EXIT_FAILURE);
    }

    r = init_timer();
    if (r) {
        syslog(LOG_ERR, "Failed to initialize timer: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }
    
    r = init_socket();
    if (r) {
        syslog(LOG_ERR, "Failed to initialize socket: %s(%d)", strerror(r), r);
        exit(EXIT_FAILURE);
    }

    for (;;) {
        listen_socket();
        conns_clean(false);
    }
}
