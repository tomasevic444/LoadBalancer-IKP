#ifndef SERVER_H
#define SERVER_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#define PORT 5059         // Port number for the server
#define BUFFER_SIZE 1024  // Size of the message buffer
#define ADD_WORKER_CODE "ADD_WORKER"

void start_server();      // Function to start the server


#endif