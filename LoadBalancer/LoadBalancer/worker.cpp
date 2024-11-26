#include "worker.h"
#include "server.h"

void handle_task(Worker* worker, const char* task) {
    // Process the task (e.g., store the data)
    printf("Worker %d storing data: %s\n", worker->worker_id, task);

    // Simulate storing data (you could replace this with actual logic)
    Sleep(1000); // Simulate a task processing time

    // Send confirmation to the server
    const char* confirmation = "Data stored successfully.";
    if (send(worker->task_socket, confirmation, strlen(confirmation), 0) == SOCKET_ERROR) {
        printf("Failed to send confirmation from Worker %d. Error: %d\n", worker->worker_id, WSAGetLastError());
    }
}

void initialize_workers(Worker* workers, int n) {
    workers = (Worker*)malloc(n * sizeof(Worker));

    for (int i = 0; i < n; i++) {
        add_worker();
    }

    printf("Initialized %d workers.\n", n);
}

DWORD WINAPI worker_function(LPVOID args) {
    Worker* worker = (Worker*)args;
    SOCKET listen_socket, connection_socket;
    struct sockaddr_in worker_address;
    char buffer[BUFFER_SIZE];
    int read_size;

    // Create a listening socket
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket == INVALID_SOCKET) {
        printf("Worker %d: Failed to create listening socket. Error: %d\n", worker->worker_id, WSAGetLastError());
        return 1;
    }

    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = INADDR_ANY;
    worker_address.sin_port = 0; // Let the system assign a random port

    if (bind(listen_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) == SOCKET_ERROR) {
        printf("Worker %d: Failed to bind listening socket. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }

    if (listen(listen_socket, 1) == SOCKET_ERROR) {
        printf("Worker %d: Failed to listen on socket. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }

    // Get the assigned port
    int address_length = sizeof(worker_address);
    getsockname(listen_socket, (struct sockaddr*)&worker_address, &address_length);
    int assigned_port = ntohs(worker_address.sin_port);
    worker->port = assigned_port;
    printf("Worker %d is listening on port %d.\n", worker->worker_id, assigned_port);

    // Wait for connection from server
    connection_socket = accept(listen_socket, NULL, NULL);
    if (connection_socket == INVALID_SOCKET) {
        printf("Worker %d: Failed to accept connection. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }
    printf("Worker %d is ready and connected.\n", worker->worker_id);

    while (1) {
        // Receive tasks from the server
        read_size = recv(connection_socket, buffer, BUFFER_SIZE - 1, 0);
        if (read_size > 0) {
            buffer[read_size] = '\0';
            handle_task(worker, buffer); // Process the task
        }
        else if (read_size == 0) {
            printf("Worker %d: Connection closed by server.\n", worker->worker_id);
            break;
        }
        else {
            printf("Worker %d: recv failed. Error: %d\n", worker->worker_id, WSAGetLastError());
            break;
        }
    }

    closesocket(connection_socket);
    closesocket(listen_socket);
    printf("Worker %d exiting.\n", worker->worker_id);
    return 0;
}
