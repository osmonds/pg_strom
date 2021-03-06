/*
 * mqueue.c
 *
 * Routines for inter-process communication via message queues
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include <limits.h>
#include <sys/time.h>
#include "pg_strom.h"

static pthread_mutexattr_t	mutex_attr;
static pthread_rwlockattr_t	rwlock_attr;
static pthread_condattr_t	cond_attr;
static int		pgstrom_mqueue_timeout;

/* variables related to shared memory segment */
static shmem_startup_hook_type shmem_startup_hook_next;
static struct {
	shmem_context  *shmcontext;		/* shred memory context for mqueue */
	pgstrom_queue  *serv_mqueue;	/* queue for OpenCL background server */
} *mqueue_shm_values;

/*
 * pgstrom_create_queue
 *
 * It creates a message queue of PG-Strom. Here is two types of message
 * queues; one is for OpenCL background server, the other is for backend
 * process to be used to receive response messages.
 * A message queue for the background server is controled with reference-
 * counter. It shall be incremented when backend enqueues a message to
 * OpenCL server, then decremented when backend dequeued a message, the
 * server tried to enqueue a message to "closed" queue, or the message
 * queue is closed.
 * It is needed for error handling in the backend side, because PostgreSQL
 * adopts exception-catch style error handling, it may lose the messages
 * being executed asynchronously.
 */
pgstrom_queue *
pgstrom_create_queue(void)
{
	shmem_context  *context = mqueue_shm_values->shmcontext;
	pgstrom_queue  *queue;

	Assert(context != NULL);
	queue = pgstrom_shmem_alloc(context, sizeof(pgstrom_queue));
	if (!queue)
		return NULL;

	if (pthread_mutex_init(&queue->lock, &mutex_attr) != 0)
		goto error;
	if (pthread_cond_init(&queue->cond, &cond_attr) != 0)
		goto error;
	dlist_init(&queue->qhead);
	queue->refcnt = 1;
	queue->closed = false;

	return queue;

error:
	pgstrom_shmem_free(queue);
	return NULL;
}

/*
 * pgstrom_enqueue
 *
 * It allows the backend to enqueue a message towards OpenCL background
 * server.
 */
bool
pgstrom_enqueue_message(pgstrom_message *message)
{
	pgstrom_queue *mqueue = mqueue_shm_values->serv_mqueue;
	int		rc;

	pthread_mutex_lock(&mqueue->lock);
	if (mqueue->closed)
	{
		pthread_mutex_unlock(&mqueue->lock);
		return false;
	}
	/* enqueue this message */
	dlist_push_tail(&mqueue->qhead, &message->chain);
	rc = pthread_cond_signal(&mqueue->cond);
	Assert(rc == 0);
	pthread_mutex_unlock(&mqueue->lock);

	return true;
}

/*
 * pgstrom_reply_message
 *
 * It enqueues a response message by the OpenCL background server.
 * It shouldn't be called by backend processes.
 */
void
pgstrom_reply_message(pgstrom_message *message)
{
	pgstrom_queue  *respq = message->respq;
	bool	queue_free = false;
	bool	message_free = false;
	int		rc;

	pthread_mutex_lock(&respq->lock);
	if (respq->closed)
	{
		/*
		 * If reply queue is already closed, it means nobody waits for
		 * the reply-message enqueued. So, server side decrements the
		 * reference count of the message queue instead of the backend,
		 * and release the message queue if no longer used.
		 */
		if (--respq->refcnt == 0)
			queue_free = true;
		message_free = true;
	}
	else
	{
		/* reply this message */
		Assert(respq->refcnt > 0);
		dlist_push_tail(&respq->qhead, &message->chain);
		rc = pthread_cond_signal(&respq->cond);
		Assert(rc == 0);
	}
	pthread_mutex_unlock(&respq->lock);

	/*
	 * Release message and response queue, if the backend already aborted
	 * and nobody can receive the response messages.
	 */
	if (message_free)
	{
		if (message->cb_release)
			message->cb_release(message);
		pgstrom_shmem_free(message);
	}

	if (queue_free)
	{
		pthread_cond_destroy(&respq->cond);
		pthread_mutex_destroy(&respq->lock);
		pgstrom_shmem_free(respq);
	}
}

/*
 * pgstrom_dequeue_message
 *
 * It fetches a message from the message queue. If empty, it waits for new
 * messages will come, or returns NULL if it exceeds timeout or it got
 * a signal being pending.
 */
pgstrom_message *
pgstrom_dequeue_message(pgstrom_queue *queue)
{
#define POOLING_INTERVAL	200000000	/* 0.2msec */
	pgstrom_message *result = NULL;
	struct timeval	basetv;
	struct timespec	timeout;
	ulong	timeleft = ((ulong)pgstrom_mqueue_timeout) * 1000000UL;
	bool	queue_release = false;
	int		rc;

	rc = gettimeofday(&basetv, NULL);
	Assert(rc == 0);
	timeout.tv_sec = basetv.tv_sec;
	timeout.tv_nsec = basetv.tv_usec * 1000;

	for (;;)
	{
		pthread_mutex_lock(&queue->lock);
		/* dequeue a message from the message queue */
		if (!dlist_is_empty(&queue->qhead))
		{
			dlist_node *dnode
				= dlist_pop_head_node(&queue->qhead);

			result = dlist_container(pgstrom_message, chain, dnode);
			if (--queue->refcnt == 0)
				queue_release = true;
			pthread_mutex_unlock(&queue->lock);
			break;
		}
		else if (timeleft == 0)
		{
			pthread_mutex_unlock(&queue->lock);
			break;
		}
		else
		{
			/* setting up the next timeout */
			if (timeleft > POOLING_INTERVAL)
			{
				timeout.tv_nsec += POOLING_INTERVAL;
				timeleft -= POOLING_INTERVAL;
			}
			else
			{
				timeout.tv_nsec += timeleft;
				timeleft = 0;
			}
			rc = pthread_cond_timedwait(&queue->cond, &queue->lock, &timeout);
			Assert(rc == 0 || rc == ETIMEDOUT);

			/* signal will break waiting loop */
			if (InterruptPending)
				timeleft = 0;
		}
	}

	/*
	 * If this queue (except for server mqueue) is already closed and
	 * no message will come further, the queue shall be dropped.
	 */
	if (queue_release && queue != mqueue_shm_values->serv_mqueue)
	{
		Assert(queue->closed);
		pthread_cond_destroy(&queue->cond);
		pthread_mutex_destroy(&queue->lock);
		pgstrom_shmem_free(queue);
	}
	return result;
}

/*
 * pgstrom_try_dequeue_message
 *
 * It is almost equivalent to pgstrom_dequeue_message(), however, it never
 * wait for new messages, will return immediately.
 */
pgstrom_message *
pgstrom_try_dequeue_message(pgstrom_queue *queue)
{
	pgstrom_message *result = NULL;
	bool	queue_release = false;

	pthread_mutex_lock(&queue->lock);
	if (!dlist_is_empty(&queue->qhead))
	{
		dlist_node *dnode
			= dlist_pop_head_node(&queue->qhead);

		result = dlist_container(pgstrom_message, chain, dnode);
		if (--queue->refcnt == 0)
			queue_release = true;
	}
	pthread_mutex_unlock(&queue->lock);

	/*
	 * If this queue (except for server mqueue) is already closed and
	 * no message will come further, the queue shall be dropped.
	 */
	if (queue_release && queue != mqueue_shm_values->serv_mqueue)
	{
		Assert(queue->closed);
		pthread_cond_destroy(&queue->cond);
		pthread_mutex_destroy(&queue->lock);
		pgstrom_shmem_free(queue);
	}
	return result;
}

/*
 * pgstrom_dequeue_server_message
 *
 * dequeue a message from the server message queue
 */
pgstrom_message *
pgstrom_dequeue_server_message(void)
{
	return pgstrom_dequeue_message(mqueue_shm_values->serv_mqueue);
}

/*
 * pgstrom_close_queue
 *
 * It closes this message queue. Once a message queue got closed, it does not
 * accept any new messages and the queue will be dropped when last message
 * is dequeued or last expected message is tried to enqueue.
 */
void
pgstrom_close_queue(pgstrom_queue *queue)
{
	bool	queue_release = false;

	pthread_mutex_lock(&queue->lock);
	Assert(!queue->closed);
	queue->closed = true;

	if (--queue->refcnt == 0)
		queue_release = true;
	pthread_mutex_unlock(&queue->lock);

	if (queue_release && queue != mqueue_shm_values->serv_mqueue)
	{
		pthread_cond_destroy(&queue->cond);
		pthread_mutex_destroy(&queue->lock);
		pgstrom_shmem_free(queue);
	}
}

/*
 * pgstrom_init_message
 *
 * It initializes the supplied message header, and increments the reference
 * counter of response message queue. It has to be incremented to avoid
 * unexpected destruction.
 */
void
pgstrom_init_message(pgstrom_message *msg,
					 MessageTag mtag,
					 pgstrom_queue *respq,
					 void (*cb_release)(pgstrom_message *msg))
{
	msg->mtag = mtag;
	if (respq)
	{
		pthread_mutex_lock(&respq->lock);
		Assert(respq->refcnt > 0);
		respq->refcnt++;
		pthread_mutex_unlock(&respq->lock);
	}
	msg->respq = respq;
	msg->cb_release = cb_release;
}

/*
 * pgstrom_setup_mqueue
 *
 * final initialization after the shared memory context got available
 */
void
pgstrom_setup_mqueue(void)
{
	shmem_context  *context;
	pgstrom_queue  *mqueue;

	context = pgstrom_shmem_context_create("PG-Strom Message Queue");
	if (!context)
		elog(ERROR, "failed to create shared memory context");
	mqueue_shm_values->shmcontext = context;

	mqueue = pgstrom_create_queue();
	if (!mqueue)
		elog(ERROR, "failed to create PG-Strom server message queue");
	mqueue_shm_values->serv_mqueue = mqueue;

	elog(LOG, "PG-Strom: Message Queue (context=%p, server mqueue=%p)",
		 context, mqueue);
}

/*
 * pgstrom_startup_mqueue
 *
 * allocation of shared memory for message queue
 */
static void
pgstrom_startup_mqueue(void)
{
	bool	found;

	if (shmem_startup_hook_next)
		(*shmem_startup_hook_next)();

	mqueue_shm_values = ShmemInitStruct("mqueue_shm_values",
										MAXALIGN(sizeof(*mqueue_shm_values)),
										&found);
	Assert(!found);
	memset(mqueue_shm_values, 0, sizeof(*mqueue_shm_values));
}

/*
 * pgstrom_init_mqueue
 *
 * initialization at library loading
 */
void
pgstrom_init_mqueue(void)
{
	int		rc;

	/* timeout configuration of the message queue feature */
	DefineCustomIntVariable("pgstrom.mqueue_timeout",
							"timeout of PG-Strom's message queue in msec",
							NULL,
							&pgstrom_mqueue_timeout,
							60 * 1000,	/* 60 sec */
							1,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	/* initialization of mutex_attr */
	rc = pthread_mutexattr_init(&mutex_attr);
	if (rc != 0)
		elog(ERROR, "failed on pthread_mutexattr_init: %s",
			 strerror(rc));
	rc = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
	if (rc != 0)
		elog(ERROR, "failed on pthread_mutexattr_setpshared: %s",
			 strerror(rc));

	/* initialization of rwlock_attr */
	rc = pthread_rwlockattr_init(&rwlock_attr);
	if (rc != 0)
		elog(ERROR, "failed on pthread_rwlockattr_init: %s",
			 strerror(rc));
	rc = pthread_rwlockattr_setpshared(&rwlock_attr, PTHREAD_PROCESS_SHARED);
	if (rc != 0)
		elog(ERROR, "failed on pthread_rwlockattr_setpshared: %s",
			 strerror(rc));

	/* initialization of cond_attr */
	rc = pthread_condattr_init(&cond_attr);
	if (rc != 0)
		elog(ERROR, "failed on pthread_condattr_init: %s",
			 strerror(rc));
	rc = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
	if (rc != 0)
		elog(ERROR, "failed on pthread_condattr_setpshared: %s",
			 strerror(rc));

	/* aquires shared memory region */
	RequestAddinShmemSpace(MAXALIGN(sizeof(*mqueue_shm_values)));
	shmem_startup_hook_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_mqueue;
}
