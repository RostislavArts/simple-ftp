#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>

#define CONTROL_PORT 2121
#define BUFFER_SIZE 4096

char transfer_type = 'A';

void send_response(int client_socket, const char *response) {
    send(client_socket, response, strlen(response), 0);
}

void handle_list(int client_socket, int data_socket) {
    DIR *dir;
    struct dirent *entry;
    char buffer[BUFFER_SIZE];

    dir = opendir("./ftp_files/");
    if (dir == NULL) {
        send_response(client_socket, "450 Failed to list directory\r\n");
        return;
    }

    send_response(client_socket, "150 Directory listing\r\n");

    while ((entry = readdir(dir)) != NULL) {
        snprintf(buffer, BUFFER_SIZE, "%s\r\n", entry->d_name);
        send(data_socket, buffer, strlen(buffer), 0);
    }

    closedir(dir);
    send_response(client_socket, "226 Directory send ok\r\n");
}

void handle_retr(int client_socket, int data_socket, const char *filename) {
    char filepath[BUFFER_SIZE];
    snprintf(filepath, sizeof(filepath), "./ftp_files/%s", filename);

    FILE *file;
    char buffer[BUFFER_SIZE];
    size_t bytes_read;

    file = fopen(filepath, transfer_type == 'I' ? "rb" : "r");
    if (!file) {
        send_response(client_socket, "550 File not found.\r\n");
        return;
    }

    send_response(client_socket, "150 Opening data connection.\r\n");

    if (transfer_type == 'I') {
        while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
            send(data_socket, buffer, bytes_read, 0);
        }
    } else {
        while (fgets(buffer, sizeof(buffer), file)) {
            for (char *p = buffer; *p; ++p) {
                if (*p == '\n') {
                    send(data_socket, "\r", 1, 0);
                }
                send(data_socket, p, 1, 0);
            }
        }
    }

    fclose(file);
    send_response(client_socket, "226 Transfer complete.\r\n");
}

void handle_stor(int client_socket, int data_socket, const char *filename) {
    char filepath[BUFFER_SIZE];
    snprintf(filepath, sizeof(filepath), "./ftp_files/%s", filename);

    FILE *file;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    file = fopen(filepath, transfer_type == 'I' ? "wb" : "w");
    if (!file) {
        perror("Error opening file for writing");
        send_response(client_socket, "550 Could not open file for writing.\r\n");
        return;
    }

    send_response(client_socket, "150 Opening data connection for file upload.\r\n");

    if (transfer_type == 'I') {
        while ((bytes_received = recv(data_socket, buffer, BUFFER_SIZE, 0)) > 0) {
            if (fwrite(buffer, 1, bytes_received, file) < bytes_received) {
                perror("Error writing to file");
                send_response(client_socket, "451 Error writing to file.\r\n");
                fclose(file);
                return;
            }
        }
    } else if (transfer_type == 'A') {
        while ((bytes_received = recv(data_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
            buffer[bytes_received] = '\0';
            for (char *p = buffer; *p; ++p) {
                if (*p == '\r' && *(p + 1) == '\n') {
                    fputc('\n', file);
                    p++;
                } else {
                    fputc(*p, file);
                }
            }
        }
    }

    if (bytes_received < 0) {
        perror("Error receiving data");
        send_response(client_socket, "426 Connection closed; transfer aborted.\r\n");
    } else {
        send_response(client_socket, "226 Transfer complete.\r\n");
    }

    fclose(file);
}

void handle_type(int client_socket, const char *type) {
    if (strcmp(type, "I") == 0) {
        transfer_type = 'I';
        send_response(client_socket, "200 Type set to I (Binary).\r\n");
    } else if (strcmp(type, "A") == 0) {
        transfer_type = 'A';
        send_response(client_socket, "200 Type set to A (ASCII).\r\n");
    } else {
        send_response(client_socket, "504 Command not implemented for that parameter.\r\n");
    }
}

int main() {
    int control_socket, client_socket, data_socket;
    struct sockaddr_in server_addr, client_addr, data_addr;
    socklen_t addr_len;
    char buffer[BUFFER_SIZE];
    char command[16], argument[256];

    control_socket = socket(AF_INET, SOCK_STREAM, 0);
    int optval = 1;
    setsockopt(control_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
    if (control_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CONTROL_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(control_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(control_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(control_socket, 5) < 0) {
        perror("Listen failed");
        close(control_socket);
        exit(EXIT_FAILURE);
    }

    printf("FTP server running on port %d\n", CONTROL_PORT);

    while (1) {
        addr_len = sizeof(client_addr);
        client_socket = accept(control_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        send_response(client_socket, "220 Local FTP server ready\r\n");

        while (recv(client_socket, buffer, BUFFER_SIZE - 1, 0) > 0) {
            buffer[strcspn(buffer, "\r\n")] = 0; //clears all data untill \r\n
            printf("Command received: %s\n", buffer);

            if (sscanf(buffer, "%s %s", command, argument) < 1) {
                send_response(client_socket, "500 Syntax error\r\n");
                continue;
            }

            if (strcmp(command, "USER") == 0) {
                send_response(client_socket, "230 User logged in\r\n");
            } else if (strcmp(command, "PORT") == 0) {
                int ip1, ip2, ip3, ip4, p1, p2;
                sscanf(argument, "%d,%d,%d,%d,%d,%d", &ip1, &ip2, &ip3, &ip4, &p1, &p2);

                char ip[INET_ADDRSTRLEN];
                snprintf(ip, INET_ADDRSTRLEN, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);

                data_addr.sin_family = AF_INET;
                data_addr.sin_port = htons(p1 * 256 + p2);
                inet_pton(AF_INET, ip, &data_addr.sin_addr);

                data_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (data_socket < 0 || connect(data_socket, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
                    send_response(client_socket, "425 Can't open data connection\r\n");
                    close(data_socket);
                    continue;
                }

                send_response(client_socket, "200 PORT command successful\r\n");
            } else if (strcmp(command, "LIST") == 0) {
                handle_list(client_socket, data_socket);
                close(data_socket);
            } else if (strcmp(command, "RETR") == 0) {
                handle_retr(client_socket, data_socket, argument);
                close(data_socket);
            } else if (strcmp(command, "STOR") == 0) {
                handle_stor(client_socket, data_socket, argument);
                close(data_socket);
            } else if (strcmp(command, "TYPE") == 0){
                handle_type(client_socket, argument);
            } else if (strcmp(command, "QUIT") == 0) {
                send_response(client_socket, "221 Goodbye\r\n");
                break;
            } else {
                send_response(client_socket, "502 Command not implemented\r\n");
            }
        }

        close(client_socket);
    }

    close(control_socket);
    return 0;
}
