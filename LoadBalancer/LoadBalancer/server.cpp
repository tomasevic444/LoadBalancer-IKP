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

        EnterCriticalSection(&worker_mutex);
        int max_sd = 0;
        __try {
            // Add worker sockets to the fd_set
            for (int i = 0; i < worker_count; i++) {
                Worker* worker = &workers[i];

                if (worker->task_socket != INVALID_SOCKET && worker->is_in_use == 0) {
                    FD_SET(worker->task_socket, &readfds);
                    if (worker->task_socket > max_sd) {
                        max_sd = worker->task_socket;
                    }
                }
            }
        }
        __finally {
            LeaveCriticalSection(&worker_mutex);
        }

        // Set timeout for select
        timeout.tv_sec = 1;  // 1-second timeout
        timeout.tv_usec = 0;

        // Monitor the sockets for readability
        activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if (activity > 0) {
            EnterCriticalSection(&worker_mutex);
            __try {
                for (int i = 0; i < worker_count; i++) {
                    Worker* worker = &workers[i];
                    if (worker->task_socket != INVALID_SOCKET && FD_ISSET(worker->task_socket, &readfds)) {
                        u_long mode = 1; // Set non-blocking mode
                        ioctlsocket(worker->task_socket, FIONBIO, &mode);

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
                LeaveCriticalSection(&worker_mutex);
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
    printf("Worker %d: Assigned to port %d\n", worker->worker_id, assigned_port);

    // Wait for connection from server
    connection_socket = accept(listen_socket, NULL, NULL);
    if (connection_socket == INVALID_SOCKET) {
        printf("Worker %d: Failed to accept connection. Error: %d\n", worker->worker_id, WSAGetLastError());
        closesocket(listen_socket);
        return 1;
    }
    printf("Worker %d: Accepted connection (fd: %d)\n", worker->worker_id, connection_socket);

    while ((read_size = recv(connection_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[read_size] = '\0';

        if (strncmp(buffer, "STORE:", 6) == 0) {
            char key[BUFFER_SIZE], value[BUFFER_SIZE];
            sscanf_s(buffer + 6, "%s", value, (unsigned int)sizeof(value));
            unsigned long key_hash = hash_function(value);
            sprintf_s(key, sizeof(key), "%lu", key_hash); // Convert hash to string

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
                send(connection_socket, buffer, read_size, 0);
            }
            else {
                worker->synced = 1;
                printf("Worker %d is now synced.\n", worker->worker_id);
            }
        }
        else if (strncmp(buffer, "GET_ALL:", 8) == 0) {
            int count = 0;
            KeyValuePair* all_values = get_all_keys_values(worker->data_store, &count);

            if (all_values && count > 0) {
                char response_buffer[BUFFER_SIZE] = { 0 };
                int response_len = 0;

                for (int i = 0; i < count; i++) {
                    response_len += snprintf(response_buffer + response_len, sizeof(response_buffer) - response_len,
                        "%s:%s\n", all_values[i].key, (char*)all_values[i].value);
                }
                send(connection_socket, response_buffer, response_len, 0);
                printf("Worker %d: Sent GET_ALL response with %d items.\n", worker->worker_id, count);
                free(all_values);
            }
            else {

                // Send a single newline to unambiguously signal "no data".
                printf("Worker %d: No data found. Sending sync completion signal.\\n", worker->worker_id);
                send(connection_socket, "\n", 1, 0);
            }
        }
    }

    if (read_size == 0) {
        printf("Worker %d: Connection closed by server.\n", worker->worker_id);
    }
    else {
        printf("Worker %d: recv failed. Error: %d\n", worker->worker_id, WSAGetLastError());
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
    Worker* new_worker = NULL;
    Worker* existing_worker = NULL;

    // Step 1: Briefly lock to safely get pointers to the workers.
    EnterCriticalSection(&worker_mutex);
    if (worker_count <= 1) {
        printf("Not enough existing workers to sync from. New worker starts empty.\n");
        if (worker_count > 0) {
            workers[worker_count - 1].synced = 1;
        }
        LeaveCriticalSection(&worker_mutex);
        return;
    }
    new_worker = &workers[worker_count - 1];
    existing_worker = &workers[0];
    LeaveCriticalSection(&worker_mutex); // Release the lock immediately.

    // Step 2: Claim the existing worker's socket to prevent the race condition.
    InterlockedExchange(&existing_worker->is_in_use, 1);
    printf("Syncing new worker %d with existing worker %d (socket claimed)...\n", new_worker->worker_id, existing_worker->worker_id);

    // Step 3: Send the GET_ALL request.
    const char* get_all_request = "GET_ALL:";
    if (send(existing_worker->task_socket, get_all_request, strlen(get_all_request), 0) == SOCKET_ERROR) {
        printf("Failed to send GET_ALL request to Worker %d. Error: %d\n", existing_worker->worker_id, WSAGetLastError());
        InterlockedExchange(&existing_worker->is_in_use, 0); // Always release the claim
        return;
    }

    // Step 4: Wait for the response.
    fd_set readfds;
    struct timeval timeout;
    FD_ZERO(&readfds);
    FD_SET(existing_worker->task_socket, &readfds);
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    int activity = select(existing_worker->task_socket + 1, &readfds, NULL, NULL, &timeout);

    if (activity <= 0) {
        printf("Timeout or error waiting for sync data from Worker %d.\n", existing_worker->worker_id);
    }
    else if (FD_ISSET(existing_worker->task_socket, &readfds)) {
        char buffer[BUFFER_SIZE];
        int bytes_received = recv(existing_worker->task_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received < 0) {
            printf("Failed to receive sync data from Worker %d. Error: %d\n", existing_worker->worker_id, WSAGetLastError());
        }
        // Case 1: The existing worker was empty.
        else if (bytes_received == 1 && buffer[0] == '\n') {
            printf("Received empty data signal from Worker %d. Sync complete.\n", existing_worker->worker_id);
        }
        // Case 2: The existing worker had data.
        else if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            printf("Received %d bytes of sync data from Worker %d. Applying to new worker...\n", bytes_received, existing_worker->worker_id);


            char* context = NULL;
            char* line = strtok_s(buffer, "\n", &context);
            while (line != NULL) {
                char value[BUFFER_SIZE];

                // Find the colon in the "key:value" string
                char* colon_pos = strchr(line, ':');
                if (colon_pos != NULL) {
                    // Copy the part of the string *after* the colon into 'value'
                    strcpy_s(value, sizeof(value), colon_pos + 1);

                    // Now, construct a proper "STORE:" command for the new worker
                    char store_command[BUFFER_SIZE];
                    snprintf(store_command, sizeof(store_command), "STORE: %s", value);

                    printf("  -> Forwarding item '%s' to new worker %d\n", value, new_worker->worker_id);

                    // Send the command to the new worker's socket
                    if (send(new_worker->task_socket, store_command, strlen(store_command), 0) == SOCKET_ERROR) {
                        printf("Error: Failed to send synced item to new worker. Aborting sync.\n");
                        break; // Exit the while loop on error
                    }
                    // In a more robust system, you might wait for an acknowledgment here.
                }

                // Get the next line from the buffer
                line = strtok_s(NULL, "\n", &context);
            }
        }
    }

    // After syncing, mark the new worker as ready to participate in replication.
    new_worker->synced = 1;

    // Step 5: Release the claim on the socket so the background handler can monitor it again.
    InterlockedExchange(&existing_worker->is_in_use, 0);
    printf("Sync for worker %d complete (socket released).\n", new_worker->worker_id);
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
