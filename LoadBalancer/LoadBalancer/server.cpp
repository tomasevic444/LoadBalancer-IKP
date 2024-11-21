#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>  // Include Winsock2 header
#include <windows.h>   // For general Windows-specific functions

#pragma comment(lib, "ws2_32.lib")  // Link Winsock library

#define PORT 5059         // Port number for the server
#define BUFFER_SIZE 1024  // Size of the message buffer

void start_server() {
    WSADATA wsa;
    SOCKET server_fd, client_fd;
    struct sockaddr_in server_address, client_address;
    char buffer[BUFFER_SIZE];
    int client_address_len = sizeof(client_address);

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error Code: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    // Create a socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code: %d\n", WSAGetLastError());
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    // Configure the server address
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    // Bind the socket to the port
    if (bind(server_fd, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        printf("Bind failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, 5) == SOCKET_ERROR) {
        printf("Listen failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", PORT);

    // Accept and process client connections
    while ((client_fd = accept(server_fd, (struct sockaddr*)&client_address, &client_address_len)) != INVALID_SOCKET) {
        printf("Client connected.\n");

        // Handle messages from the client
        while (1) {
            int read_size = recv(client_fd, buffer, BUFFER_SIZE, 0);

            if (read_size > 0) {
                buffer[read_size] = '\0';  // Null-terminate the received data
                printf("Received: %s\n", buffer);

                // Optionally, send an acknowledgment back to the client
                send(client_fd, "Message received\n", 17, 0);
            }
            else if (read_size == 0) {
                printf("Client disconnected.\n");
                break;  // Exit the loop when the client disconnects
            }
            else {
                printf("Error receiving data. Error Code: %d\n", WSAGetLastError());
                break;  // Exit the loop on a receive error
            }
        }

        closesocket(client_fd);  // Close the client connection
    }

    if (client_fd == INVALID_SOCKET) {
        printf("Accept failed. Error Code: %d\n", WSAGetLastError());
    }

    // Clean up
    closesocket(server_fd);  // Close the server socket
    WSACleanup();           // Cleanup Winsock
}
