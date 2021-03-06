#ifndef PRIORITYQUEUE_H_
#define PRIORITYQUEUE_H_
#include <base/system.h>
//#if defined(CONF_FAMILY_UNIX)
#ifdef __cplusplus
extern "C"{
#endif

typedef struct item {
	double priority;
	int value;
} item;

typedef struct queue {
	int size;
	unsigned int allocated;
	item *root;
	int *index;
	unsigned int indexAllocated;
} queue;

void insert (queue *q, int value, double priority);
void deleteMin (queue *q);
item *findMin (const queue *q);
void changePriority (queue *q, int ind, double newPriority);
void deleteq (queue *q, int ind);
int priorityOf (const queue *q, int ind);
int exists (const queue *q, int ind);
queue *createQueue ();
void freeQueue (queue *q);

#ifdef __cplusplus
}
#endif
#endif
//#endif