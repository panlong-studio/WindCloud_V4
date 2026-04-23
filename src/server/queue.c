#include <stdlib.h>
#include "queue.h"

/**
 * @brief  把一个客户端 fd 放进任务队列尾部
 * @param  pQueue 任务队列
 * @param  fd 要入队的客户端 fd
 * @return 成功返回 0
 */
int enQueue(queue_t* pQueue,int fd){
    // 先创建一个新结点。
    node_t* pNew=(node_t*)calloc(1,sizeof(node_t));

    // 把客户端 fd 存进新结点。
    pNew->fd=fd;

    // 如果当前队列为空，那么新结点既是头，也是尾。
    if(pQueue->size==0){
        pQueue->head=pNew;
        pQueue->end=pNew;
    }else{
        // 如果队列不为空，就把新结点接到旧尾巴后面。
        pQueue->end->pNext=pNew;

        // 再更新尾指针。
        pQueue->end=pNew;
    }

    // 队列长度加 1。
    pQueue->size++;
    return 0;
}

/**
 * @brief  从任务队列头部取出一个客户端 fd
 * @param  pQueue 任务队列
 * @return 成功返回客户端 fd，队列为空返回 -1
 */
int deQueue(queue_t* pQueue){
    // 空队列没有元素可取。
    if(pQueue->size==0){
        return -1;
    }

    // p 指向当前队头结点。
    node_t*p=pQueue->head;

    // 先把 fd 保存下来，后面返回给调用者。
    int fd = p->fd;

    // 队头后移到下一个结点。
    pQueue->head=p->pNext;

    // 如果原来只有 1 个元素，那么取走后尾指针也要清空。
    if(pQueue->size==1){
        pQueue->end=NULL;
    }

    // 队列长度减 1。
    pQueue->size--;

    // 释放旧队头结点。
    free(p);

    // 返回取出的客户端 fd。
    return fd;
}
