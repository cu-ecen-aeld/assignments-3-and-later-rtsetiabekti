#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <pthread.h>
#include <sys/queue.h>
#include <stdbool.h>

#include <time.h>

#define PORT "9000"

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATA_PATH "/dev/aesdchar"
#else
#define DATA_PATH "/var/tmp/aesdsocketdata"
#endif

#define BUF_SIZE 1024

timer_t timerid;
int server_fd = -1;
volatile sig_atomic_t caught_sig = 0;

struct thread_node {
    pthread_t   thread_id;
    int         client_fd;
    char        ip_str[INET_ADDRSTRLEN];
    bool        complete;
    SLIST_ENTRY(thread_node) entries;
};
SLIST_HEAD(thread_list, thread_node) head = SLIST_HEAD_INITIALIZER(head);
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_signal(int sig) {
    caught_sig = 1;
    // We don't call syslog or shutdown here to keep the handler 100% async-signal-safe
    // for Valgrind's benefit.
}

void cleanup() {
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    struct thread_node *cur = SLIST_FIRST(&head);
    while (cur != NULL) {
        struct thread_node *next = SLIST_NEXT(cur, entries);
        pthread_join(cur->thread_id, NULL);
        free(cur);
        cur = next;
    }
    SLIST_INIT(&head);

#if !USE_AESD_CHAR_DEVICE
    timer_delete(timerid);
#endif
    pthread_mutex_destroy(&file_mutex);

#if !USE_AESD_CHAR_DEVICE
    unlink(DATA_PATH);
#endif
    syslog(LOG_INFO, "Caught signal, exiting");
    closelog();
}

void *thread_func(void *arg) {
    struct thread_node *node = (struct thread_node *)arg;

    int data_fd = open(DATA_PATH, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (data_fd == -1) {
        close(node->client_fd);
        node->complete = true;
        return NULL;
    }

    char *rx_buf = malloc(BUF_SIZE);
    ssize_t total_recv = 0;
    ssize_t current_buf_size = BUF_SIZE;

    while (1) {
        ssize_t bytes_received = recv(node->client_fd, rx_buf + total_recv, current_buf_size - total_recv, 0);
        if (bytes_received <= 0) break;
        total_recv += bytes_received;

        if (memchr(rx_buf + total_recv - bytes_received, '\n', bytes_received)) {
            pthread_mutex_lock(&file_mutex);          // 1a
            write(data_fd, rx_buf, total_recv);

            fsync(data_fd);
            lseek(data_fd, 0, SEEK_SET);
            char read_buf[BUF_SIZE];
            ssize_t bytes_read;
            while ((bytes_read = read(data_fd, read_buf, BUF_SIZE)) > 0) {
                send(node->client_fd, read_buf, bytes_read, 0);
            }
            pthread_mutex_unlock(&file_mutex);        // 1a
            break;
        }

        current_buf_size += BUF_SIZE;
        char *new_ptr = realloc(rx_buf, current_buf_size);
        if (!new_ptr) { free(rx_buf); rx_buf = NULL; break; }
        rx_buf = new_ptr;
    }

    free(rx_buf);
    close(data_fd);
    close(node->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", node->ip_str);

    node->complete = true;   // 1b: signal main this thread is done
    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
void timer_handler(union sigval sv) {
    (void)sv;

    char timestamp[128];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    // 2a: "timestamp:" + RFC 2822 time + newline
    int len = strftime(timestamp, sizeof(timestamp),
                       "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);
    if (len == 0) return;

    // 2b: same mutex as the socket writes → atomic w.r.t. socket data
    pthread_mutex_lock(&file_mutex);
    int fd = open(DATA_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd != -1) {
        write(fd, timestamp, len);
        close(fd);
    }
    pthread_mutex_unlock(&file_mutex);
}
#endif

int main(int argc, char *argv[]) {
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Setup signal handling IMMEDIATELY
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        return -1;
    }

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd == -1) {
        freeaddrinfo(res);
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) == -1) {
        close(server_fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    // DAEMON MODE
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0);
        setsid();
        if (chdir("/") == -1) return -1;
        int dev_null = open("/dev/null", O_RDWR);
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        close(dev_null);
    }

    if (listen(server_fd, 10) == -1) {
        cleanup();
        return -1;
    }

#if !USE_AESD_CHAR_DEVICE
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_value.sival_ptr = &timerid;

    if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == 0) {
        struct itimerspec its;
        its.it_value.tv_sec = 10;      // first fire after 10s
        its.it_value.tv_nsec = 0;
        its.it_interval.tv_sec = 10;   // then every 10s
        its.it_interval.tv_nsec = 0;
        timer_settime(timerid, 0, &its, NULL);
    }
#endif

    while (!caught_sig) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (caught_sig) break;
            continue;
        }

        // Allocate this connection's node
        struct thread_node *node = malloc(sizeof(struct thread_node));
        if (!node) {
            close(client_fd);
            continue;
        }
        node->client_fd = client_fd;
        node->complete = false;
        inet_ntop(AF_INET, &client_addr.sin_addr, node->ip_str, sizeof(node->ip_str));
        syslog(LOG_INFO, "Accepted connection from %s", node->ip_str);

        // Spawn the worker thread
        if (pthread_create(&node->thread_id, NULL, thread_func, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            close(client_fd);
            free(node);
            continue;
        }

        // Track it in the list
        SLIST_INSERT_HEAD(&head, node, entries);

        struct thread_node *cur = SLIST_FIRST(&head);
        while (cur != NULL) {
            struct thread_node *next = SLIST_NEXT(cur, entries);
            if (cur->complete) {
                pthread_join(cur->thread_id, NULL);
                SLIST_REMOVE(&head, cur, thread_node, entries);
                free(cur);
            }
            cur = next;
        }
    }

    cleanup();
    return 0;
}
