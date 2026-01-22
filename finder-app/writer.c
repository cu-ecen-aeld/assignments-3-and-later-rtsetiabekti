#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    // 1. Setup syslog
    openlog("writer-a2", LOG_PID, LOG_USER);

    // 2. Check arguments
    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments: %d", argc);
        fprintf(stderr, "Usage: %s <file> <string>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    const char *content = argv[2];

    // 3. Log the attempt
    syslog(LOG_DEBUG, "Writing %s to %s", content, filename);

    // 4. Open file for writing (create if not exists, truncate if does)
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "Error opening file %s: %s", filename, strerror(errno));
        return 1;
    }

    // 5. Write to file
    ssize_t nr = write(fd, content, strlen(content));
    if (nr == -1) {
        syslog(LOG_ERR, "Error writing to file: %s", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    closelog();
    return 0;
}
