#pragma once
#ifndef SERVER_H
#define SERVER_H

#pragma comment(lib, "ws2_32.lib")
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashmap.h"
#define MAX_WORKER_COUNT 100 // Maximum number of workers
// Worker structure
typedef struct {
    int worker_id;
    SOCKET task_socket;      // Za komunikaciju sa LB-om
    HANDLE thread_handle;
    int port;                // Njegov sopstveni port za slušanje
    int load;
    HashMap* data_store;
    volatile LONG is_in_use;

    // NOVO: Lista portova ostalih radnika (peers)
    int peer_ports[MAX_WORKER_COUNT];
    int peer_count;
} Worker;

// Function declarations
DWORD WINAPI worker_function(LPVOID args);
unsigned long hash_function(const char* str);
DWORD WINAPI worker_response_handler(LPVOID args);
void initialize_workers(int n);
int add_worker();
void sync_new_worker();
void distribute_task_to_worker(const char* task, SOCKET client_socket);
void start_server();
#endif