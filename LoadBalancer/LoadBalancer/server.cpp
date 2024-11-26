#pragma comment(lib, "ws2_32.lib")
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 1024
#define INITIAL_WORKER_COUNT 5
#define ADD_WORKER_CMD "ADD_WORKER"

// Worker structure
typedef struct {
    int worker_id;
    SOCKET task_socket; // For communication with the server
    HANDLE thread_handle;
    int port; // Store the worker's port for debugging purposes
} Worker;

Worker* workers = NULL;
int worker_count = 0;
int worker_capacity = 0;
CRITICAL_SECTION worker_mutex;


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
        printf("Worker %d: Failed to accept connection. Error: %d\n", worker->worker_id,WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }
    printf("Worker %d is ready and connected.\n", worker->worker_id);

    while (1) {
        // Receive tasks from the server
        read_size = recv(connection_socket, buffer, BUFFER_SIZE - 1, 0);
        if (read_size > 0) {
            buffer[read_size] = '\0';
            printf("Worker %d received task: %s\n", worker->worker_id, buffer);

            // Process the task
            printf("Worker %d processing task: %s\n", worker->worker_id, buffer);
            /*------------------------------------------------------------------------*/
            //                      TODO 
            // - IF COMMAND = STORE: STORE DATA AND SEND DATA BACK FOR SYNC
            // - IF COMMAND = GET: GET ALL DATA AND SEND DATA BACK FOR SYNC
            /*------------------------------------------------------------------------*/

            // Send response to the server
            const char* response = "Task completed by worker.";
            if (send(connection_socket, response, strlen(response), 0) == SOCKET_ERROR) {
                printf("Worker %d: Failed to send response. Error: %d\n", worker->worker_id, WSAGetLastError());
            }
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

int connect_to_worker(Worker* worker) {
    struct sockaddr_in worker_address;

    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = inet_addr("127.0.0.1"); // Assuming localhost for simplicity
    worker_address.sin_port = htons(worker->port);

    SOCKET worker_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (worker_socket == INVALID_SOCKET) {
        printf("Failed to create socket for Worker %d. Error: %d\n", worker->worker_id, WSAGetLastError());
        return 1;
    }

    if (connect(worker_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) == SOCKET_ERROR) {
        printf("Failed to connect to Worker %d on port %d. Error: %d\n", worker->worker_id, worker->port, WSAGetLastError());
        closesocket(worker_socket);
        return 1;
    }

    worker->task_socket = worker_socket;
    printf("Server connected to Worker %d on port %d.\n", worker->worker_id, worker->port);
    return 0;
}


int add_worker() {
    EnterCriticalSection(&worker_mutex);

    if (worker_count == worker_capacity) {
        worker_capacity *= 2;
        workers = (Worker*)realloc(workers, worker_capacity * sizeof(Worker));
    }

    Worker* new_worker = &workers[worker_count];
    new_worker->worker_id = worker_count + 1;

    // Create thread for worker
    new_worker->thread_handle = CreateThread(NULL, 0, worker_function, (LPVOID)new_worker, 0, NULL);
    if (new_worker->thread_handle == NULL) {
        printf("Failed to create thread for Worker %d.\n", new_worker->worker_id);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    // Wait for worker to initialize and retrieve its port (implement a mechanism here)
    Sleep(500); // Allow time for worker to initialize (better: use a synchronization mechanism)

    // Connect to worker
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        printf("Failed to create server socket for Worker %d. Error: %d\n", new_worker->worker_id, WSAGetLastError());
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    int worker_port = new_worker->port;
    if (worker_port == 0) {
        printf("Failed to retrieve port for Worker %d.\n", new_worker->worker_id);
        return 1;
    }

    struct sockaddr_in worker_address;
    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = inet_addr("127.0.0.1"); // Assuming local workers
    worker_address.sin_port = htons(new_worker->port);

    if (connect(server_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) == SOCKET_ERROR) {
        printf("Failed to connect to Worker %d on port %d. Error: %d\n",
            new_worker->worker_id, ntohs(worker_address.sin_port), WSAGetLastError());
        closesocket(server_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    new_worker->task_socket = server_socket;
    printf("Server connected to Worker %d on port %d.\n", new_worker->worker_id, ntohs(worker_address.sin_port));

    worker_count++;
    LeaveCriticalSection(&worker_mutex);
    return 0;
}



// Initialize a fixed number of workers
void initialize_workers(int n) {
    worker_capacity = n;
    workers = (Worker*)malloc(worker_capacity * sizeof(Worker));

    for (int i = 0; i < n; i++) {
        add_worker();
    }

    printf("Initialized %d workers.\n", n);
}

void distribute_task_to_worker(const char* task, SOCKET client_socket) {
    static int current_worker = 0;

    EnterCriticalSection(&worker_mutex);

    if (worker_count == 0) {
        printf("No workers available to handle the task.\n");
        const char* response = "ERROR: No workers available.";
        send(client_socket, response, strlen(response), 0);
    }
    else {
        Worker* worker = &workers[current_worker];
        printf("Assigning task to Worker %d.\n", worker->worker_id);

        int bytes_sent = send(worker->task_socket, task, strlen(task), 0);
        if (bytes_sent == SOCKET_ERROR) {
            printf("Failed to send task to Worker %d. Error: %d\n", worker->worker_id, WSAGetLastError());
            const char* response = "ERROR: Failed to assign task.";
            send(client_socket, response, strlen(response), 0);
        }
        else {
            printf("Successfully sent task to Worker %d: %s\n", worker->worker_id, task);

            // Wait for the worker's response
            char buffer[BUFFER_SIZE];
            int bytes_received = recv(worker->task_socket, buffer, BUFFER_SIZE - 1, 0);
            if (bytes_received > 0) {
                buffer[bytes_received] = '\0';
                printf("Received response from Worker %d: %s\n", worker->worker_id, buffer);

                // Forward the response to the client
                send(client_socket, buffer, strlen(buffer), 0);
            }
            else {
                printf("Failed to receive response from Worker %d. Error: %d\n", worker->worker_id, WSAGetLastError());
                const char* response = "ERROR: Failed to receive worker response.";
                send(client_socket, response, strlen(response), 0);
            }
        }

        current_worker = (current_worker + 1) % worker_count;
    }

    LeaveCriticalSection(&worker_mutex);
}



// Start the server to accept client connections
void start_server() {
    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_address, client_address;
    int client_address_len = sizeof(client_address);
    char buffer[BUFFER_SIZE];

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    InitializeCriticalSection(&worker_mutex);

    // Initialize the worker pool
    initialize_workers(INITIAL_WORKER_COUNT);

   

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        printf("Failed to create server socket. Error: %d\n", WSAGetLastError());
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(5059);

    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        printf("Failed to bind server socket. Error: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) == SOCKET_ERROR) {
        printf("Listen failed. Error: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port 5059...\n");

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_address, &client_address_len);
        if (client_socket == INVALID_SOCKET) {
            printf("Failed to accept connection. Error: %d\n", WSAGetLastError());
            continue;
        }
        while (1) {
            int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
            if (bytes_received > 0) {
                buffer[bytes_received] = '\0';
                if (strcmp(buffer, ADD_WORKER_CMD) == 0) {
                    printf("Received add_worker command.\n");
                    add_worker();
                    send(client_socket, "ADD_RECEIVED", 12, 0);
                }
                else {
                    printf("Received task from client: %s\n", buffer);
                    distribute_task_to_worker(buffer, client_socket); // Forward the task and pass the client socket
                }
            }
        }

        closesocket(client_socket);
    }

    closesocket(server_socket);
    DeleteCriticalSection(&worker_mutex);
    WSACleanup();
}
