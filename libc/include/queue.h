#ifndef _QUEUE_H
#define _QUEUE_H
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
struct qnode;
struct queue;
typedef struct qnode qnode_t;
typedef struct queue queue_t;

struct qnode
{
    void *value;
    struct qnode *prev;
    struct qnode *next;
};

#define QUEUE_TRACE 1

struct queue
{
    size_t count;
    qnode_t *head;
    qnode_t *tail;
    int flags;
};

#define queue_for(n, q) for (qnode_t *(n) = (q)->head; (n); (n) = (n)->next)

#define QUEUE_NEW() &(queue_t){0}

static inline queue_t *queue_new()
{
    queue_t *queue;
    queue = (queue_t *)malloc(sizeof(queue_t));
    if (!queue) return NULL;

    memset(queue, 0, sizeof(queue_t));
    return queue;
}

static inline qnode_t *enqueue(queue_t *queue, void *value)
{
    if (!queue)
        return NULL;

    int trace = queue->flags & QUEUE_TRACE;
    if (trace) printf("qtrace: enqueue(%p, %p)\n", queue, value);

    qnode_t *node;
    node = (qnode_t *)malloc(sizeof(qnode_t));
    if (!node)
        return NULL;

    memset(node, 0, sizeof(qnode_t));

    if (trace) printf("qtrace: allocted node %p\n", node);

    node->value = value;

    if (!queue->count)
    {
        queue->head = queue->tail = node;
    } else
    {
        node->prev = queue->tail;
        queue->tail->next = node;
        queue->tail = node;
    }

    ++queue->count;
    return node;
}

static inline void *dequeue(queue_t *queue)
{
    if (!queue || !queue->count)
        return NULL;

    --queue->count;
    qnode_t *head = queue->head;

    queue->head = head->next;

    if (queue->head)
        queue->head->prev = NULL;

    if (head == queue->tail)
        queue->tail = NULL;

    void *value = head->value;
    free(head);

    return value;
}

static inline void queue_remove(queue_t *queue, void *value)
{
    if (!queue || !queue->count)
        return;

    queue_for (node, queue) {
        if (node->value == value) {
            if (!node->prev) {    /* Head */
                dequeue(queue);
            } else if (!node->next) {   /* Tail */
                --queue->count;
                queue->tail = queue->tail->prev;
                queue->tail->next = NULL;
                free(node);
            } else {
                --queue->count;
                node->prev->next = node->next;
                node->next->prev = node->prev;
                free(node);
            }

            break;
        }
    }
}

static inline void queue_node_remove(queue_t *queue, qnode_t *node)
{
    if (!queue || !queue->count || !node)
        return;

    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;
    if (queue->head == node)
        queue->head = node->next;
    if (queue->tail == node)
        queue->tail = node->prev;

    --queue->count;
    free(node);
    return;
}

#endif