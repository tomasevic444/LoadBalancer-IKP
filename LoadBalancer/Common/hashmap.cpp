// Common.cpp : Defines the functions for the static library.
//

#include "pch.h"
#include "framework.h"
#include "hashmap.h"

// Simple hash function to convert a string to an index
unsigned int hash(const char* key) {
    unsigned int hash_value = 0;
    while (*key) {
        hash_value = (hash_value * 31) + *key++; // 31 is a prime number multiplier
    }
    return hash_value % HASH_MAP_SIZE;
}

// Create a new hash map
HashMap* create_hash_map() {
    HashMap* map = (HashMap*)malloc(sizeof(HashMap));
    if (map == NULL) {
        printf("Error: Memory allocation failed for hash map.\n");
        return NULL;
    }

    // Initialize the table to NULL
    for (int i = 0; i < HASH_MAP_SIZE; i++) {
        map->table[i] = NULL;
    }

    return map;
}

// Destroy the hash map and free memory
void destroy_hash_map(HashMap* map) {
    if (map == NULL) return;

    for (int i = 0; i < HASH_MAP_SIZE; i++) {
        HashMapEntry* entry = map->table[i];
        while (entry != NULL) {
            HashMapEntry* temp = entry;
            entry = entry->next;
            free(temp->key);   // Free the key
            free(temp);        // Free the entry
        }
    }
    free(map); // Free the hash map
}

// Insert a key-value pair into the hash map
int insert(HashMap* map, const char* key, void* value) {
    if (map == NULL || key == NULL) return -1;

    unsigned int index = hash(key);
    HashMapEntry* new_entry = (HashMapEntry*)malloc(sizeof(HashMapEntry));
    if (new_entry == NULL) {
        printf("Error: Memory allocation failed for new entry.\n");
        return -1;
    }

    new_entry->key = _strdup(key); // Duplicate the key string
    new_entry->value = value;     // Set the value
    new_entry->next = NULL;

    // If no collision, just insert the new entry at the index
    if (map->table[index] == NULL) {
        map->table[index] = new_entry;
    }
    else {
        // Handle collision using chaining (linked list)
        HashMapEntry* current = map->table[index];
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_entry;
    }

    return 0; // Success
}

// Retrieve a value by its key
void* get(HashMap* map, const char* key) {
    if (map == NULL || key == NULL) return NULL;

    unsigned int index = hash(key);
    HashMapEntry* entry = map->table[index];

    // Traverse the linked list at the hashed index
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            return entry->value; // Found the key, return the value
        }
        entry = entry->next;
    }

    return NULL; // Key not found
}

// Remove a key-value pair from the hash map
int remove_key(HashMap* map, const char* key) {
    if (map == NULL || key == NULL) return -1;

    unsigned int index = hash(key);
    HashMapEntry* entry = map->table[index];
    HashMapEntry* prev = NULL;

    // Traverse the linked list to find the entry to remove
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            // Found the entry, remove it from the list
            if (prev == NULL) {
                map->table[index] = entry->next; // Remove from the head of the list
            }
            else {
                prev->next = entry->next; // Remove from the middle or end of the list
            }
            free(entry->key);  // Free the key
            free(entry);       // Free the entry
            return 0;          // Success
        }
        prev = entry;
        entry = entry->next;
    }

    return -1; // Key not found
}

// Print the contents of the hash map for debugging purposes
void print_hash_map(HashMap* map) {
    if (map == NULL) return;

    for (int i = 0; i < HASH_MAP_SIZE; i++) {
        HashMapEntry* entry = map->table[i];
        if (entry != NULL) {
            printf("Bucket %d:\n", i);
            while (entry != NULL) {
                printf("  Key: %s, Value: %p\n", entry->key, entry->value);
                entry = entry->next;
            }
        }
    }
}
