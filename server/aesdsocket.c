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

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUF_SIZE 1024

int server_fd = -1;
int data_fd = -1;
volatile sig_atomic_t caught_sig = 0;

void handle_signal(int sig) {
    caught_sig = 1;
    syslog(LOG_INFO, "Caught signal, exiting");
    // Wake up accept() or recv() if possible
    if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
}

void cleanup() {
    if (server_fd != -1) close(server_fd);
    if (data_fd != -1) close(data_fd);
    unlink(DATA_FILE);
    closelog();
}

int main(int argc, char *argv[]) {
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Setup signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Get address info
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

    // Check for daemon mode
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0); // Parent exits
        setsid();
        chdir("/");
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

    while (!caught_sig) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        
        if (client_fd == -1) {
            if (caught_sig) break;
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        // Receive logic
        data_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        char *rx_buf = malloc(BUF_SIZE);
        ssize_t total_recv = 0;
        ssize_t current_buf_size = BUF_SIZE;

        while (1) {
            ssize_t bytes_received = recv(client_fd, rx_buf + total_recv, BUF_SIZE, 0);
            if (bytes_received <= 0) break;
            
            total_recv += bytes_received;
            if (rx_buf[total_recv - 1] == '\n') {
                write(data_fd, rx_buf, total_recv);
                break;
            }

            current_buf_size += BUF_SIZE;
            rx_buf = realloc(rx_buf, current_buf_size);
        }

        // Send logic: send the full file back
        lseek(data_fd, 0, SEEK_SET);
        char read_buf[BUF_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(data_fd, read_buf, BUF_SIZE)) > 0) {
            send(client_fd, read_buf, bytes_read, 0);
        }

        free(rx_buf);
        close(data_fd);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", ip_str);
    }

    cleanup();
    return 0;
}
