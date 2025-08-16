#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

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

    // Set timeouts for sending and receiving
    int timeout = 5000;  // 5 seconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // Configure the server address
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0) {
        printf("Invalid address/ Address not supported\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    Sleep(3000);

    // Connect to the server
    if (connect(sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        printf("Connection failed. Error Code: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Send multiple messages
    printf("Connected to server.\n");
    while (1) {
        printf("-----------------------------------------------------------\nType 'ADD_WORKER' to add another worker to the server.\nType 'exit' to quit.");
        printf("\nEnter data: ");
        if (fgets(message, BUFFER_SIZE, stdin) == NULL) {
            fprintf(stderr, "Error reading input or EOF encountered.\n");
            continue;
        }

        message[strcspn(message, "\n")] = '\0';

        if (strlen(message) == 0) {
            printf("No input provided. Please try again.\n");
        }
        if (strcmp(message, "exit") == 0) {
            printf("Exiting...\n");
            break;
        }

        // Send the message
        int total_sent = 0;
        int message_len = strlen(message);
        while (total_sent < message_len) {
            int bytes_sent = send(sock, message + total_sent, message_len - total_sent, 0);
            if (bytes_sent == SOCKET_ERROR) {
                printf("Failed to send message. Error Code: %d\n", WSAGetLastError());
                break;
            }
            total_sent += bytes_sent;
        }

        // Wait for a response from the server
        int response_len = recv(sock, message, BUFFER_SIZE, 0);
        if (response_len > 0) {
            message[response_len] = '\0';  // Null-terminate the received data
            printf("%s\n", message);
        }
        else if (response_len == 0) {
            printf("Server disconnected.\n");
            break;
        }
        else {
            printf("Failed to receive server response. Error Code: %d\n", WSAGetLastError());
            break;
        }
    }

    // Gracefully close the connection
    shutdown(sock, SD_BOTH);
    closesocket(sock);
    WSACleanup();
    return 0;
}
