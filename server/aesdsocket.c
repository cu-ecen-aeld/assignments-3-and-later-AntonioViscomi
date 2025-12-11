#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

int server_fd = -1;
int client_fd = -1;
volatile sig_atomic_t keep_running = 1;

void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        keep_running = 0;
        
        // Break any blocking socket calls
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

void setup_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    // We do NOT use SA_RESTART because we want blocking calls (like accept)
    // to return so we can check the keep_running flag.
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("Error registering SIGINT");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("Error registering SIGTERM");
        exit(EXIT_FAILURE);
    }
}

void send_file_content(int c_fd) {
    int file_fd = open(DATA_FILE, O_RDONLY);
    if (file_fd == -1) {
        syslog(LOG_ERR, "Failed to open data file for reading: %s", strerror(errno));
        return;
    }

    char buf[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = read(file_fd, buf, sizeof(buf))) > 0) {
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            ssize_t result = send(c_fd, buf + bytes_sent, bytes_read - bytes_sent, 0);
            if (result == -1) {
                syslog(LOG_ERR, "Error sending data to client: %s", strerror(errno));
                close(file_fd);
                return;
            }
            bytes_sent += result;
        }
    }

    close(file_fd);
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    struct addrinfo hints, *res;
    struct sockaddr_storage client_addr;
    socklen_t client_addr_len;
    char client_ip[INET6_ADDRSTRLEN];
    
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    setup_signals();

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        syslog(LOG_ERR, "getaddrinfo failed");
        return -1;
    }

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd == -1) {
        syslog(LOG_ERR, "socket creation failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    int yes = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "fork failed");
            return -1;
        }
        if (pid > 0) {
            // Parent exits
            exit(EXIT_SUCCESS);
        }

        setsid();
        chdir("/");
        // Redirect stdin/out/err to /dev/null
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        open("/dev/null", O_RDWR);
        dup(0);
        dup(0);
    }

    if (listen(server_fd, 10) == -1) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        return -1;
    }

    while (keep_running) {
        client_addr_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);

        if (client_fd == -1) {
            if (errno == EINTR) {

                continue;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        void *addr;
        if (((struct sockaddr *)&client_addr)->sa_family == AF_INET) {
            addr = &(((struct sockaddr_in *)&client_addr)->sin_addr);
        } else {
            addr = &(((struct sockaddr_in6 *)&client_addr)->sin6_addr);
        }
        inet_ntop(client_addr.ss_family, addr, client_ip, sizeof(client_ip));
        
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        char *packet_buffer = NULL;
        size_t current_len = 0;
        char temp_buf[BUFFER_SIZE];
        ssize_t recv_bytes;
        int packet_complete = 0;

        while (!packet_complete && (recv_bytes = recv(client_fd, temp_buf, sizeof(temp_buf), 0)) > 0) {
            char *new_buffer = realloc(packet_buffer, current_len + recv_bytes);
            if (!new_buffer) {
                syslog(LOG_ERR, "Malloc failed");
                if (packet_buffer) free(packet_buffer);
                packet_buffer = NULL;
                break;
            }
            packet_buffer = new_buffer;

            memcpy(packet_buffer + current_len, temp_buf, recv_bytes);
            current_len += recv_bytes;

            if (memchr(temp_buf, '\n', recv_bytes) != NULL) {
                packet_complete = 1;
            }
        }

        if (recv_bytes == -1) {
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
        } else if (packet_buffer && packet_complete) {
            int file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (file_fd == -1) {
                syslog(LOG_ERR, "File open failed: %s", strerror(errno));
            } else {
                if (write(file_fd, packet_buffer, current_len) == -1) {
                    syslog(LOG_ERR, "File write failed: %s", strerror(errno));
                }
                close(file_fd);
                send_file_content(client_fd);
            }
        }

        if (packet_buffer) free(packet_buffer);

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(client_fd);
        client_fd = -1;
    }
    
    if (server_fd != -1) close(server_fd);
    remove(DATA_FILE);
    closelog();
    
    return 0;
}