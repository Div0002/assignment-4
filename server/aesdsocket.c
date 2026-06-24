#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define AESD_PORT               (9000U)
#define AESD_LISTEN_BACKLOG     (10)
#define RECEIVE_BUFFER_SIZE     (1024U)
#define FILE_TRANSFER_SIZE      (4096U)
#define INITIAL_PACKET_CAPACITY (1024U)

#define AESD_DATA_FILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t g_termination_requested = 0;

/**
 * Records a termination request.
 *
 * The signal handler performs only an assignment to sig_atomic_t because
 * functions such as syslog(), malloc(), and close() are not generally safe
 * to call from an asynchronous signal handler.
 */
static void termination_signal_handler(int signal_number)
{
    (void)signal_number;
    g_termination_requested = 1;
}

/**
 * Installs handlers for SIGINT and SIGTERM and ignores SIGPIPE.
 *
 * SIGPIPE is ignored so that send() reports EPIPE instead of terminating the
 * process when a client disconnects unexpectedly.
 */
static int configure_signal_handlers(void)
{
    struct sigaction termination_action;
    struct sigaction ignore_action;

    (void)memset(&termination_action, 0, sizeof(termination_action));
    termination_action.sa_handler = termination_signal_handler;
    termination_action.sa_flags = 0;

    if (sigemptyset(&termination_action.sa_mask) != 0)
    {
        syslog(LOG_ERR, "sigemptyset failed: %s", strerror(errno));
        return -1;
    }

    if (sigaction(SIGINT, &termination_action, NULL) != 0)
    {
        syslog(LOG_ERR, "Unable to register SIGINT handler: %s",
               strerror(errno));
        return -1;
    }

    if (sigaction(SIGTERM, &termination_action, NULL) != 0)
    {
        syslog(LOG_ERR, "Unable to register SIGTERM handler: %s",
               strerror(errno));
        return -1;
    }

    (void)memset(&ignore_action, 0, sizeof(ignore_action));
    ignore_action.sa_handler = SIG_IGN;

    if (sigemptyset(&ignore_action.sa_mask) != 0)
    {
        syslog(LOG_ERR, "sigemptyset failed: %s", strerror(errno));
        return -1;
    }

    if (sigaction(SIGPIPE, &ignore_action, NULL) != 0)
    {
        syslog(LOG_ERR, "Unable to ignore SIGPIPE: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * Creates a TCP socket and binds it to port 9000 on all local interfaces.
 */
static int create_bound_server_socket(void)
{
    int socket_fd;
    int reuse_address = 1;
    struct sockaddr_in server_address;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(socket_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse_address,
                   sizeof(reuse_address)) != 0)
    {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        (void)close(socket_fd);
        return -1;
    }

    (void)memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons((uint16_t)AESD_PORT);

    if (bind(socket_fd,
             (const struct sockaddr *)&server_address,
             sizeof(server_address)) != 0)
    {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        (void)close(socket_fd);
        return -1;
    }

    return socket_fd;
}

/**
 * Converts the current process into a background daemon.
 *
 * This function is called only after the server has successfully bound to
 * port 9000, as required by the assignment.
 */
static int daemonize_process(void)
{
    pid_t process_id;
    int null_fd;

    process_id = fork();
    if (process_id < 0)
    {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return -1;
    }

    if (process_id > 0)
    {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0)
    {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") != 0)
    {
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        return -1;
    }

    null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
    {
        syslog(LOG_ERR, "Unable to open /dev/null: %s", strerror(errno));
        return -1;
    }

    if ((dup2(null_fd, STDIN_FILENO) < 0) ||
        (dup2(null_fd, STDOUT_FILENO) < 0) ||
        (dup2(null_fd, STDERR_FILENO) < 0))
    {
        syslog(LOG_ERR, "dup2 failed: %s", strerror(errno));
        (void)close(null_fd);
        return -1;
    }

    if (null_fd > STDERR_FILENO)
    {
        (void)close(null_fd);
    }

    return 0;
}

/**
 * Writes the complete supplied buffer to a file descriptor.
 */
static int write_all(int file_fd, const uint8_t *data, size_t data_length)
{
    size_t bytes_written = 0U;

    while (bytes_written < data_length)
    {
        ssize_t result = write(file_fd,
                               &data[bytes_written],
                               data_length - bytes_written);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            syslog(LOG_ERR, "write failed: %s", strerror(errno));
            return -1;
        }

        if (result == 0)
        {
            syslog(LOG_ERR, "write returned zero before completing");
            return -1;
        }

        bytes_written += (size_t)result;
    }

    return 0;
}

/**
 * Appends one complete newline-terminated packet to the data file.
 */
static int append_packet_to_file(const uint8_t *packet,
                                 size_t packet_length)
{
    int file_fd;
    int result;

    file_fd = open(AESD_DATA_FILE,
                   O_WRONLY | O_CREAT | O_APPEND,
                   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (file_fd < 0)
    {
        syslog(LOG_ERR, "Unable to open %s: %s",
               AESD_DATA_FILE,
               strerror(errno));
        return -1;
    }

    result = write_all(file_fd, packet, packet_length);

    if (close(file_fd) != 0)
    {
        syslog(LOG_ERR, "Unable to close %s: %s",
               AESD_DATA_FILE,
               strerror(errno));
        result = -1;
    }

    return result;
}

/**
 * Sends an entire buffer, handling partial send() operations.
 */
static int send_all(int client_fd,
                    const uint8_t *data,
                    size_t data_length)
{
    size_t bytes_sent = 0U;

    while (bytes_sent < data_length)
    {
        ssize_t result = send(client_fd,
                              &data[bytes_sent],
                              data_length - bytes_sent,
                              0);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                if (g_termination_requested != 0)
                {
                    return -1;
                }

                continue;
            }

            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }

        if (result == 0)
        {
            syslog(LOG_ERR, "send returned zero before completing");
            return -1;
        }

        bytes_sent += (size_t)result;
    }

    return 0;
}

/**
 * Streams the complete data file to the connected client.
 *
 * The file is transferred in fixed-size blocks. Therefore, the complete file
 * is never loaded into heap memory.
 */
static int send_complete_data_file(int client_fd)
{
    int file_fd;
    int result = 0;
    uint8_t transfer_buffer[FILE_TRANSFER_SIZE];

    file_fd = open(AESD_DATA_FILE, O_RDONLY);
    if (file_fd < 0)
    {
        syslog(LOG_ERR, "Unable to open %s for reading: %s",
               AESD_DATA_FILE,
               strerror(errno));
        return -1;
    }

    while (result == 0)
    {
        ssize_t bytes_read = read(file_fd,
                                  transfer_buffer,
                                  sizeof(transfer_buffer));

        if (bytes_read > 0)
        {
            if (send_all(client_fd,
                         transfer_buffer,
                         (size_t)bytes_read) != 0)
            {
                result = -1;
            }
        }
        else if (bytes_read == 0)
        {
            break;
        }
        else if (errno == EINTR)
        {
            if (g_termination_requested != 0)
            {
                result = -1;
            }
        }
        else
        {
            syslog(LOG_ERR, "read failed: %s", strerror(errno));
            result = -1;
        }
    }

    if (close(file_fd) != 0)
    {
        syslog(LOG_ERR, "Unable to close %s: %s",
               AESD_DATA_FILE,
               strerror(errno));
        result = -1;
    }

    return result;
}

/**
 * Extends the dynamically allocated packet buffer.
 */
static int append_to_packet_buffer(uint8_t **packet_buffer,
                                   size_t *packet_length,
                                   size_t *packet_capacity,
                                   const uint8_t *new_data,
                                   size_t new_data_length)
{
    size_t required_capacity;
    size_t new_capacity;
    uint8_t *resized_buffer;

    if (new_data_length == 0U)
    {
        return 0;
    }

    if (*packet_length > (SIZE_MAX - new_data_length))
    {
        syslog(LOG_ERR, "Received packet length overflow");
        return -1;
    }

    required_capacity = *packet_length + new_data_length;

    if (required_capacity > *packet_capacity)
    {
        new_capacity = (*packet_capacity == 0U)
                           ? INITIAL_PACKET_CAPACITY
                           : *packet_capacity;

        while (new_capacity < required_capacity)
        {
            if (new_capacity > (SIZE_MAX / 2U))
            {
                new_capacity = required_capacity;
                break;
            }

            new_capacity *= 2U;
        }

        resized_buffer = realloc(*packet_buffer, new_capacity);
        if (resized_buffer == NULL)
        {
            syslog(LOG_ERR,
                   "Unable to allocate %zu bytes for received packet",
                   new_capacity);
            return -1;
        }

        *packet_buffer = resized_buffer;
        *packet_capacity = new_capacity;
    }

    (void)memcpy(&(*packet_buffer)[*packet_length],
                 new_data,
                 new_data_length);

    *packet_length = required_capacity;

    return 0;
}

/**
 * Receives and processes packets from one connected client.
 *
 * Data may be fragmented across multiple recv() calls, and a single recv()
 * call may contain multiple newline-terminated packets.
 */
static int handle_client_connection(int client_fd)
{
    uint8_t receive_buffer[RECEIVE_BUFFER_SIZE];
    uint8_t *packet_buffer = NULL;
    size_t packet_length = 0U;
    size_t packet_capacity = 0U;
    int result = 0;

    while ((g_termination_requested == 0) && (result == 0))
    {
        ssize_t received_length = recv(client_fd,
                                       receive_buffer,
                                       sizeof(receive_buffer),
                                       0);

        if (received_length > 0)
        {
            size_t segment_start = 0U;
            size_t available_length = (size_t)received_length;

            while ((segment_start < available_length) && (result == 0))
            {
                uint8_t *newline_position;
                size_t remaining_length = available_length - segment_start;

                newline_position = memchr(&receive_buffer[segment_start],
                                          '\n',
                                          remaining_length);

                if (newline_position == NULL)
                {
                    result = append_to_packet_buffer(
                        &packet_buffer,
                        &packet_length,
                        &packet_capacity,
                        &receive_buffer[segment_start],
                        remaining_length);

                    segment_start = available_length;
                }
                else
                {
                    size_t newline_index =
                        (size_t)(newline_position - receive_buffer);

                    size_t segment_length =
                        (newline_index - segment_start) + 1U;

                    result = append_to_packet_buffer(
                        &packet_buffer,
                        &packet_length,
                        &packet_capacity,
                        &receive_buffer[segment_start],
                        segment_length);

                    if (result == 0)
                    {
                        result = append_packet_to_file(packet_buffer,
                                                       packet_length);
                    }

                    if (result == 0)
                    {
                        result = send_complete_data_file(client_fd);
                    }

                    packet_length = 0U;
                    segment_start = newline_index + 1U;
                }
            }
        }
        else if (received_length == 0)
        {
            break;
        }
        else if (errno == EINTR)
        {
            if (g_termination_requested != 0)
            {
                break;
            }
        }
        else
        {
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            result = -1;
        }
    }

    free(packet_buffer);
    packet_buffer = NULL;

    return result;
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int server_fd;
    int exit_status = 0;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (argc == 2)
    {
        if (strcmp(argv[1], "-d") == 0)
        {
            daemon_mode = true;
        }
        else
        {
            syslog(LOG_ERR, "Unsupported argument: %s", argv[1]);
            closelog();
            return -1;
        }
    }
    else if (argc != 1)
    {
        syslog(LOG_ERR, "Usage: aesdsocket [-d]");
        closelog();
        return -1;
    }

    if (configure_signal_handlers() != 0)
    {
        closelog();
        return -1;
    }

    server_fd = create_bound_server_socket();
    if (server_fd < 0)
    {
        closelog();
        return -1;
    }

    if (daemon_mode)
    {
        if (daemonize_process() != 0)
        {
            (void)close(server_fd);
            closelog();
            return -1;
        }
    }

    /*
     * Remove stale data left by an earlier abnormal termination. During a
     * normal shutdown this file is also removed below.
     */
    if ((unlink(AESD_DATA_FILE) != 0) && (errno != ENOENT))
    {
        syslog(LOG_ERR, "Unable to remove stale %s: %s",
               AESD_DATA_FILE,
               strerror(errno));
        (void)close(server_fd);
        closelog();
        return -1;
    }

    if (listen(server_fd, AESD_LISTEN_BACKLOG) != 0)
    {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        (void)close(server_fd);
        closelog();
        return -1;
    }

    while (g_termination_requested == 0)
    {
        struct sockaddr_in client_address;
        socklen_t client_address_length = sizeof(client_address);
        char client_ip[INET_ADDRSTRLEN] = "unknown";
        int client_fd;

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_address,
                           &client_address_length);

        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                if (g_termination_requested != 0)
                {
                    break;
                }

                continue;
            }

            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            exit_status = -1;
            break;
        }

        if (inet_ntop(AF_INET,
                      &client_address.sin_addr,
                      client_ip,
                      sizeof(client_ip)) == NULL)
        {
            syslog(LOG_ERR, "inet_ntop failed: %s", strerror(errno));
        }

        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        if (handle_client_connection(client_fd) != 0)
        {
            if (g_termination_requested == 0)
            {
                syslog(LOG_ERR,
                       "Error while processing connection from %s",
                       client_ip);
            }
        }

        if (close(client_fd) != 0)
        {
            syslog(LOG_ERR, "Unable to close client socket: %s",
                   strerror(errno));
        }

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    if (g_termination_requested != 0)
    {
        syslog(LOG_INFO, "Caught signal, exiting");
    }

    if (close(server_fd) != 0)
    {
        syslog(LOG_ERR, "Unable to close server socket: %s",
               strerror(errno));
        exit_status = -1;
    }

    if ((unlink(AESD_DATA_FILE) != 0) && (errno != ENOENT))
    {
        syslog(LOG_ERR, "Unable to delete %s: %s",
               AESD_DATA_FILE,
               strerror(errno));
        exit_status = -1;
    }

    closelog();

    return exit_status;
}
