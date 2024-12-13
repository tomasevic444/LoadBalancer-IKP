#pragma once
#include "pch.h"
#include "framework.h"
#include "queue.h"

// Initialize the task queue
TaskNode* task_queue_head = NULL;
TaskNode* task_queue_tail = NULL;


// Adding and removing tasks to queue (for now unused but will probably be important later)
void enqueue_task(TaskInfo task_info) {

    TaskNode* new_node = (TaskNode*)malloc(sizeof(TaskNode));
    new_node->task_info = task_info;
    new_node->next = NULL;

    if (task_queue_tail) {
        task_queue_tail->next = new_node;
    }
    else {
        task_queue_head = new_node;
    }
    task_queue_tail = new_node;

}
TaskInfo dequeue_task() {

    TaskInfo task_info = { 0 };
    if (task_queue_head) {
        TaskNode* temp = task_queue_head;
        task_info = temp->task_info;
        task_queue_head = task_queue_head->next;
        if (!task_queue_head) {
            task_queue_tail = NULL;
        }
        free(temp);
    }

    return task_info;
}