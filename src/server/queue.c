#include <stdlib.h>
#include <string.h>
#include "queue.h"

/**
 * @brief  把一个传输任务放进任务队列尾部
 * @param  pQueue 任务队列
 * @param  task 要入队的传输任务
 * @return 成功返回 0，失败返回 -1
 */
int enQueue(queue_t* pQueue,const transfer_task_t *task){
    /* 第一步：为新任务分配队列结点。 */
    node_t* pNew=(node_t*)calloc(1,sizeof(node_t));
    if(pNew==NULL || task==NULL){
        free(pNew);
        return -1;
    }

    /* 第二步：把任务内容复制到新结点中。 */
    memcpy(&pNew->task, task, sizeof(transfer_task_t));

    /* 第三步：根据队列当前状态，调整头尾指针。 */
    if(pQueue->size==0){
        pQueue->head=pNew;
        pQueue->end=pNew;
    }else{
        pQueue->end->pNext=pNew;
        pQueue->end=pNew;
    }

    /* 第四步：更新队列长度。 */
    pQueue->size++;
    return 0;
}

/**
 * @brief  从任务队列头部取出一个传输任务
 * @param  pQueue 任务队列
 * @param  task 输出参数，用来保存出队任务
 * @return 成功返回 0，队列为空或参数非法返回 -1
 */
int deQueue(queue_t* pQueue,transfer_task_t *task){
    /* 第一步：先检查参数和队列状态。 */
    if(pQueue==NULL || pQueue->size==0 || task==NULL){
        return -1;
    }

    /* 第二步：记录当前队头结点地址。 */
    node_t*p=pQueue->head;

    /* 第三步：把队头任务复制给调用方。 */
    memcpy(task, &p->task, sizeof(transfer_task_t));

    /* 第四步：队头后移，准备删除旧结点。 */
    pQueue->head=p->pNext;

    /* 如果原队列只有一个元素，出队后尾指针也要清空。 */
    if(pQueue->size==1){
        pQueue->end=NULL;
    }

    /* 第五步：更新队列长度。 */
    pQueue->size--;

    /* 第六步：释放旧队头结点。 */
    free(p);

    return 0;
}
