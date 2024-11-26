#pragma once
#ifndef WORKER_H
#define WORKER_H

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 1024

// Worker structure
typedef struct {
    int worker_id;
    SOCKET task_socket;
    HANDLE thread_handle;
    int port;
    int load;
} Worker;

// Worker function declaration
DWORD WINAPI worker_function(LPVOID args);
void handle_task(Worker* worker, const char* task);
void initialize_workers(int count);

#endif // WORKER_H
