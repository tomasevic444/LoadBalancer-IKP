#pragma once

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "constants.h"

typedef struct {
    int worker_id;
    char task[BUFFER_SIZE];
} TaskInfo;

typedef struct TaskNode {
    TaskInfo task_info;
    struct TaskNode* next;
} TaskNode;


void enqueue_task(TaskInfo task_info);
TaskInfo dequeue_task();