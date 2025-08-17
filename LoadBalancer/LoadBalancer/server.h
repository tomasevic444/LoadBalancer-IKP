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

// Worker structure
typedef struct {
    int worker_id;
    SOCKET task_socket; // For communication with the server
    HANDLE thread_handle; // handle for running the worker function
    int port;
    int load;
    HashMap* data_store;
    int synced;
    volatile LONG is_in_use; // Use LONG for Interlocked functions
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