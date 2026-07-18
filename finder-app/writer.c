#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define WRITEFILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

static int write_all(int fd, const char *buf, size_t count)
{
    while (count > 0) {
        ssize_t written = write(fd, buf, count);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        count -= (size_t)written;
        buf += written;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *writefile;
    const char *writestr;
    int fd;
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);

    if (argc != 3) {
        if (argc < 2) {
            fprintf(stderr, "Error: missing both arguments: <writefile> and <writestr>\n");
            syslog(LOG_ERR, "Invalid arguments: expected 2, got %d "
                            "(missing <writefile> and <writestr>)", argc - 1);
        } else if (argc < 3) {
            fprintf(stderr, "Error: missing argument: <writestr>\n");
            syslog(LOG_ERR, "Invalid arguments: expected 2, got %d "
                            "(missing <writestr>)", argc - 1);
        } else {
            fprintf(stderr, "Error: too many arguments: expected 2, got %d\n", argc - 1);
            syslog(LOG_ERR, "Invalid arguments: expected 2, got %d", argc - 1);
        }
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        fprintf(stderr, "  <writefile>  full path to the file to write\n");
        fprintf(stderr, "  <writestr>   text string to write into that file\n");
        closelog();
        return 1;
    }

    writefile = argv[1];
    writestr = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, WRITEFILE_MODE);
    if (fd < 0) {
        fprintf(stderr, "Error: could not open %s for writing: %s\n",
                writefile, strerror(errno));
        syslog(LOG_ERR, "Could not open %s for writing: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    if (write_all(fd, writestr, strlen(writestr)) != 0 ||
        write_all(fd, "\n", 1) != 0) {
        fprintf(stderr, "Error: could not write to %s: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "Could not write to %s: %s", writefile, strerror(errno));
        close(fd);
        closelog();
        return 1;
    }

    if (close(fd) != 0) {
        fprintf(stderr, "Error: could not close %s: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "Could not close %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
