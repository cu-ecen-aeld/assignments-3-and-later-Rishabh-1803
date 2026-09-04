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
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define BUF_SIZE 1024
#define SOCKET_PORT "9000"

#if !USE_AESD_CHAR_DEVICE
#define STORE_PATH "/var/tmp/aesdsocketdata"
#define TIMER_PERIOD_SEC 10
#endif

#if USE_AESD_CHAR_DEVICE
#define STORE_PATH "/dev/aesdchar"
#endif

typedef struct conn_ctx {
    pthread_t thread_id;
    int conn_fd;
    char host[NI_MAXHOST];
    struct conn_ctx *next;
} conn_ctx_t;

typedef struct context {
    int sock_fd;
#if !USE_AESD_CHAR_DEVICE
    timer_t timer_id;
    int store_fd;
    pthread_mutex_t mtx_store;
#endif
    conn_ctx_t *conns;
} context_t;

static context_t ctx;

static void conns_log(void)
{
    int pos = 0;
    conn_ctx_t *cur;
    char buf[1024];

    cur = ctx.conns;
    while (cur) {
        pos += sprintf(&buf[pos], "%ld(%d) -> ",
                       cur->thread_id, cur->conn_fd);
        cur = cur->next;
    }

    sprintf(&buf[pos], "NULL");
    syslog(LOG_INFO, "conns: %s", buf);
}

static void conn_ctx_clean(conn_ctx_t *conn)
{
    int r;

    if (conn->conn_fd != -1) {
        r = pthread_cancel(conn->thread_id);
        if (r) {
            syslog(LOG_ERR, "pthread_cancel fail: %s(%d)",
                   strerror(r), r);
            exit(EXIT_FAILURE);
        }

        close(conn->conn_fd);
    }

    r = pthread_join(conn->thread_id, NULL);
    if (r) {
        syslog(LOG_ERR, "pthread_join fail: %s(%d)",
               strerror(r), r);
        exit(EXIT_FAILURE);
    }
}

static void conns_clean(bool forced)
{
    conn_ctx_t *curr;
    conn_ctx_t *prev;
    conn_ctx_t *temp;

    prev = NULL;
    curr = ctx.conns;

    while (curr) {
        if (forced || curr->conn_fd == -1) {
            conn_ctx_clean(curr);

            temp = curr->next;

            if (prev)
                prev->next = temp;
            else
                ctx.conns = temp;

            free(curr);
            curr = temp;
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}

static void on_signal(int sig_num)
{
    if (sig_num != SIGTERM && sig_num != SIGINT) {
        syslog(LOG_WARNING,
               "Caught unexpected signal %d, ignore",
               sig_num);
        return;
    }

    syslog(LOG_INFO, "Caught signal %d, exiting", sig_num);

    conns_clean(true);
    close(ctx.sock_fd);

#if !USE_AESD_CHAR_DEVICE
    timer_delete(ctx.timer_id);
    pthread_mutex_destroy(&ctx.mtx_store);
    close(ctx.store_fd);
    remove(STORE_PATH);
#endif

    /*
     * Do not remove /dev/aesdchar here.  In Assignment 8 the
     * driver/device lifetime is independent of aesdsocket.
     */
    exit(EXIT_SUCCESS);
}

static int init_socket(void)
{
    int r;
    struct addrinfo hints;
    struct addrinfo *addr;
    struct addrinfo *rp;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;

    r = getaddrinfo(NULL, SOCKET_PORT, &hints, &addr);
    if (r) {
        syslog(LOG_ERR, "getaddrinfo fail: %s(%d)",
               gai_strerror(r), r);
        return r;
    }

    for (rp = addr; rp != NULL; rp = rp->ai_next) {
        ctx.sock_fd = socket(rp->ai_family,
                             rp->ai_socktype,
                             rp->ai_protocol);

        if (ctx.sock_fd == -1)
            continue;

        {
            const int enable = 1;

            if (setsockopt(ctx.sock_fd,
                           SOL_SOCKET,
                           SO_REUSEADDR,
                           &enable,
                           sizeof(enable)) < 0) {
                close(ctx.sock_fd);
                continue;
            }

            if (setsockopt(ctx.sock_fd,
                           SOL_SOCKET,
                           SO_REUSEPORT,
                           &enable,
                           sizeof(enable)) < 0) {
                close(ctx.sock_fd);
                continue;
            }
        }

        r = bind(ctx.sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (r == 0)
            break;

        close(ctx.sock_fd);
    }

    if (rp == NULL) {
        freeaddrinfo(addr);
        syslog(LOG_ERR, "socket bind fail");
        return ENOTCONN;
    }

    freeaddrinfo(addr);

    if (listen(ctx.sock_fd, 50) == -1) {
        syslog(LOG_ERR, "listen fail: %s(%d)",
               strerror(errno), errno);
        return errno;
    }

    return 0;
}

static int open_store(void)
{
    int fd;

    /*
     * Assignment 8 requires the driver endpoint to be opened only
     * when it is actually accessed.
     */
    fd = open(STORE_PATH, O_RDWR);
    if (fd == -1) {
        syslog(LOG_ERR, "Failed to open %s: %s(%d)",
               STORE_PATH, strerror(errno), errno);
    }

    return fd;
}

static int write_request_to_store(const char *buf, size_t len)
{
    int fd;
    ssize_t nwrite;

    fd = open_store();
    if (fd == -1)
        return -1;

    nwrite = write(fd, buf, len);

    if (nwrite != (ssize_t)len) {
        syslog(LOG_ERR,
               "write() returned %ld, expected %zu",
               nwrite, len);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int send_store_contents(int conn_fd)
{
    int fd;
    char buf[BUF_SIZE];
    ssize_t nread;
    ssize_t nwrite;

    fd = open_store();
    if (fd == -1)
        return -1;

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        syslog(LOG_ERR, "lseek() failed: %s(%d)",
               strerror(errno), errno);
        close(fd);
        return -1;
    }

    do {
        nread = read(fd, buf, sizeof(buf));

        if (nread == -1) {
            syslog(LOG_ERR, "read() failed: %s(%d)",
                   strerror(errno), errno);
            close(fd);
            return -1;
        }

        if (nread > 0) {
            nwrite = send(conn_fd, buf, nread, 0);

            if (nwrite != nread) {
                syslog(LOG_ERR,
                       "send() returned %ld, expected %ld",
                       nwrite, nread);
                close(fd);
                return -1;
            }
        }
    } while (nread > 0);

    close(fd);
    return 0;
}

static void *process_conn(void *arg)
{
    char buf[BUF_SIZE];
    ssize_t nread;
    conn_ctx_t *conn_ctx;

    conn_ctx = (conn_ctx_t *)arg;

    do {
        nread = recv(conn_ctx->conn_fd,
                     buf,
                     sizeof(buf),
                     0);

        if (nread == -1) {
            syslog(LOG_ERR,
                   "recv() fail: %s(%d)",
                   strerror(errno),
                   errno);
            break;
        }

        if (nread > 0) {
            if (write_request_to_store(buf, (size_t)nread) != 0)
                break;
        }
    } while ((nread > 0) && (buf[nread - 1] != '\n'));

    /*
     * The driver provides the serialization and state protection.
     * Open it only for this actual access and close it immediately
     * when the response has been read.
     */
    if (nread >= 0)
        send_store_contents(conn_ctx->conn_fd);

    close(conn_ctx->conn_fd);

    syslog(LOG_INFO,
           "Closed connection from %s",
           conn_ctx->host);

    conn_ctx->conn_fd = -1;

    return NULL;
}

static void listen_socket(void)
{
    int r;
    int conn_fd;
    conn_ctx_t *conn_ctx;
    socklen_t addr_len;
    char service[NI_MAXSERV];
    struct sockaddr_storage peer_addr;

    addr_len = sizeof(peer_addr);

    conn_fd = accept(ctx.sock_fd,
                     (struct sockaddr *)&peer_addr,
                     &addr_len);

    if (conn_fd == -1)
        return;

    conn_ctx = malloc(sizeof(*conn_ctx));
    if (!conn_ctx) {
        syslog(LOG_ERR,
               "malloc() fail: %s(%d)",
               strerror(errno),
               errno);
        close(conn_fd);
        return;
    }

    memset(conn_ctx, 0, sizeof(*conn_ctx));

    r = getnameinfo((struct sockaddr *)&peer_addr,
                    addr_len,
                    conn_ctx->host,
                    sizeof(conn_ctx->host),
                    service,
                    sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV);

    if (r) {
        syslog(LOG_ERR,
               "getnameinfo() fail: %s(%d)",
               gai_strerror(r),
               r);
    } else {
        syslog(LOG_INFO,
               "Accepted connection from %s (%s), fd %d",
               conn_ctx->host,
               service,
               conn_fd);
    }

    conn_ctx->conn_fd = conn_fd;

    r = pthread_create(&conn_ctx->thread_id,
                       NULL,
                       process_conn,
                       conn_ctx);

    if (r) {
        syslog(LOG_ERR,
               "pthread_create() fail: %s(%d)",
               strerror(r),
               r);
        close(conn_fd);
        free(conn_ctx);
        return;
    }

    conn_ctx->next = ctx.conns;
    ctx.conns = conn_ctx;

    conns_log();
}

#if !USE_AESD_CHAR_DEVICE

static void write_timestamp(void)
{
    time_t now;
    struct tm *local_tm;
    char time_str[64];
    char write_buf[128];
    size_t bufsz;
    ssize_t nwrite;

    now = time(NULL);
    local_tm = localtime(&now);

    if (!local_tm) {
        syslog(LOG_ERR, "failed to get local time");
        return;
    }

    if (strftime(time_str,
                 sizeof(time_str),
                 "%a, %d %b %Y %T %z",
                 local_tm) == 0) {
        syslog(LOG_ERR, "strftime returned 0");
        return;
    }

    snprintf(write_buf,
             sizeof(write_buf),
             "timestamp:%s\n",
             time_str);

    if (pthread_mutex_lock(&ctx.mtx_store) != 0)
        return;

    bufsz = strlen(write_buf);
    nwrite = write(ctx.store_fd, write_buf, bufsz);

    if (nwrite != (ssize_t)bufsz) {
        syslog(LOG_WARNING,
               "written bytes %ld != %zu expected",
               nwrite,
               bufsz);
    }

    pthread_mutex_unlock(&ctx.mtx_store);
}

static void on_timer(union sigval sv)
{
    (void)sv;
    write_timestamp();
}

static int init_timer(void)
{
    struct itimerspec its;
    struct sigevent sev;

    memset(&sev, 0, sizeof(sev));

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = NULL;
    sev.sigev_notify_function = on_timer;

    if (timer_create(CLOCK_MONOTONIC,
                     &sev,
                     &ctx.timer_id) == -1) {
        syslog(LOG_ERR,
               "timer_create() fail: %s(%d)",
               strerror(errno),
               errno);
        return errno;
    }

    its.it_value.tv_sec = TIMER_PERIOD_SEC;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = TIMER_PERIOD_SEC;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(ctx.timer_id,
                      0,
                      &its,
                      NULL) == -1) {
        syslog(LOG_ERR,
               "timer_settime() fail: %s(%d)",
               strerror(errno),
               errno);
        return errno;
    }

    return 0;
}

#endif

int main(int argc, char **argv)
{
    int r;
    struct sigaction sig_act;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        r = fork();

        if (r < 0)
            exit(EXIT_FAILURE);

        if (r > 0)
            exit(EXIT_SUCCESS);

        if (setsid() == -1)
            exit(EXIT_FAILURE);
    }

    openlog(argv[0], LOG_PERROR, LOG_INFO);

    syslog(LOG_INFO,
           "Starting socket server on %s",
           SOCKET_PORT);

    memset(&sig_act, 0, sizeof(sig_act));
    sig_act.sa_handler = on_signal;

    if (sigaction(SIGINT, &sig_act, NULL) ||
        sigaction(SIGTERM, &sig_act, NULL)) {
        syslog(LOG_ERR,
               "sigaction fail: %s(%d)",
               strerror(errno),
               errno);
        exit(EXIT_FAILURE);
    }

    ctx.conns = NULL;

#if !USE_AESD_CHAR_DEVICE
    if (pthread_mutex_init(&ctx.mtx_store, NULL) != 0) {
        syslog(LOG_ERR, "pthread_mutex_init failed");
        exit(EXIT_FAILURE);
    }

    ctx.store_fd = open(STORE_PATH,
                        O_RDWR | O_CREAT | O_TRUNC,
                        0600);

    if (ctx.store_fd == -1) {
        syslog(LOG_ERR,
               "Failed to open file %s: %s",
               STORE_PATH,
               strerror(errno));
        exit(EXIT_FAILURE);
    }

    r = init_timer();
    if (r)
        exit(EXIT_FAILURE);
#endif

    r = init_socket();
    if (r) {
        syslog(LOG_ERR,
               "Failed to initialize socket: %s(%d)",
               strerror(r),
               r);
        exit(EXIT_FAILURE);
    }

    while (true) {
        listen_socket();
        conns_clean(false);
    }

    return 0;
}
