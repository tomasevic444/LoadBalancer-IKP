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

void broadcast_peer_list() {
    EnterCriticalSection(&worker_mutex);
    __try {
        if (worker_count == 0) return;

        // Sastavi poruku sa listom portova, npr: "UPDATE_PEERS:50001,50002,50003"
        char msg[BUFFER_SIZE];
        strcpy_s(msg, sizeof(msg), "UPDATE_PEERS:");
        char port_str[16];
        for (int i = 0; i < worker_count; i++) {
            sprintf_s(port_str, sizeof(port_str), "%d", workers[i].port);
            strcat_s(msg, sizeof(msg), port_str);
            if (i < worker_count - 1) {
                strcat_s(msg, sizeof(msg), ",");
            }
        }

        // Pošalji ažuriranu listu SVAKOM workeru
        printf("Broadcasting new peer list to all workers...\n");
        for (int i = 0; i < worker_count; i++) {
            if (workers[i].task_socket != INVALID_SOCKET) {
                send(workers[i].task_socket, msg, (int)strlen(msg), 0);
            }
        }
    }
    __finally {
        LeaveCriticalSection(&worker_mutex);
    }
}

// Trim leading spaces
static const char* ltrim(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return s;
}

// Parses a message of form:
//   TASK:STORE:<value>
//   REPL:STORE:<value>
//   GET_ALL:
// Returns 1 on success and fills out type/op/value; 0 otherwise.
int parse_message(const char* msg, char* type, size_t type_sz, char* op, size_t op_sz, char* value, size_t value_sz) {
    // Make a local copy to tokenize safely
    char buf[BUFFER_SIZE];
    strncpy_s(buf, sizeof(buf), msg, _TRUNCATE);

    // Remove trailing newlines
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';

    // Special cases
    if (strncmp(buf, "GET_ALL:", 8) == 0) {
        strncpy_s(type, type_sz, "CTRL", _TRUNCATE);
        strncpy_s(op, op_sz, "GET_ALL", _TRUNCATE);
        value[0] = '\0';
        return 1;
    }

    // Expected prefix TYPE:...
    char* first_colon = strchr(buf, ':');
    if (!first_colon) return 0;

    *first_colon = '\0';
    strncpy_s(type, type_sz, buf, _TRUNCATE);

    // Next should be OP:...
    char* rest = first_colon + 1;
    char* second_colon = strchr(rest, ':');
    if (!second_colon) return 0;

    *second_colon = '\0';
    strncpy_s(op, op_sz, rest, _TRUNCATE);

    // Value can contain spaces; take the remainder
    const char* val = second_colon + 1;
    val = ltrim(val);
    strncpy_s(value, value_sz, val, _TRUNCATE);
    return 1;
}

// Builds a message TYPE:OP:VALUE into out buffer
void build_message(char* out, size_t out_sz, const char* type, const char* op, const char* value) {
    if (value && value[0] != '\0') {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s:%s:%s", type, op, value);
    }
    else {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s:%s:", type, op);
    }
}

// Handles the worker responses and synchronizes the workers after each response
DWORD WINAPI worker_response_handler(LPVOID args) {
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;

    while (1) {
        FD_ZERO(&readfds);
        EnterCriticalSection(&worker_mutex);
        int max_sd = 0;
        for (int i = 0; i < worker_count; i++) {
            if (workers[i].task_socket != INVALID_SOCKET) {
                FD_SET(workers[i].task_socket, &readfds);
                if (workers[i].task_socket > max_sd) max_sd = (int)workers[i].task_socket;
            }
        }
        LeaveCriticalSection(&worker_mutex);

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int activity = select(max_sd + 1, &readfds, NULL, NULL, &timeout);

        if (activity > 0) {
            for (int i = 0; i < worker_count; i++) {
                if (workers[i].task_socket != INVALID_SOCKET && FD_ISSET(workers[i].task_socket, &readfds)) {
                    int bytes_received = recv(workers[i].task_socket, buffer, BUFFER_SIZE - 1, 0);
                    if (bytes_received > 0) {
                        buffer[bytes_received] = '\0';
                        // Jedina uloga je da primi potvrdu (ACK) i ispiše je
                        printf("LB received acknowledgment from Worker %d: %s\n", workers[i].worker_id, buffer);
                    }
                    else if (bytes_received == 0) {
                        printf("Worker %d disconnected.\n", workers[i].worker_id);
                        closesocket(workers[i].task_socket);
                        workers[i].task_socket = INVALID_SOCKET;
                        // Nakon diskonekcije, trebalo bi ponovo obavestiti sve o novoj listi
                        broadcast_peer_list();
                    }
                }
            }
        }
    }
    return 0;
}

// Main function that workers execute, it stores or loads data from and to the worker node
DWORD WINAPI worker_function(LPVOID args) {
    Worker* worker = (Worker*)args;
    SOCKET listen_socket, lb_connection_socket;
    struct sockaddr_in worker_address;
    char buffer[BUFFER_SIZE];

    // --- KORAK 1: Kreiraj soket za slušanje konekcija ---
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket == INVALID_SOCKET) { /* ... handle error ... */ return 1; }

    worker_address.sin_family = AF_INET;
    worker_address.sin_addr.s_addr = INADDR_ANY;
    worker_address.sin_port = 0;
    if (bind(listen_socket, (struct sockaddr*)&worker_address, sizeof(worker_address)) != 0) { /* ... handle error ... */ return 1; }
    if (listen(listen_socket, SOMAXCONN) != 0) { /* ... handle error ... */ return 1; }

    int addr_len = sizeof(worker_address);
    getsockname(listen_socket, (struct sockaddr*)&worker_address, &addr_len);
    worker->port = ntohs(worker_address.sin_port);
    printf("Worker %d: Assigned to port %d\n", worker->worker_id, worker->port);

    // --- KORAK 2: Prihvati JEDNU STALNU konekciju od Load Balancera ---
    lb_connection_socket = accept(listen_socket, NULL, NULL);
    if (lb_connection_socket == INVALID_SOCKET) { /* ... handle error ... */ return 1; }
    printf("Worker %d: Accepted connection from Load Balancer.\n", worker->worker_id);

    // --- KORAK 3: Glavna petlja sa select() ---
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(lb_connection_socket, &readfds);
        FD_SET(listen_socket, &readfds);

        int max_sd = (lb_connection_socket > listen_socket) ? (int)lb_connection_socket : (int)listen_socket;

        int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            printf("Worker %d: select() error: %d\n", worker->worker_id, WSAGetLastError());
            break;
        }

        // --- Obrada poruke od Load Balancera ---
        if (FD_ISSET(lb_connection_socket, &readfds)) {
            int read_size = recv(lb_connection_socket, buffer, BUFFER_SIZE - 1, 0);
            if (read_size <= 0) {
                printf("Worker %d: Disconnected from LB.\n", worker->worker_id);
                break;
            }
            buffer[read_size] = '\0';

            char* context = NULL;
            char* command = strtok_s(buffer, ":", &context);
            if (command) {
                if (strcmp(command, "UPDATE_PEERS") == 0) {
                    printf("Worker %d received updated peer list.\n", worker->worker_id);
                    worker->peer_count = 0;
                    char* port_str = strtok_s(context, ",", &context);
                    while (port_str != NULL && worker->peer_count < MAX_WORKER_COUNT) {
                        worker->peer_ports[worker->peer_count++] = atoi(port_str);
                        port_str = strtok_s(NULL, ",", &context);
                    }
                }
                else if (strcmp(command, "TASK") == 0) {
                    char* op = strtok_s(context, ":", &context);
                    char* value = context;
                    if (op && value && strcmp(op, "STORE") == 0) {
                        unsigned long key_hash = hash_function(value);
                        char key_str[21];
                        sprintf_s(key_str, sizeof(key_str), "%lu", key_hash);
                        insert(worker->data_store, key_str, _strdup(value));
                        printf("Worker %d stored data from TASK: %s -> %s\n", worker->worker_id, key_str, value);
                        send(lb_connection_socket, "ACK:OK", 6, 0);

                        printf("Worker %d starting replication to %d peers...\n", worker->worker_id, worker->peer_count - 1);
                        char repl_msg[BUFFER_SIZE];
                        build_message(repl_msg, sizeof(repl_msg), "REPL", "STORE", value);
                        for (int i = 0; i < worker->peer_count; i++) {
                            if (worker->peer_ports[i] != worker->port) {
                                SOCKET peer_socket = socket(AF_INET, SOCK_STREAM, 0);
                                struct sockaddr_in peer_addr;
                                peer_addr.sin_family = AF_INET;
                                peer_addr.sin_port = htons(worker->peer_ports[i]);
                                inet_pton(AF_INET, "127.0.0.1", &peer_addr.sin_addr);
                                if (connect(peer_socket, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) == 0) {
                                    send(peer_socket, repl_msg, (int)strlen(repl_msg), 0);
                                    printf("  -> Worker %d sent REPL to port %d\n", worker->worker_id, worker->peer_ports[i]);
                                }
                                closesocket(peer_socket);
                            }
                        }
                    }
                }
                else if (strcmp(command, "GET_ALL") == 0) {
                    int count = 0;
                    KeyValuePair* all_values = get_all_keys_values(worker->data_store, &count);
                    if (all_values && count > 0) {
                        char response_buffer[BUFFER_SIZE] = { 0 };
                        int response_len = 0;
                        for (int i = 0; i < count; i++) {
                            response_len += snprintf(response_buffer + response_len, sizeof(response_buffer) - response_len,
                                "%s:%s\n", all_values[i].key, (char*)all_values[i].value);
                        }
                        send(lb_connection_socket, response_buffer, response_len, 0);
                        free(all_values);
                    }
                    else {
                        send(lb_connection_socket, "\n", 1, 0);
                    }
                }
            }
        }

        // --- Obrada konekcije od drugog Workera (Replikacija) ---
        if (FD_ISSET(listen_socket, &readfds)) {
            SOCKET peer_socket = accept(listen_socket, NULL, NULL);
            if (peer_socket != INVALID_SOCKET) {
                int read_size = recv(peer_socket, buffer, BUFFER_SIZE - 1, 0);
                if (read_size > 0) {
                    buffer[read_size] = '\0';
                    char* context = NULL;
                    char* command = strtok_s(buffer, ":", &context);
                    if (command && strcmp(command, "REPL") == 0) {
                        char* op = strtok_s(context, ":", &context);
                        char* value = context;
                        if (op && value && strcmp(op, "STORE") == 0) {
                            unsigned long key_hash = hash_function(value);
                            char key_str[21];
                            sprintf_s(key_str, sizeof(key_str), "%lu", key_hash);
                            insert(worker->data_store, key_str, _strdup(value));
                            printf("Worker %d stored data from REPL: %s -> %s\n", worker->worker_id, key_str, value);
                        }
                    }
                }
                closesocket(peer_socket);
            }
        }
    }

    // --- Čišćenje ---
    destroy_hash_map(worker->data_store);
    worker->data_store = NULL;
    closesocket(lb_connection_socket);
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

    if (worker_count == MAX_WORKER_COUNT) {
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    Worker* new_worker = &workers[worker_count];
    memset(new_worker, 0, sizeof(Worker));
    new_worker->worker_id = worker_count + 1;
    new_worker->load = 0;
    new_worker->task_socket = INVALID_SOCKET;

    new_worker->thread_handle = CreateThread(NULL, 0, worker_function, (LPVOID)new_worker, 0, NULL);
    if (new_worker->thread_handle == NULL) {
        printf("Failed to create thread for Worker %d.\n", new_worker->worker_id);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    // Sačekaj da worker inicijalizuje i preuzme svoj port
    Sleep(100);

    // Poveži se sa novim workerom
    if (connect_to_worker(new_worker) != 0) {
        // connect_to_worker već ispisuje grešku, samo izađi
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    new_worker->data_store = create_hash_map();
    if (!new_worker->data_store) {
        printf("Failed to initialize hashmap for Worker %d.\n", new_worker->worker_id);
        closesocket(new_worker->task_socket);
        LeaveCriticalSection(&worker_mutex);
        return 1;
    }

    // UKLONJENO: Linija "new_worker->synced = 1;" je ovde bila i sada je obrisana.

    worker_count++;
    LeaveCriticalSection(&worker_mutex);

    // Obavesti sve workere o novoj listi kolega
    broadcast_peer_list();

    return 0;
}

// Sync the new worker by fetching data from any synced worker and populating the hashmap

void sync_new_worker() {
    Worker* new_worker = NULL;
    Worker* existing_worker = NULL;

    EnterCriticalSection(&worker_mutex);
    if (worker_count <= 1) {
        printf("Not enough existing workers to sync from. New worker starts empty.\n");
        // UKLONJENO: Blok "if (worker_count > 0) { ... }" je ovde bio i sada je obrisan.
        LeaveCriticalSection(&worker_mutex);
        return;
    }
    new_worker = &workers[worker_count - 1];
    existing_worker = &workers[0];
    LeaveCriticalSection(&worker_mutex);

    // Ostatak funkcije je isti...
    InterlockedExchange(&existing_worker->is_in_use, 1);
    printf("Syncing new worker %d with existing worker %d (socket claimed)...\n", new_worker->worker_id, existing_worker->worker_id);

    const char* get_all_request = "GET_ALL:";
    if (send(existing_worker->task_socket, get_all_request, (int)strlen(get_all_request), 0) == SOCKET_ERROR) {
        printf("Failed to send GET_ALL request to Worker %d. Error: %d\n", existing_worker->worker_id, WSAGetLastError());
        InterlockedExchange(&existing_worker->is_in_use, 0);
        return;
    }

    fd_set readfds;
    struct timeval timeout;
    FD_ZERO(&readfds);
    FD_SET(existing_worker->task_socket, &readfds);
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    int activity = select((int)existing_worker->task_socket + 1, &readfds, NULL, NULL, &timeout);

    if (activity > 0 && FD_ISSET(existing_worker->task_socket, &readfds)) {
        char buffer[BUFFER_SIZE];
        int bytes_received = recv(existing_worker->task_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received > 0 && !(bytes_received == 1 && buffer[0] == '\n')) {
            buffer[bytes_received] = '\0';
            printf("Received %d bytes of sync data from Worker %d. Applying to new worker...\n", bytes_received, existing_worker->worker_id);

            char* context = NULL;
            char* line = strtok_s(buffer, "\n", &context);
            while (line != NULL) {
                char* colon_pos = strchr(line, ':');
                if (colon_pos != NULL) {
                    const char* value = colon_pos + 1;
                    char store_command[BUFFER_SIZE];
                    build_message(store_command, sizeof(store_command), "REPL", "STORE", value);
                    printf("  -> Forwarding item '%s' to new worker %d\n", value, new_worker->worker_id);
                    if (send(new_worker->task_socket, store_command, (int)strlen(store_command), 0) == SOCKET_ERROR) {
                        printf("Error: Failed to send synced item to new worker. Aborting sync.\n");
                        break;
                    }
                }
                line = strtok_s(NULL, "\n", &context);
            }
        }
    }

    // UKLONJENO: Linija "new_worker->synced = 1;" je ovde bila i sada je obrisana.

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
    EnterCriticalSection(&worker_mutex);

    if (worker_count == 0) {
        printf("No workers available to handle the task.\n");
        const char* response = "ERROR: No workers available.";
        send(client_socket, response, (int)strlen(response), 0);
    }
    else {
        // Build TASK message for the chosen worker
        char modified_task[BUFFER_SIZE];
        // Ensure exact format: TASK:STORE:<value>  (no extra spaces)
        _snprintf_s(modified_task, BUFFER_SIZE, _TRUNCATE, "TASK:STORE:%s", task);

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

            int bytes_sent = send(selected_worker->task_socket, modified_task, (int)strlen(modified_task), 0);
            if (bytes_sent == SOCKET_ERROR) {
                printf("Failed to send task to Worker %d. Error: %d\n", selected_worker->worker_id, WSAGetLastError());
                const char* response = "ERROR: Failed to assign task.";
                send(client_socket, response, (int)strlen(response), 0);
            }
            else {
                printf("Successfully sent task to Worker %d: %s\n", selected_worker->worker_id, modified_task);
                send(client_socket, "OK", 2, 0);

                // Update the worker's load (add the task size)
                selected_worker->load += (int)strlen(task);

                // Add task to the queue (if needed)
                TaskInfo task_info = { selected_worker->worker_id, "" };
                strncpy_s(task_info.task, task, BUFFER_SIZE - 1);
                enqueue_task(task_info);
            }
        }
        else {
            printf("No workers found with available load.\n");
            const char* response = "ERROR: No workers available.";
            send(client_socket, response, (int)strlen(response), 0);
        }
    }

    LeaveCriticalSection(&worker_mutex);
}

DWORD WINAPI handle_client(LPVOID args) {
    ClientHandlerArgs* handler_args = (ClientHandlerArgs*)args;
    SOCKET client_socket = handler_args->client_socket;

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &handler_args->client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
    int client_port = ntohs(handler_args->client_address.sin_port);

    printf("Handling connection for client %s:%d\n", client_ip, client_port);

    free(handler_args);

    char buffer[BUFFER_SIZE];
    int bytes_received;

    while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';

        if (strcmp(buffer, ADD_WORKER_CMD) == 0) {
            printf("Received 'ADD_WORKER' command from client %s:%d\n", client_ip, client_port);
            int result = add_worker();
            if (result != 0) {
                send(client_socket, "ERROR: Max workers reached or failed to add.", 44, 0);
            }
            else {
                printf("Syncing new worker with existing data...\n");
                sync_new_worker(); // blocking sync is acceptable here
                send(client_socket, "SUCCESS: Worker added and synced.", 32, 0);
            }
        }
        else {
            // Regular task payload is the plain value (e.g., "Hello")
            printf("Received task from client %s:%d: %s\n", client_ip, client_port, buffer);
            distribute_task_to_worker(buffer, client_socket);
        }
    }

    if (bytes_received == 0) {
        printf("Client %s:%d disconnected gracefully.\n", client_ip, client_port);
    }
    else {
        printf("Connection lost with client %s:%d. Error: %d\n", client_ip, client_port, WSAGetLastError());
    }

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
