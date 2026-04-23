#ifndef _QUEUE_H_
#define _QUEUE_H_

typedef struct node_s{
    int fd;
    struct node_s*pNext;
}node_t;

typedef struct queue_s{
    node_t* head;
    node_t* end;
    int size;
}queue_t;

int enQueue(queue_t*pQueue,int fd);
int deQueue(queue_t*pQueue);

#endif