#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>  // Required for inet_pton

#pragma comment(lib, "ws2_32.lib")

#define PORT 5059
#define BUFFER_SIZE 1024

int main() {
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in server_address;
    char message[BUFFER_SIZE];

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    // Create a socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Configure the server address
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0) {
        printf("Invalid address/ Address not supported\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Connect to the server
    if (connect(sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        printf("Connection failed. Error Code: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Send multiple messages
    printf("Connected to server. Type 'exit' to quit.\n");
    while (1) {
        printf("Enter a message to send: ");
        fgets(message, BUFFER_SIZE, stdin);

        // Remove trailing newline character from input
        message[strcspn(message, "\n")] = 0;

        // Check for exit command
        if (strcmp(message, "exit") == 0) {
            printf("Exiting...\n");
            break;
        }

        // Send the message
        if (send(sock, message, strlen(message), 0) == SOCKET_ERROR) {
            printf("Failed to send message. Error Code: %d\n", WSAGetLastError());
            break;
        }

        printf("Message sent: %s\n", message);
    }

    // Clean up
    closesocket(sock);
    WSACleanup();
    return 0;
}
