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


// Function declarations
DWORD WINAPI worker_function(LPVOID args);
void initialize_workers(int n);
int add_worker();
void distribute_task_to_worker(const char* task);
void start_server();
#endif