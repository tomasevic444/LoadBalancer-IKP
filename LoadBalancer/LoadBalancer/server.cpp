#pragma comment(lib, "ws2_32.lib")
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

// DEFINE a recent Windows version (0x0600 = Windows Vista)
#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h> 

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "server.h"
#include "constants.h"
#include "queue.h"

Worker* workers = NULL;
int worker_count = 0;
int worker_capacity = 0;

typedef struct {
    SOCKET client_socket;
    struct sockaddr_in client_address;
} ClientHandlerArgs;

// Critical section mutex-es for syncronizing tasks
CRITICAL_SECTION worker_mutex;
CRITICAL_SECTION task_queue_mutex;

CRITICAL_SECTION terminal_mutex;

// useful for debugging
int sync_active = 0;

//handle for the response thread function
HANDLE response_thread;

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

        //printf("ENTERING the thread response mutex\n");
        EnterCriticalSection(&worker_mutex);
        //printf("IN the thread response mutex\n");
        int max_sd = 0;
        __try {
            // Add worker sockets to the fd_set
            for (int i = 0; i < worker_count; i++) {
                Worker* worker = &workers[i];
                if (worker->task_socket != INVALID_SOCKET) {
                    FD_SET(worker->task_socket, &readfds);
                    if (worker->task_socket > max_sd) {
                        max_sd = worker->task_socket;
                    }
                }
            }
        }
        __finally {
            //printf("LEAVING the thread response mutex\n");
            LeaveCriticalSection(&worker_mutex);
            //printf("LEFT the thread response mutex\n");
        }

        // Set timeout for select
        timeout.tv_sec = 1;  // 1-second timeout
        timeout.tv_usec = 0;

        // Monitor the sockets for readability
        activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if (activity > 0) {
            //printf("ENTERING the thread response mutex\n");
            EnterCriticalSection(&worker_mutex);
            //("IN the thread response mutex\n");
            __try {
                for (int i = 0; i < worker_count; i++) {
                    Worker* worker = &workers[i];
                    if (worker->task_socket != INVALID_SOCKET && FD_ISSET(worker->task_socket, &readfds)) {
                        // Use a non-blocking approach with recv
                        u_long mode = 1; // Set non-blocking mode
                        ioctlsocket(worker->task_socket, FIONBIO, &mode);

                        //printf("FD IS SET, attempting to recv\n");
                        int bytes_received = recv(worker->task_socket, buffer, BUFFER_SIZE - 1, 0);

                        if (bytes_received > 0) {
                            buffer[bytes_received] = '\0';
                             printf("Worker %d responded: %s\n", worker->worker_id, buffer);

                            // Start the sync procedure
                            sync_active = 1;
                            for (int j = 0; j < worker_count; j++) {
                                if (i != j) workers[j].synced = 0;
                            }

                            // Broadcast data to all workers except the sending one
                            for (int j = 0; j < worker_count; j++) {
                                if (i != j) {
                                    if (send(workers[j].task_socket, buffer, bytes_received, 0) == SOCKET_ERROR) {
                                        printf("Failed to sync Worker %d. Error: %d\n", workers[j].worker_id, WSAGetLastError());
                                    }
                                }
                            }
                            sync_active = 0;

                        }
                        else if (bytes_received == 0) {
                            printf("Worker %d disconnected.\n", worker->worker_id);
                            closesocket(worker->task_socket);
                            worker->task_socket = INVALID_SOCKET;
                        }
                        else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                            // Ignore non-blocking errors
                            printf("Worker %d: recv failed. Error: %d\n", worker->worker_id, WSAGetLastError());
                        }

                        // Reset the socket to blocking mode
                        mode = 0;
                        ioctlsocket(worker->task_socket, FIONBIO, &mode);
                    }
                }
            }
            __finally {
                //printf("LEAVING the thread response mutex\n");
                LeaveCriticalSection(&worker_mutex);
                //printf("LEFT the thread response mutex\n");
            }
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
    //printf("Worker %d: Created listening socket (fd: %d)\n", worker->worker_id, listen_socket);

    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = INADDR_ANY;
    worker_address.sin_port = 0; // Let the system assign a random port

    if (bind(listen_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) == SOCKET_ERROR) {
        printf("Worker %d: Failed to bind listening socket. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }
    //printf("Worker %d: Bound to port (auto assigned)\n", worker->worker_id);

    if (listen(listen_socket, 1) == SOCKET_ERROR) {
        printf("Worker %d: Failed to listen on socket. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }
    printf("Worker %d: Listening on socket %d...\n", worker->worker_id, listen_socket);

    // Get the assigned port
    int address_length = sizeof(worker_address);
    getsockname(listen_socket, (struct sockaddr*)&worker_address, &address_length);
    int assigned_port = ntohs(worker_address.sin_port);
    worker->port = assigned_port;
    printf("Worker %d: Assigned to port %d\n", worker->worker_id, assigned_port);

    // Wait for connection from server
    connection_socket = accept(listen_socket, NULL, NULL);
    if (connection_socket == INVALID_SOCKET) {
        printf("Worker %d: Failed to accept connection. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }
    printf("Worker %d: Accepted connection (fd: %d)\n", worker->worker_id, connection_socket);

    while (1) {

        // Receive tasks from the server
        read_size = recv(connection_socket, buffer, BUFFER_SIZE - 1, 0);
        //printf("Worker %d: Received %d bytes\n", worker->worker_id, read_size);

        if (read_size > 0) {
            buffer[read_size] = '\0';  // Null-terminate the received data
            //printf("Worker %d: Received data: %s\n", worker->worker_id, buffer);

            if (strncmp(buffer, "STORE:", 6) == 0) {
                // Extract key and value
                char key[BUFFER_SIZE], value[BUFFER_SIZE];
                sscanf_s(buffer + 6, "%s", value, _countof(value));
                unsigned long key_hash = hash_function(value);
                sprintf_s(key, "%lu", key_hash); // Convert hash to string

                if (insert(worker->data_store, key, _strdup(value)) == 0) {
                    EnterCriticalSection(&terminal_mutex);
                    printf("Worker %d stored data: %s -> %s\n", worker->worker_id, key, value);
                    print_hash_map(worker->data_store);
                    LeaveCriticalSection(&terminal_mutex);
                }
                else {
                    printf("Worker %d failed to store data: %s -> %s\n", worker->worker_id, key, value);
                }

                if (worker->synced == 1) {
                    const char* response = buffer;
                    int bytes_sent = send(connection_socket, response, read_size, 0);
                    printf("Worker %d: Sent %d bytes back: %s\n", worker->worker_id, bytes_sent, response);
                }
                else {
                    worker->synced = 1;
                    printf("Worker %d is now synced.\n", worker->worker_id);
                }
            }
            else if (strncmp(buffer, "GET_ALL:", 7) == 0) {
                // Retrieve from the worker's hash map
                int count = 0;
                KeyValuePair* all_values = get_all_keys_values(worker->data_store, &count);

                if (all_values) {
                    char response_buffer[BUFFER_SIZE];
                    int response_len = 0;

                    // Format all key-value pairs as "key:value" and separate them by newlines
                    for (int i = 0; i < count; i++) {
                        int bytes_written = snprintf(response_buffer + response_len,
                            sizeof(response_buffer) - response_len,
                            "%s:%s\n", all_values[i].key, (char*)all_values[i].value);
                        if (bytes_written < 0 || response_len + bytes_written >= sizeof(response_buffer)) {
                            printf("Buffer overflow or formatting error.\n");
                            break;
                        }
                        response_len += bytes_written;
                    }

                    int bytes_sent = send(connection_socket, response_buffer, response_len, 0);
                    printf("Worker %d: Sent GET_ALL response, %d bytes.\n", worker->worker_id, bytes_sent);

                    free(all_values);  // Clean up memory if needed
                }
                else {
                    printf("Worker %d: No key-value pairs found.\n", worker->worker_id);
                }
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

    // Check and expand capacity if needed
    if (worker_count == MAX_WORKER_COUNT) {
        return 1;
    }

    // Initialize the new worker in the array
    Worker* new_worker = &workers[worker_count];
    memset(new_worker, 0, sizeof(Worker));
    new_worker->worker_id = worker_count + 1;
    new_worker->load = 0;
    new_worker->task_socket = INVALID_SOCKET;

    // Create thread for worker
    new_worker->thread_handle = CreateThread(NULL, 0, worker_function, (LPVOID)new_worker, 0, NULL);
    if (new_worker->thread_handle == NULL) {
        printf("Failed to create thread for Worker %d.\n", new_worker->worker_id);
        free(new_worker);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    // Wait for worker to initialize and retrieve its port
    Sleep(100);

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
    worker_address.sin_port = htons(worker_port);

    if (connect(server_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) == SOCKET_ERROR) {
        printf("Failed to connect to Worker %d on port %d. Error: %d\n", new_worker->worker_id, worker_port, WSAGetLastError());
        closesocket(server_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    new_worker->task_socket = server_socket;
    printf("Server connected to Worker %d on port %d.\n", new_worker->worker_id, worker_port);

    new_worker->data_store = create_hash_map();
    if (!new_worker->data_store) {
        printf("Failed to initialize hashmap for Worker %d.\n", new_worker->worker_id);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    new_worker->synced = 1;

    worker_count++;
    LeaveCriticalSection(&worker_mutex);
    return 0;
}

// Sync the new worker by fetching data from any synced worker and populating the hashmap

void sync_new_worker() {
    EnterCriticalSection(&worker_mutex);

    Worker* new_worker = &workers[worker_count - 1];
    Worker* existing_worker = &workers[0];  // Assuming the first worker to be the source

    printf("Syncing new worker %d with existing worker %d...\n", new_worker->worker_id, existing_worker->worker_id);

    // Send GET_ALL request to the existing worker
    const char* get_all_request = "GET_ALL";
    printf("Sending GET_ALL request to Worker %d...\n", existing_worker->worker_id);
    if (send(existing_worker->task_socket, get_all_request, strlen(get_all_request), 0) == SOCKET_ERROR) {
        printf("Failed to send GET_ALL request to Worker %d. Error: %d\n", existing_worker->worker_id, WSAGetLastError());
        LeaveCriticalSection(&worker_mutex);
        return;
    }
    printf("GET_ALL request sent to Worker %d.\n", existing_worker->worker_id);

    // Prepare fd_set for select call
    fd_set readfds;
    struct timeval timeout;
    int activity;

    FD_ZERO(&readfds);
    FD_SET(existing_worker->task_socket, &readfds);

    timeout.tv_sec = 5;  // Set timeout to 5 seconds
    timeout.tv_usec = 0;

    // Wait for response using select
    printf("Waiting for response from Worker %d...\n", existing_worker->worker_id);
    activity = select(existing_worker->task_socket + 1, &readfds, NULL, NULL, &timeout);

    if (activity <= 0) {
        printf("Timeout or error waiting for data from Worker %d.\n", existing_worker->worker_id);
        LeaveCriticalSection(&worker_mutex);
        return;
    }

    // If the socket is ready to read, receive the data
    if (FD_ISSET(existing_worker->task_socket, &readfds)) {
        char buffer[BUFFER_SIZE];
        int bytes_received = recv(existing_worker->task_socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            printf("Failed to receive data from Worker %d. Error: %d\n", existing_worker->worker_id, WSAGetLastError());
            LeaveCriticalSection(&worker_mutex);
            return;
        }

        buffer[bytes_received] = '\0';  // Null-terminate the received data
        printf("Received data from Worker %d.\n", existing_worker->worker_id);

        char* context = NULL;
        char* line = strtok_s(buffer, "\n", &context);
        while (line != NULL) {
            // Skip empty lines or lines with only whitespace
            if (strlen(line) == 0 || strspn(line, " \t") == strlen(line)) {
                line = strtok_s(NULL, "\n", &context);
                continue;
            }

            // Parse key-value pair
            char key[BUFFER_SIZE] = { 0 };
            char value[BUFFER_SIZE] = { 0 };

            char* colon_pos = strchr(line, ':');
            if (colon_pos != NULL) {
                size_t key_len = colon_pos - line;
                size_t value_len = strlen(line) - key_len - 1;

                strncpy_s(key, sizeof(key), line, key_len);
                strncpy_s(value, sizeof(value), colon_pos + 1, value_len);

                // Trim spaces from key and value
                char* key_end = key + strlen(key) - 1;
                while (key_end >= key && isspace(*key_end)) {
                    *key_end = '\0';
                    key_end--;
                }

                char* value_end = value + strlen(value) - 1;
                while (value_end >= value && isspace(*value_end)) {
                    *value_end = '\0';
                    value_end--;
                }

                // Send STORE request to the new worker
                char store_request[BUFFER_SIZE];
                snprintf(store_request, sizeof(store_request), "STORE:%s:%s", key, value);

                printf("Sending STORE request to Worker %d: %s\n", new_worker->worker_id, store_request);
                if (send(new_worker->task_socket, store_request, strlen(store_request), 0) == SOCKET_ERROR) {
                    printf("Failed to send STORE request to Worker %d. Error: %d\n", new_worker->worker_id, WSAGetLastError());
                    LeaveCriticalSection(&worker_mutex);
                    return;
                }

                // Wait for acknowledgment
                char ack_buffer[BUFFER_SIZE];
                int ack_bytes_received = recv(new_worker->task_socket, ack_buffer, sizeof(ack_buffer), 0);
                if (ack_bytes_received <= 0) {
                    printf("Failed to receive acknowledgment from Worker %d. Error: %d\n", new_worker->worker_id, WSAGetLastError());
                    LeaveCriticalSection(&worker_mutex);
                    return;
                }

                ack_buffer[ack_bytes_received] = '\0';
                printf("Received acknowledgment from Worker %d: %s\n", new_worker->worker_id, ack_buffer);
            }
            else {
                printf("Failed to parse key-value pair from line: %s\n", line);
            }

            line = strtok_s(NULL, "\n", &context);
        }
    }

    new_worker->synced = 1;
    printf("New worker %d is fully synchronized with existing data from Worker %d.\n", new_worker->worker_id, existing_worker->worker_id);
    LeaveCriticalSection(&worker_mutex);
    printf("Exited the critical section from sync.\n");
}


// Initialize initial workers
void initialize_workers(int n) {

    worker_capacity = n;
    workers = (Worker*)malloc(MAX_WORKER_COUNT * sizeof(Worker)); //allocate memory for all the possible workers
    for (int i = 0; i < n; i++) {
        add_worker();
    }
    printf("Initialized %d workers.\n", n);
}

void distribute_task_to_worker(const char* task, SOCKET client_socket) {
    static int current_worker = 0;

    //printf("entering the distribution mutex\n");
    EnterCriticalSection(&worker_mutex);
    //printf("ENTERED THE DISTRIBUTION MUTEX\n");

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
    //printf("Leaving the distribution mutex\n");
    LeaveCriticalSection(&worker_mutex);
    //printf("left the distribution mutex\n");
}

DWORD WINAPI handle_client(LPVOID args) {
    // Cast the void pointer back to our struct and get the client socket
    ClientHandlerArgs* handler_args = (ClientHandlerArgs*)args;
    SOCKET client_socket = handler_args->client_socket;

    // We can get the client's IP address for logging
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &handler_args->client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
    int client_port = ntohs(handler_args->client_address.sin_port);

    printf("Handling connection for client %s:%d\n", client_ip, client_port);

    // The arguments struct is no longer needed, so we free it to prevent a memory leak.
    free(handler_args);

    char buffer[BUFFER_SIZE];
    int bytes_received;

    // Loop to receive data from this specific client
    while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0'; // Null-terminate the received string

        // Check for the ADD_WORKER command
        if (strcmp(buffer, ADD_WORKER_CMD) == 0) {
            printf("Received 'ADD_WORKER' command from client %s:%d\n", client_ip, client_port);
            int result = add_worker();
            if (result != 0) {
                send(client_socket, "ERROR: Max workers reached or failed to add.", 44, 0);
            }
            else {
                printf("Syncing new worker with existing data...\n");
                sync_new_worker(); // Note: This is still a blocking operation
                send(client_socket, "SUCCESS: Worker added and synced.", 32, 0);
            }
        }
        else {
            // It's a regular task
            printf("Received task from client %s:%d: %s\n", client_ip, client_port, buffer);
            distribute_task_to_worker(buffer, client_socket);
        }
    }

    // If recv returns 0, the client gracefully closed the connection.
    // If it returns a negative number, an error occurred.
    if (bytes_received == 0) {
        printf("Client %s:%d disconnected gracefully.\n", client_ip, client_port);
    }
    else {
        printf("Connection lost with client %s:%d. Error: %d\n", client_ip, client_port, WSAGetLastError());
    }

    // Clean up: close the socket for this client and exit the thread.
    closesocket(client_socket);
    return 0;
}


void start_server() {
    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_address, client_address;
    int client_address_len = sizeof(client_address);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    InitializeCriticalSection(&worker_mutex);
    InitializeCriticalSection(&task_queue_mutex);
    InitializeCriticalSection(&terminal_mutex);

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

    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) { 
        printf("Listen failed. Error: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port 5059... Ready to accept multiple clients.\n");


    while (1) {
        //  Accept a new connection. This call blocks until a client connects.
        client_socket = accept(server_socket, (struct sockaddr*)&client_address, &client_address_len);
        if (client_socket == INVALID_SOCKET) {
            printf("Failed to accept connection. Error: %d\n", WSAGetLastError());
            continue; // Go back to waiting for another connection
        }

        //  Allocate memory for the thread arguments.
        ClientHandlerArgs* args = (ClientHandlerArgs*)malloc(sizeof(ClientHandlerArgs));
        if (args == NULL) {
            printf("ERROR: Failed to allocate memory for client handler arguments.\n");
            closesocket(client_socket); // Clean up the connection
            continue;
        }
        args->client_socket = client_socket;
        args->client_address = client_address;

        //  Create a new thread to handle this client.
        HANDLE thread_handle = CreateThread(NULL, 0, handle_client, args, 0, NULL);
        if (thread_handle == NULL) {
            printf("ERROR: Failed to create thread for new client.\n");
            free(args); // Clean up the allocated memory
            closesocket(client_socket); // Clean up the connection
        }
        else {
            // We've successfully launched a thread to handle the client.
            // We don't need to wait for it, so we can close the handle.
            // The thread will continue to run in the background.
            CloseHandle(thread_handle);
        }
    }

    // Cleanup (in a real server, you'd need a way to break the loop to reach this)
    closesocket(server_socket);
    DeleteCriticalSection(&worker_mutex);
    DeleteCriticalSection(&task_queue_mutex);
    DeleteCriticalSection(&terminal_mutex);
    WSACleanup();
}
