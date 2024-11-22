#pragma comment(lib, "ws2_32.lib")
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "server.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 1024
#define ADD_WORKER_CODE "ADD_WORKER"

// Worker structure definition
typedef struct {
    int worker_id;
    SOCKET worker_socket; // Worker-to-server communication socket
    HANDLE thread_handle;
    int load; // Load based on the number of bytes processed
} Worker;

Worker* workers = NULL;
int worker_count = 0;
int worker_capacity = 0;
CRITICAL_SECTION worker_mutex;

#define INITIAL_WORKER_CAPACITY 5

// Worker thread function
DWORD WINAPI worker_function(LPVOID args) {
    Worker* worker = (Worker*)args;
    char buffer[BUFFER_SIZE];
    int read_size;

    printf("Worker %d started on socket: %d.\n", worker->worker_id, worker->worker_socket);

    while (1) {
        // Use select to check if the socket is readable
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(worker->worker_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 5;  // 5-second timeout
        timeout.tv_usec = 0;

        printf("Worker %d waiting for data...\n", worker->worker_id);

        int select_result = select(0, &read_fds, NULL, NULL, &timeout);
        if (select_result < 0) {
            printf("Worker %d: select failed. Error Code: %d\n", worker->worker_id, WSAGetLastError());
            break;
        }
        else if (select_result == 0) {
            printf("Worker %d: No data received (timeout).\n", worker->worker_id);
            continue;  // No data, keep waiting
        }

        if (FD_ISSET(worker->worker_socket, &read_fds)) {
            read_size = recv(worker->worker_socket, buffer, BUFFER_SIZE - 1, 0);

            if (read_size > 0) {
                buffer[read_size] = '\0';  // Null-terminate the received data
                printf("Worker %d received %d bytes: '%s'\n", worker->worker_id, read_size, buffer);

                // Process data
                printf("Worker %d processing data: %s\n", worker->worker_id, buffer);

                // Send response back to server
                const char* response = "Task processed.";
                send(worker->worker_socket, response, strlen(response), 0);
            }
            else if (read_size == 0) {
                printf("Worker %d: Server closed the connection.\n", worker->worker_id);
                break;
            }
            else {
                printf("Worker %d: recv failed. Error Code: %d\n",
                    worker->worker_id, WSAGetLastError());
                break;
            }
        }
    }

    // Clean up
    closesocket(worker->worker_socket);
    worker->worker_socket = INVALID_SOCKET;
    printf("Worker %d stopped.\n", worker->worker_id);
    return 0;
}


int add_worker() {
    EnterCriticalSection(&worker_mutex);

    if (worker_count == worker_capacity) {
        worker_capacity = (worker_capacity == 0) ? INITIAL_WORKER_CAPACITY : worker_capacity * 2;
        workers = (Worker*)realloc(workers, worker_capacity * sizeof(Worker));
    }

    Worker* new_worker = &workers[worker_count];
    new_worker->worker_id = worker_count + 1;
    new_worker->load = 0;

    // Server socket for communicating with this worker
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        printf("Failed to create server socket. Error Code: %d\n", WSAGetLastError());
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(5060 + worker_count); // Unique port for each worker

    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        printf("Failed to bind server socket for Worker %d. Error Code: %d\n", new_worker->worker_id, WSAGetLastError());
        closesocket(server_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    if (listen(server_socket, 1) == SOCKET_ERROR) {
        printf("Listen failed for Worker %d. Error Code: %d\n", new_worker->worker_id, WSAGetLastError());
        closesocket(server_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    // Create the worker's connecting socket
    new_worker->worker_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (new_worker->worker_socket == INVALID_SOCKET) {
        printf("Failed to create worker socket. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    struct sockaddr_in worker_address;
    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = inet_addr("127.0.0.1");
    worker_address.sin_port = htons(5060 + worker_count); // Same port as server socket

    printf("Worker %d attempting to connect...\n", new_worker->worker_id);

    // Connect worker socket to the server socket
    if (connect(new_worker->worker_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) == SOCKET_ERROR) {
        printf("Worker %d failed to connect. Error Code: %d\n", new_worker->worker_id, WSAGetLastError());
        closesocket(new_worker->worker_socket);
        closesocket(server_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    printf("Waiting for Worker %d to connect...\n", new_worker->worker_id);

    // Accept the connection on the server side
    SOCKET accepted_socket = accept(server_socket, NULL, NULL);
    if (accepted_socket == INVALID_SOCKET) {
        printf("Failed to accept connection for Worker %d. Error Code: %d\n", new_worker->worker_id, WSAGetLastError());
        closesocket(server_socket);
        closesocket(new_worker->worker_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    // Use the accepted socket for further communication
    closesocket(server_socket); // No longer needed
    new_worker->worker_socket = accepted_socket;

    // Start worker thread
    new_worker->thread_handle = CreateThread(NULL, 0, worker_function, (LPVOID)new_worker, 0, NULL);
    if (new_worker->thread_handle == NULL) {
        printf("Failed to create thread for Worker %d.\n", new_worker->worker_id);
        closesocket(new_worker->worker_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    printf("Worker %d added successfully.\n", new_worker->worker_id);
    worker_count++;
    LeaveCriticalSection(&worker_mutex);
    return 0;
}


// Function to distribute data to a worker
void distribute_data_to_worker(const char* data) {
    static int current_worker = 0;

    EnterCriticalSection(&worker_mutex);

    if (worker_count == 0) {
        printf("No workers available to handle the data.\n");
    }
    else {
        Worker* worker = &workers[current_worker];
        printf("Sending data to Worker %d.\n", worker->worker_id);

        int bytes_sent = send(worker->worker_socket, data, strlen(data), 0);
        if (bytes_sent == SOCKET_ERROR) {
            printf("Failed to send data to Worker %d. Error Code: %d\n",
                worker->worker_id, WSAGetLastError());
        }
        else {
            printf("Successfully sent %d bytes to Worker %d: %s\n", bytes_sent, worker->worker_id, data);
        }

        current_worker = (current_worker + 1) % worker_count;
    }

    LeaveCriticalSection(&worker_mutex);
}



// Server main logic
void start_server() {
    WSADATA wsa;
    SOCKET server_fd, client_fd;
    struct sockaddr_in server_address, client_address;
    int client_address_len = sizeof(client_address);
    char buffer[BUFFER_SIZE];

    // Initialise the worker management system
    InitializeCriticalSection(&worker_mutex);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error Code: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code: %d\n", WSAGetLastError());
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(5059);

    if (bind(server_fd, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        printf("Bind failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) == SOCKET_ERROR) {
        printf("Listen failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", 5059);

    while ((client_fd = accept(server_fd, (struct sockaddr*)&client_address, &client_address_len)) != INVALID_SOCKET) {

        int bytes_received;

        while ((bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0)) > 0) {

            buffer[bytes_received] = '\0';
            printf("Received data: '%s'\n", buffer);

            // Remove newline or any extra whitespace from the received message
            buffer[strcspn(buffer, "\n")] = '\0';

            // Check for the ADD_WORKER command
            if (strcmp(buffer, ADD_WORKER_CODE) == 0) {
                printf("Add Worker command received.\n");

                // Call the function to add a new worker
                if (add_worker() != 1) {
                    const char* response = "Worker created.";
                    send(client_fd, response, strlen(response), 0);
                }
                else {
                    const char* response = "Unable to create worker!";
                    send(client_fd, response, strlen(response), 0);
                }
            }
            else {
                printf("Message received: %s\n", buffer);
                // Send acknowledgment for any other message
                const char* response = "Data received.";
                send(client_fd, response, strlen(response), 0);

                distribute_data_to_worker(buffer);  // Send the data to a worker
            }
        }
        closesocket(client_fd);
    }

    DeleteCriticalSection(&worker_mutex);
    closesocket(server_fd);
    WSACleanup();
}
