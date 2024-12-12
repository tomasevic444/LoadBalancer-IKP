#pragma comment(lib, "ws2_32.lib")
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashmap.h"
#include "hashmap.cpp"

#define BUFFER_SIZE 1024
#define INITIAL_WORKER_COUNT 5
#define ADD_WORKER_CMD "ADD_WORKER"

// Worker structure
typedef struct {
    int worker_id;
    SOCKET task_socket; // For communication with the server
    HANDLE thread_handle; // handle for running the worker function
    int port;
    int load;
    HashMap* data_store;
    int synced;
} Worker;

Worker* workers = NULL;
int worker_count = 0;
int worker_capacity = 0;

// Critical section mutex-es for syncronizing tasks
CRITICAL_SECTION worker_mutex;
CRITICAL_SECTION worker_queue_mutex;

typedef struct {
    int worker_id;
    char task[BUFFER_SIZE];
} TaskInfo;

typedef struct TaskNode {
    TaskInfo task_info;
    struct TaskNode* next;
} TaskNode;

TaskNode* task_queue_head = NULL;
TaskNode* task_queue_tail = NULL;
CRITICAL_SECTION task_queue_mutex;

int sync_active = 0;

//handle for the response thread function
HANDLE response_thread;

// Adding and removing tasks to queue (for now unused but will probably be important later)
void enqueue_task(TaskInfo task_info) {
    EnterCriticalSection(&task_queue_mutex);

    TaskNode* new_node = (TaskNode*)malloc(sizeof(TaskNode));
    new_node->task_info = task_info;
    new_node->next = NULL;

    if (task_queue_tail) {
        task_queue_tail->next = new_node;
    }
    else {
        task_queue_head = new_node;
    }
    task_queue_tail = new_node;

    LeaveCriticalSection(&task_queue_mutex);
}
TaskInfo dequeue_task() {
    EnterCriticalSection(&task_queue_mutex);

    TaskInfo task_info = { 0 };
    if (task_queue_head) {
        TaskNode* temp = task_queue_head;
        task_info = temp->task_info;
        task_queue_head = task_queue_head->next;
        if (!task_queue_head) {
            task_queue_tail = NULL;
        }
        free(temp);
    }

    LeaveCriticalSection(&task_queue_mutex);
    return task_info;
}

// Compute hash for given string value (for key -> value pairing)
unsigned long hash_function(const char* str) {
    unsigned long hash = 5381; // Starting value for the hash
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }

    return hash;
}

// Handles the worker responses and synchronizes the workers after each response
DWORD WINAPI worker_response_handler(LPVOID args) {
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;
    int activity;

    while (1) {
        FD_ZERO(&readfds);

        // Add worker sockets to the fd_set
        EnterCriticalSection(&worker_mutex);
        int max_sd = 0;
        for (int i = 0; i < worker_count; i++) {
            Worker* worker = &workers[i];
            if (worker->task_socket != INVALID_SOCKET) {
                FD_SET(worker->task_socket, &readfds);
                if (worker->task_socket > max_sd) {
                    max_sd = worker->task_socket;
                }
            }
        }
        LeaveCriticalSection(&worker_mutex);

        // Set timeout for select
        timeout.tv_sec = 1;  // 1-second timeout
        timeout.tv_usec = 0;

        // Monitor the sockets for readability
        activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if (activity < 0 && WSAGetLastError() != WSAEINTR) {
            printf("Select failed. Error: %d\n", WSAGetLastError());
            break;
        }

        if (activity > 0) {
            EnterCriticalSection(&worker_mutex);
            for (int i = 0; i < worker_count; i++) {
                Worker* worker = &workers[i];

                if (worker->task_socket != INVALID_SOCKET && FD_ISSET(worker->task_socket, &readfds)) {
                    int bytes_received = recv(worker->task_socket, buffer, BUFFER_SIZE - 1, 0);

                    if (bytes_received > 0) {
                        buffer[bytes_received] = '\0';
                        printf("Worker %d responded: %s\n", worker->worker_id, buffer);

                        //start the sync procedure 
                        sync_active = 1;
                        // Process the response here or signal the main server
                        for (int j = 0; j < worker_count; j++) {
                            if (i != j) workers[i].synced = 0;
                        }
                        //send the data received to all workers
                        ///TODO
                        for (int j = 0; j < worker_count; j++) {
                            if (i != j) send(workers[i].task_socket, buffer, bytes_received, 0);
                        }
                        // resume normal work
                        sync_active = 0;

                    }
                    else if (bytes_received == 0) {
                        printf("Worker %d disconnected.\n", worker->worker_id);
                        closesocket(worker->task_socket);
                        worker->task_socket = INVALID_SOCKET;
                    }
                    else {
                        printf("Worker %d: recv failed. Error: %d\n", worker->worker_id, WSAGetLastError());
                    }
                }
            }
            LeaveCriticalSection(&worker_mutex);
        }

        // Sleep to reduce CPU usage
        Sleep(50);
    }

    return 0;
}

// Main function that workers execute, it stores or loads data from and to the worker node
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

        if (sync_active == 1 && worker->synced == 0) {
            // Receive tasks from the server
            read_size = recv(connection_socket, buffer, BUFFER_SIZE - 1, 0);
            buffer[read_size] = '\0';

            // Extract key and value
            char key[BUFFER_SIZE], value[BUFFER_SIZE];
            sscanf_s(buffer + 6, "%s", value, _countof(value));

            // Generate a key based on the value
            unsigned long key_hash = hash_function(value);
            sprintf_s(key, "%lu", key_hash); // Convert hash to string

            // Store in the worker's hash map
            if (insert(worker->data_store, key, _strdup(value)) == 0) {
                printf("Worker %d stored data: %s -> %s\n", worker->worker_id, key, value);
                print_hash_map(worker->data_store);
                const char* response = buffer;
                send(connection_socket, response, read_size, 0);
            }
            else {
                printf("Worker %d failed to store data: %s -> %s\n", worker->worker_id, key, value);
                const char* response = buffer;
                send(connection_socket, response, read_size, 0);
            }
            worker->synced = 1;
        }

        // Receive tasks from the server
        read_size = recv(connection_socket, buffer, BUFFER_SIZE - 1, 0);
        if (read_size > 0) {
            buffer[read_size] = '\0';
            printf("Worker %d received task: %s\n", worker->worker_id, buffer);

            // Process the task
            printf("Worker %d processing task: %s\n", worker->worker_id, buffer);

            /*============THE FUN PART==============*/


            /*
                Format: STORE: <value>
                Key generation: generates a hash for the key based on the value
                Sends: <value> contained in key-ed location
            */
            if (strncmp(buffer, "STORE:", 6) == 0) {
                // Extract key and value
                char key[BUFFER_SIZE], value[BUFFER_SIZE];
                sscanf_s(buffer + 6, "%s", value,_countof(value));

                // Generate a key based on the value
                unsigned long key_hash = hash_function(value);
                sprintf_s(key, "%lu", key_hash); // Convert hash to string

                // Store in the worker's hash map
                if (insert(worker->data_store, key, _strdup(value)) == 0) {
                    printf("Worker %d stored data: %s -> %s\n", worker->worker_id, key, value);
                    print_hash_map(worker->data_store);
                    const char* response = buffer;
                    send(connection_socket, response, read_size, 0);
                }
                else {
                    printf("Worker %d failed to store data: %s -> %s\n", worker->worker_id, key, value);
                    const char* response = buffer;
                    send(connection_socket, response, read_size, 0);
                }
            }
            /*
                Format: GET: <key_value>
                Sends: <value> contained in key-ed location
            */
            else if (strncmp(buffer, "GET:", 4) == 0) {
                // Extract key
                char key[BUFFER_SIZE];
                sscanf_s(buffer + 4, "%s", key,_countof(key));

                // Retrieve from the worker's hash map
                char* value = (char*)get(worker->data_store, key);
                if (value) {
                    printf("Worker %d retrieved data: %s -> %s\n", worker->worker_id, key, value);
                    send(connection_socket, value, strlen(value), 0);
                }
                else {
                    printf("Worker %d could not find key: %s\n", worker->worker_id, key);
                    const char* response = "GET:NOT_FOUND";
                    send(connection_socket, response, strlen(response), 0);
                }
            }
            Sleep(500);
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

    destroy_hash_map(worker->data_store);
    worker->data_store = NULL;
    closesocket(connection_socket);
    closesocket(listen_socket);
    printf("Worker %d exiting.\n", worker->worker_id);
    return 0;
}

// Connecting the worker to the server socket using TCP
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

// Creating and adding worker to worker pool
int add_worker() {
    EnterCriticalSection(&worker_mutex);

    if (worker_count == worker_capacity) {
        worker_capacity *= 2;
        workers = (Worker*)realloc(workers, worker_capacity * sizeof(Worker));
    }

    Worker* new_worker = &workers[worker_count];
    new_worker->worker_id = worker_count + 1;
    new_worker->load = 0;

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
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    struct sockaddr_in worker_address;
    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = inet_addr("127.0.0.1");
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

    // initializing hashmap for individual worker
    new_worker->data_store = create_hash_map();
    if (!new_worker->data_store) {
        printf("Failed to initialise hashmap for Worker %d.\n", new_worker->worker_id);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    new_worker->synced = 1;

    worker_count++;
    LeaveCriticalSection(&worker_mutex);
    return 0;
}

// Initialize initial workers
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
        // Determine the type of task
        char modified_task[BUFFER_SIZE];
        snprintf(modified_task, BUFFER_SIZE, "STORE: %s", task);

        // Find the worker with the least load
        int min_load = INT_MAX;
        int selected_worker_index = -1;

        for (int i = 0; i < worker_count; i++) {
            Worker* worker = &workers[i];
            if (worker->load < min_load) {
                min_load = worker->load;
                selected_worker_index = i;
            }
        }

        if (selected_worker_index != -1) {
            Worker* selected_worker = &workers[selected_worker_index];
            printf("Assigning task to Worker %d with load %d.\n", selected_worker->worker_id, selected_worker->load);

            int bytes_sent = send(selected_worker->task_socket, modified_task, strlen(modified_task), 0);
            if (bytes_sent == SOCKET_ERROR) {
                printf("Failed to send task to Worker %d. Error: %d\n", selected_worker->worker_id, WSAGetLastError());
                const char* response = "ERROR: Failed to assign task.";
                send(client_socket, response, strlen(response), 0);
            }
            else {
                printf("Successfully sent task to Worker %d: %s\n", selected_worker->worker_id, modified_task);
                send(client_socket, "OK", 2, 0);

                // Update the worker's load (add the task size)
                selected_worker->load += strlen(task);

                // Add task to the queue (if needed)
                TaskInfo task_info = { selected_worker->worker_id, "" };
                strncpy_s(task_info.task, task, BUFFER_SIZE - 1);
                enqueue_task(task_info);
            }
        }
        else {
            printf("No workers found with available load.\n");
            const char* response = "ERROR: No workers available.";
            send(client_socket, response, strlen(response), 0);
        }
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
    InitializeCriticalSection(&task_queue_mutex);

    // Start the worker reponse handler thread
    response_thread = CreateThread(NULL, 0, worker_response_handler, NULL, 0, NULL);

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
    DeleteCriticalSection(&task_queue_mutex);
    WSACleanup();
}
