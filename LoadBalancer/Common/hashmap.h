#pragma once

#ifndef HASHMAP_H
#define HASHMAP_H 

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// The maximum size of the hash table
#define HASH_MAP_SIZE 100

// Define the structure for each element in the hash map (key-value pair)
typedef struct HashMapEntry {
    char* key;            // Key (string)
    void* value;          // Value (generic pointer to allow different types of values)
    struct HashMapEntry* next; // Pointer to the next entry in case of a hash collision (linked list)
} HashMapEntry;

// Define the structure for the hash map
typedef struct HashMap {
    HashMapEntry* table[HASH_MAP_SIZE]; // Array of hash map entries
} HashMap;

typedef struct KeyValuePair {
    char* key;
    void* value;
} KeyValuePair;

// Function declarations
unsigned int hash(const char* key);
HashMap* create_hash_map();
void destroy_hash_map(HashMap* map);
int insert(HashMap* map, const char* key, void* value);
void* get(HashMap* map, const char* key);
int remove_key(HashMap* map, const char* key);
void print_hash_map(HashMap* map);

KeyValuePair* get_all_keys_values(HashMap* map, int* count);

#endif // HASHMAP_H
