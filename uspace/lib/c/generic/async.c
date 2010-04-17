/*
 * Copyright (c) 2006 Ondrej Palkovsky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup libc
 * @{
 */
/** @file
 */

/**
 * Asynchronous library
 *
 * The aim of this library is to provide a facility for writing programs which
 * utilize the asynchronous nature of HelenOS IPC, yet using a normal way of
 * programming.
 *
 * You should be able to write very simple multithreaded programs, the async
 * framework will automatically take care of most synchronization problems.
 *
 * Default semantics:
 * - async_send_*(): Send asynchronously. If the kernel refuses to send
 *                   more messages, [ try to get responses from kernel, if
 *                   nothing found, might try synchronous ]
 *
 * Example of use (pseudo C):
 *
 * 1) Multithreaded client application
 *
 *   fibril_create(fibril1, ...);
 *   fibril_create(fibril2, ...);
 *   ...
 *
 *   int fibril1(void *arg)
 *   {
 *     conn = ipc_connect_me_to();
 *     c1 = async_send(conn);
 *     c2 = async_send(conn);
 *     async_wait_for(c1);
 *     async_wait_for(c2);
 *     ...
 *   }
 *
 *
 * 2) Multithreaded server application
 *
 *   main()
 *   {
 *     async_manager();
 *   }
 *
 *   my_client_connection(icallid, *icall)
 *   {
 *     if (want_refuse) {
 *       ipc_answer_0(icallid, ELIMIT);
 *       return;
 *     }
 *     ipc_answer_0(icallid, EOK);
 *
 *     callid = async_get_call(&call);
 *     somehow_handle_the_call(callid, call);
 *     ipc_answer_2(callid, 1, 2, 3);
 *
 *     callid = async_get_call(&call);
 *     ...
 *   }
 *
 */

#include <futex.h>
#include <async.h>
#include <async_priv.h>
#include <fibril.h>
#include <stdio.h>
#include <adt/hash_table.h>
#include <adt/list.h>
#include <ipc/ipc.h>
#include <assert.h>
#include <errno.h>
#include <sys/time.h>
#include <arch/barrier.h>
#include <bool.h>

atomic_t async_futex = FUTEX_INITIALIZER;

/** Number of threads waiting for IPC in the kernel. */
atomic_t threads_in_ipc_wait = { 0 };

typedef struct {
	awaiter_t wdata;
	
	/** If reply was received. */
	bool done;
	
	/** Pointer to where the answer data is stored. */
	ipc_call_t *dataptr;
	
	ipcarg_t retval;
} amsg_t;

/**
 * Structures of this type are used to group information about a call and a
 * message queue link.
 */
typedef struct {
	link_t link;
	ipc_callid_t callid;
	ipc_call_t call;
} msg_t;

typedef struct {
	awaiter_t wdata;
	
	/** Hash table link. */
	link_t link;
	
	/** Incoming phone hash. */
	ipcarg_t in_phone_hash;
	
	/** Messages that should be delivered to this fibril. */
	link_t msg_queue;
	
	/** Identification of the opening call. */
	ipc_callid_t callid;
	/** Call data of the opening call. */
	ipc_call_t call;
	
	/** Identification of the closing call. */
	ipc_callid_t close_callid;
	
	/** Fibril function that will be used to handle the connection. */
	void (*cfibril)(ipc_callid_t, ipc_call_t *);
} connection_t;

/** Identifier of the incoming connection handled by the current fibril. */
fibril_local connection_t *FIBRIL_connection;

static void default_client_connection(ipc_callid_t callid, ipc_call_t *call);
static void default_interrupt_received(ipc_callid_t callid, ipc_call_t *call);

/**
 * Pointer to a fibril function that will be used to handle connections.
 */
static async_client_conn_t client_connection = default_client_connection;

/**
 * Pointer to a fibril function that will be used to handle interrupt
 * notifications.
 */
static async_client_conn_t interrupt_received = default_interrupt_received;

static hash_table_t conn_hash_table;
static LIST_INITIALIZE(timeout_list);

#define CONN_HASH_TABLE_CHAINS  32

/** Compute hash into the connection hash table based on the source phone hash.
 *
 * @param key Pointer to source phone hash.
 *
 * @return Index into the connection hash table.
 *
 */
static hash_index_t conn_hash(unsigned long *key)
{
	assert(key);
	return (((*key) >> 4) % CONN_HASH_TABLE_CHAINS);
}

/** Compare hash table item with a key.
 *
 * @param key  Array containing the source phone hash as the only item.
 * @param keys Expected 1 but ignored.
 * @param item Connection hash table item.
 *
 * @return True on match, false otherwise.
 *
 */
static int conn_compare(unsigned long key[], hash_count_t keys, link_t *item)
{
	connection_t *hs = hash_table_get_instance(item, connection_t, link);
	return (key[0] == hs->in_phone_hash);
}

/** Connection hash table removal callback function.
 *
 * This function is called whenever a connection is removed from the connection
 * hash table.
 *
 * @param item Connection hash table item being removed.
 *
 */
static void conn_remove(link_t *item)
{
	free(hash_table_get_instance(item, connection_t, link));
}


/** Operations for the connection hash table. */
static hash_table_operations_t conn_hash_table_ops = {
	.hash = conn_hash,
	.compare = conn_compare,
	.remove_callback = conn_remove
};

/** Sort in current fibril's timeout request.
 *
 * @param wd Wait data of the current fibril.
 *
 */
void async_insert_timeout(awaiter_t *wd)
{
	wd->to_event.occurred = false;
	wd->to_event.inlist = true;
	
	link_t *tmp = timeout_list.next;
	while (tmp != &timeout_list) {
		awaiter_t *cur;
		
		cur = list_get_instance(tmp, awaiter_t, to_event.link);
		if (tv_gteq(&cur->to_event.expires, &wd->to_event.expires))
			break;
		tmp = tmp->next;
	}
	
	list_append(&wd->to_event.link, tmp);
}

/** Try to route a call to an appropriate connection fibril.
 *
 * If the proper connection fibril is found, a message with the call is added to
 * its message queue. If the fibril was not active, it is activated and all
 * timeouts are unregistered.
 *
 * @param callid Hash of the incoming call.
 * @param call   Data of the incoming call.
 *
 * @return False if the call doesn't match any connection.
 *         True if the call was passed to the respective connection fibril.
 *
 */
static bool route_call(ipc_callid_t callid, ipc_call_t *call)
{
	futex_down(&async_futex);
	
	unsigned long key = call->in_phone_hash;
	link_t *hlp = hash_table_find(&conn_hash_table, &key);
	
	if (!hlp) {
		futex_up(&async_futex);
		return false;
	}
	
	connection_t *conn = hash_table_get_instance(hlp, connection_t, link);
	
	msg_t *msg = malloc(sizeof(*msg));
	if (!msg) {
		futex_up(&async_futex);
		return false;
	}
	
	msg->callid = callid;
	msg->call = *call;
	list_append(&msg->link, &conn->msg_queue);
	
	if (IPC_GET_METHOD(*call) == IPC_M_PHONE_HUNGUP)
		conn->close_callid = callid;
	
	/* If the connection fibril is waiting for an event, activate it */
	if (!conn->wdata.active) {
		
		/* If in timeout list, remove it */
		if (conn->wdata.to_event.inlist) {
			conn->wdata.to_event.inlist = false;
			list_remove(&conn->wdata.to_event.link);
		}
		
		conn->wdata.active = true;
		fibril_add_ready(conn->wdata.fid);
	}
	
	futex_up(&async_futex);
	return true;
}

/** Notification fibril.
 *
 * When a notification arrives, a fibril with this implementing function is
 * created. It calls interrupt_received() and does the final cleanup.
 *
 * @param arg Message structure pointer.
 *
 * @return Always zero.
 *
 */
static int notification_fibril(void *arg)
{
	msg_t *msg = (msg_t *) arg;
	interrupt_received(msg->callid, &msg->call);
	
	free(msg);
	return 0;
}

/** Process interrupt notification.
 *
 * A new fibril is created which would process the notification.
 *
 * @param callid Hash of the incoming call.
 * @param call   Data of the incoming call.
 *
 * @return False if an error occured.
 *         True if the call was passed to the notification fibril.
 *
 */
static bool process_notification(ipc_callid_t callid, ipc_call_t *call)
{
	futex_down(&async_futex);
	
	msg_t *msg = malloc(sizeof(*msg));
	if (!msg) {
		futex_up(&async_futex);
		return false;
	}
	
	msg->callid = callid;
	msg->call = *call;
	
	fid_t fid = fibril_create(notification_fibril, msg);
	fibril_add_ready(fid);
	
	futex_up(&async_futex);
	return true;
}

/** Return new incoming message for the current (fibril-local) connection.
 *
 * @param call  Storage where the incoming call data will be stored.
 * @param usecs Timeout in microseconds. Zero denotes no timeout.
 *
 * @return If no timeout was specified, then a hash of the
 *         incoming call is returned. If a timeout is specified,
 *         then a hash of the incoming call is returned unless
 *         the timeout expires prior to receiving a message. In
 *         that case zero is returned.
 *
 */
ipc_callid_t async_get_call_timeout(ipc_call_t *call, suseconds_t usecs)
{
	assert(FIBRIL_connection);
	
	/* Why doing this?
	 * GCC 4.1.0 coughs on FIBRIL_connection-> dereference.
	 * GCC 4.1.1 happilly puts the rdhwr instruction in delay slot.
	 *           I would never expect to find so many errors in
	 *           a compiler.
	 */
	connection_t *conn = FIBRIL_connection;
	
	futex_down(&async_futex);
	
	if (usecs) {
		gettimeofday(&conn->wdata.to_event.expires, NULL);
		tv_add(&conn->wdata.to_event.expires, usecs);
	} else
		conn->wdata.to_event.inlist = false;
	
	/* If nothing in queue, wait until something arrives */
	while (list_empty(&conn->msg_queue)) {
		if (conn->close_callid) {
			/*
			 * Handle the case when the connection was already
			 * closed by the client but the server did not notice
			 * the first IPC_M_PHONE_HUNGUP call and continues to
			 * call async_get_call_timeout(). Repeat
			 * IPC_M_PHONE_HUNGUP until the caller notices. 
			 */
			memset(call, 0, sizeof(ipc_call_t));
			IPC_SET_METHOD(*call, IPC_M_PHONE_HUNGUP);
			futex_up(&async_futex);
			return conn->close_callid;
		}

		if (usecs)
			async_insert_timeout(&conn->wdata);
		
		conn->wdata.active = false;
		
		/*
		 * Note: the current fibril will be rescheduled either due to a
		 * timeout or due to an arriving message destined to it. In the
		 * former case, handle_expired_timeouts() and, in the latter
		 * case, route_call() will perform the wakeup.
		 */
		fibril_switch(FIBRIL_TO_MANAGER);
		
		/*
		 * Futex is up after getting back from async_manager.
		 * Get it again.
		 */
		futex_down(&async_futex);
		if ((usecs) && (conn->wdata.to_event.occurred)
		    && (list_empty(&conn->msg_queue))) {
			/* If we timed out -> exit */
			futex_up(&async_futex);
			return 0;
		}
	}
	
	msg_t *msg = list_get_instance(conn->msg_queue.next, msg_t, link);
	list_remove(&msg->link);
	
	ipc_callid_t callid = msg->callid;
	*call = msg->call;
	free(msg);
	
	futex_up(&async_futex);
	return callid;
}

/** Default fibril function that gets called to handle new connection.
 *
 * This function is defined as a weak symbol - to be redefined in user code.
 *
 * @param callid Hash of the incoming call.
 * @param call   Data of the incoming call.
 *
 */
static void default_client_connection(ipc_callid_t callid, ipc_call_t *call)
{
	ipc_answer_0(callid, ENOENT);
}

/** Default fibril function that gets called to handle interrupt notifications.
 *
 * This function is defined as a weak symbol - to be redefined in user code.
 *
 * @param callid Hash of the incoming call.
 * @param call   Data of the incoming call.
 *
 */
static void default_interrupt_received(ipc_callid_t callid, ipc_call_t *call)
{
}

/** Wrapper for client connection fibril.
 *
 * When a new connection arrives, a fibril with this implementing function is
 * created. It calls client_connection() and does the final cleanup.
 *
 * @param arg Connection structure pointer.
 *
 * @return Always zero.
 *
 */
static int connection_fibril(void *arg)
{
	/*
	 * Setup fibril-local connection pointer and call client_connection().
	 *
	 */
	FIBRIL_connection = (connection_t *) arg;
	FIBRIL_connection->cfibril(FIBRIL_connection->callid,
	    &FIBRIL_connection->call);
	
	/* Remove myself from the connection hash table */
	futex_down(&async_futex);
	unsigned long key = FIBRIL_connection->in_phone_hash;
	hash_table_remove(&conn_hash_table, &key, 1);
	futex_up(&async_futex);
	
	/* Answer all remaining messages with EHANGUP */
	while (!list_empty(&FIBRIL_connection->msg_queue)) {
		msg_t *msg;
		
		msg = list_get_instance(FIBRIL_connection->msg_queue.next,
		    msg_t, link);
		list_remove(&msg->link);
		ipc_answer_0(msg->callid, EHANGUP);
		free(msg);
	}
	
	if (FIBRIL_connection->close_callid)
		ipc_answer_0(FIBRIL_connection->close_callid, EOK);
	
	return 0;
}

/** Create a new fibril for a new connection.
 *
 * Create new fibril for connection, fill in connection structures and inserts
 * it into the hash table, so that later we can easily do routing of messages to
 * particular fibrils.
 *
 * @param in_phone_hash Identification of the incoming connection.
 * @param callid        Hash of the opening IPC_M_CONNECT_ME_TO call.
 *                      If callid is zero, the connection was opened by
 *                      accepting the IPC_M_CONNECT_TO_ME call and this function
 *                      is called directly by the server.
 * @param call          Call data of the opening call.
 * @param cfibril       Fibril function that should be called upon opening the
 *                      connection.
 *
 * @return New fibril id or NULL on failure.
 *
 */
fid_t async_new_connection(ipcarg_t in_phone_hash, ipc_callid_t callid,
    ipc_call_t *call, void (*cfibril)(ipc_callid_t, ipc_call_t *))
{
	connection_t *conn = malloc(sizeof(*conn));
	if (!conn) {
		if (callid)
			ipc_answer_0(callid, ENOMEM);
		return NULL;
	}
	
	conn->in_phone_hash = in_phone_hash;
	list_initialize(&conn->msg_queue);
	conn->callid = callid;
	conn->close_callid = 0;
	
	if (call)
		conn->call = *call;
	
	/* We will activate the fibril ASAP */
	conn->wdata.active = true;
	conn->cfibril = cfibril;
	conn->wdata.fid = fibril_create(connection_fibril, conn);
	
	if (!conn->wdata.fid) {
		free(conn);
		if (callid)
			ipc_answer_0(callid, ENOMEM);
		return NULL;
	}
	
	/* Add connection to the connection hash table */
	unsigned long key = conn->in_phone_hash;
	
	futex_down(&async_futex);
	hash_table_insert(&conn_hash_table, &key, &conn->link);
	futex_up(&async_futex);
	
	fibril_add_ready(conn->wdata.fid);
	
	return conn->wdata.fid;
}

/** Handle a call that was received.
 *
 * If the call has the IPC_M_CONNECT_ME_TO method, a new connection is created.
 * Otherwise the call is routed to its connection fibril.
 *
 * @param callid Hash of the incoming call.
 * @param call   Data of the incoming call.
 *
 */
static void handle_call(ipc_callid_t callid, ipc_call_t *call)
{
	/* Unrouted call - do some default behaviour */
	if ((callid & IPC_CALLID_NOTIFICATION)) {
		process_notification(callid, call);
		goto out;
	}
	
	switch (IPC_GET_METHOD(*call)) {
	case IPC_M_CONNECT_ME:
	case IPC_M_CONNECT_ME_TO:
		/* Open new connection with fibril etc. */
		async_new_connection(IPC_GET_ARG5(*call), callid, call,
		    client_connection);
		goto out;
	}
	
	/* Try to route the call through the connection hash table */
	if (route_call(callid, call))
		goto out;
	
	/* Unknown call from unknown phone - hang it up */
	ipc_answer_0(callid, EHANGUP);
	return;
	
out:
	;
}

/** Fire all timeouts that expired. */
static void handle_expired_timeouts(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	
	futex_down(&async_futex);
	
	link_t *cur = timeout_list.next;
	while (cur != &timeout_list) {
		awaiter_t *waiter;
		
		waiter = list_get_instance(cur, awaiter_t, to_event.link);
		if (tv_gt(&waiter->to_event.expires, &tv))
			break;

		cur = cur->next;

		list_remove(&waiter->to_event.link);
		waiter->to_event.inlist = false;
		waiter->to_event.occurred = true;
		
		/*
		 * Redundant condition?
		 * The fibril should not be active when it gets here.
		 */
		if (!waiter->active) {
			waiter->active = true;
			fibril_add_ready(waiter->fid);
		}
	}
	
	futex_up(&async_futex);
}

/** Endless loop dispatching incoming calls and answers.
 *
 * @return Never returns.
 *
 */
static int async_manager_worker(void)
{
	while (true) {
		if (fibril_switch(FIBRIL_FROM_MANAGER)) {
			futex_up(&async_futex); 
			/*
			 * async_futex is always held when entering a manager
			 * fibril.
			 */
			continue;
		}
		
		futex_down(&async_futex);
		
		suseconds_t timeout;
		if (!list_empty(&timeout_list)) {
			awaiter_t *waiter = list_get_instance(timeout_list.next,
			    awaiter_t, to_event.link);
			
			struct timeval tv;
			gettimeofday(&tv, NULL);
			
			if (tv_gteq(&tv, &waiter->to_event.expires)) {
				futex_up(&async_futex);
				handle_expired_timeouts();
				continue;
			} else
				timeout = tv_sub(&waiter->to_event.expires,
				    &tv);
		} else
			timeout = SYNCH_NO_TIMEOUT;
		
		futex_up(&async_futex);

		atomic_inc(&threads_in_ipc_wait);
		
		ipc_call_t call;
		ipc_callid_t callid = ipc_wait_cycle(&call, timeout,
		    SYNCH_FLAGS_NONE);
		
		atomic_dec(&threads_in_ipc_wait);

		if (!callid) {
			handle_expired_timeouts();
			continue;
		}
		
		if (callid & IPC_CALLID_ANSWERED)
			continue;
		
		handle_call(callid, &call);
	}
	
	return 0;
}

/** Function to start async_manager as a standalone fibril.
 *
 * When more kernel threads are used, one async manager should exist per thread.
 *
 * @param arg Unused.
 * @return Never returns.
 *
 */
static int async_manager_fibril(void *arg)
{
	futex_up(&async_futex);
	
	/*
	 * async_futex is always locked when entering manager
	 */
	async_manager_worker();
	
	return 0;
}

/** Add one manager to manager list. */
void async_create_manager(void)
{
	fid_t fid = fibril_create(async_manager_fibril, NULL);
	fibril_add_manager(fid);
}

/** Remove one manager from manager list */
void async_destroy_manager(void)
{
	fibril_remove_manager();
}

/** Initialize the async framework.
 *
 * @return Zero on success or an error code.
 */
int __async_init(void)
{
	if (!hash_table_create(&conn_hash_table, CONN_HASH_TABLE_CHAINS, 1,
	    &conn_hash_table_ops)) {
		printf("%s: Cannot create async hash table\n", "libc");
		return ENOMEM;
	}
	
	return 0;
}

/** Reply received callback.
 *
 * This function is called whenever a reply for an asynchronous message sent out
 * by the asynchronous framework is received.
 *
 * Notify the fibril which is waiting for this message that it has arrived.
 *
 * @param arg    Pointer to the asynchronous message record.
 * @param retval Value returned in the answer.
 * @param data   Call data of the answer.
 */
static void reply_received(void *arg, int retval, ipc_call_t *data)
{
	futex_down(&async_futex);
	
	amsg_t *msg = (amsg_t *) arg;
	msg->retval = retval;
	
	/* Copy data after futex_down, just in case the call was detached */
	if ((msg->dataptr) && (data))
		*msg->dataptr = *data;
	
	write_barrier();
	
	/* Remove message from timeout list */
	if (msg->wdata.to_event.inlist)
		list_remove(&msg->wdata.to_event.link);
	
	msg->done = true;
	if (!msg->wdata.active) {
		msg->wdata.active = true;
		fibril_add_ready(msg->wdata.fid);
	}
	
	futex_up(&async_futex);
}

/** Send message and return id of the sent message.
 *
 * The return value can be used as input for async_wait() to wait for
 * completion.
 *
 * @param phoneid Handle of the phone that will be used for the send.
 * @param method  Service-defined method.
 * @param arg1    Service-defined payload argument.
 * @param arg2    Service-defined payload argument.
 * @param arg3    Service-defined payload argument.
 * @param arg4    Service-defined payload argument.
 * @param dataptr If non-NULL, storage where the reply data will be
 *                stored.
 *
 * @return Hash of the sent message or 0 on error.
 *
 */
aid_t async_send_fast(int phoneid, ipcarg_t method, ipcarg_t arg1,
    ipcarg_t arg2, ipcarg_t arg3, ipcarg_t arg4, ipc_call_t *dataptr)
{
	amsg_t *msg = malloc(sizeof(*msg));
	
	if (!msg)
		return 0;
	
	msg->done = false;
	msg->dataptr = dataptr;
	
	msg->wdata.to_event.inlist = false;
	/* We may sleep in the next method, but it will use its own mechanism */
	msg->wdata.active = true;
	
	ipc_call_async_4(phoneid, method, arg1, arg2, arg3, arg4, msg,
	    reply_received, true);
	
	return (aid_t) msg;
}

/** Send message and return id of the sent message
 *
 * The return value can be used as input for async_wait() to wait for
 * completion.
 *
 * @param phoneid Handle of the phone that will be used for the send.
 * @param method  Service-defined method.
 * @param arg1    Service-defined payload argument.
 * @param arg2    Service-defined payload argument.
 * @param arg3    Service-defined payload argument.
 * @param arg4    Service-defined payload argument.
 * @param arg5    Service-defined payload argument.
 * @param dataptr If non-NULL, storage where the reply data will be
 *                stored.
 *
 * @return Hash of the sent message or 0 on error.
 *
 */
aid_t async_send_slow(int phoneid, ipcarg_t method, ipcarg_t arg1,
    ipcarg_t arg2, ipcarg_t arg3, ipcarg_t arg4, ipcarg_t arg5,
    ipc_call_t *dataptr)
{
	amsg_t *msg = malloc(sizeof(*msg));
	
	if (!msg)
		return 0;
	
	msg->done = false;
	msg->dataptr = dataptr;
	
	msg->wdata.to_event.inlist = false;
	/* We may sleep in next method, but it will use its own mechanism */
	msg->wdata.active = true;
	
	ipc_call_async_5(phoneid, method, arg1, arg2, arg3, arg4, arg5, msg,
	    reply_received, true);
	
	return (aid_t) msg;
}

/** Wait for a message sent by the async framework.
 *
 * @param amsgid Hash of the message to wait for.
 * @param retval Pointer to storage where the retval of the answer will
 *               be stored.
 *
 */
void async_wait_for(aid_t amsgid, ipcarg_t *retval)
{
	amsg_t *msg = (amsg_t *) amsgid;
	
	futex_down(&async_futex);
	if (msg->done) {
		futex_up(&async_futex);
		goto done;
	}
	
	msg->wdata.fid = fibril_get_id();
	msg->wdata.active = false;
	msg->wdata.to_event.inlist = false;
	
	/* Leave the async_futex locked when entering this function */
	fibril_switch(FIBRIL_TO_MANAGER);
	
	/* Futex is up automatically after fibril_switch */
	
done:
	if (retval)
		*retval = msg->retval;
	
	free(msg);
}

/** Wait for a message sent by the async framework, timeout variant.
 *
 * @param amsgid  Hash of the message to wait for.
 * @param retval  Pointer to storage where the retval of the answer will
 *                be stored.
 * @param timeout Timeout in microseconds.
 *
 * @return Zero on success, ETIMEOUT if the timeout has expired.
 *
 */
int async_wait_timeout(aid_t amsgid, ipcarg_t *retval, suseconds_t timeout)
{
	amsg_t *msg = (amsg_t *) amsgid;
	
	/* TODO: Let it go through the event read at least once */
	if (timeout < 0)
		return ETIMEOUT;
	
	futex_down(&async_futex);
	if (msg->done) {
		futex_up(&async_futex);
		goto done;
	}
	
	gettimeofday(&msg->wdata.to_event.expires, NULL);
	tv_add(&msg->wdata.to_event.expires, timeout);
	
	msg->wdata.fid = fibril_get_id();
	msg->wdata.active = false;
	async_insert_timeout(&msg->wdata);
	
	/* Leave the async_futex locked when entering this function */
	fibril_switch(FIBRIL_TO_MANAGER);
	
	/* Futex is up automatically after fibril_switch */
	
	if (!msg->done)
		return ETIMEOUT;
	
done:
	if (retval)
		*retval = msg->retval;
	
	free(msg);
	
	return 0;
}

/** Wait for specified time.
 *
 * The current fibril is suspended but the thread continues to execute.
 *
 * @param timeout Duration of the wait in microseconds.
 *
 */
void async_usleep(suseconds_t timeout)
{
	amsg_t *msg = malloc(sizeof(*msg));
	
	if (!msg)
		return;
	
	msg->wdata.fid = fibril_get_id();
	msg->wdata.active = false;
	
	gettimeofday(&msg->wdata.to_event.expires, NULL);
	tv_add(&msg->wdata.to_event.expires, timeout);
	
	futex_down(&async_futex);
	
	async_insert_timeout(&msg->wdata);
	
	/* Leave the async_futex locked when entering this function */
	fibril_switch(FIBRIL_TO_MANAGER);
	
	/* Futex is up automatically after fibril_switch() */
	
	free(msg);
}

/** Setter for client_connection function pointer.
 *
 * @param conn Function that will implement a new connection fibril.
 *
 */
void async_set_client_connection(async_client_conn_t conn)
{
	client_connection = conn;
}

/** Setter for interrupt_received function pointer.
 *
 * @param intr Function that will implement a new interrupt
 *             notification fibril.
 */
void async_set_interrupt_received(async_client_conn_t intr)
{
	interrupt_received = intr;
}

/** Pseudo-synchronous message sending - fast version.
 *
 * Send message asynchronously and return only after the reply arrives.
 *
 * This function can only transfer 4 register payload arguments. For
 * transferring more arguments, see the slower async_req_slow().
 *
 * @param phoneid Hash of the phone through which to make the call.
 * @param method  Method of the call.
 * @param arg1    Service-defined payload argument.
 * @param arg2    Service-defined payload argument.
 * @param arg3    Service-defined payload argument.
 * @param arg4    Service-defined payload argument.
 * @param r1      If non-NULL, storage for the 1st reply argument.
 * @param r2      If non-NULL, storage for the 2nd reply argument.
 * @param r3      If non-NULL, storage for the 3rd reply argument.
 * @param r4      If non-NULL, storage for the 4th reply argument.
 * @param r5      If non-NULL, storage for the 5th reply argument.
 *
 * @return Return code of the reply or a negative error code.
 *
 */
ipcarg_t async_req_fast(int phoneid, ipcarg_t method, ipcarg_t arg1,
    ipcarg_t arg2, ipcarg_t arg3, ipcarg_t arg4, ipcarg_t *r1, ipcarg_t *r2,
    ipcarg_t *r3, ipcarg_t *r4, ipcarg_t *r5)
{
	ipc_call_t result;
	aid_t eid = async_send_4(phoneid, method, arg1, arg2, arg3, arg4,
	    &result);
	
	ipcarg_t rc;
	async_wait_for(eid, &rc);
	
	if (r1)
		*r1 = IPC_GET_ARG1(result);
	
	if (r2)
		*r2 = IPC_GET_ARG2(result);
	
	if (r3)
		*r3 = IPC_GET_ARG3(result);
	
	if (r4)
		*r4 = IPC_GET_ARG4(result);
	
	if (r5)
		*r5 = IPC_GET_ARG5(result);
	
	return rc;
}

/** Pseudo-synchronous message sending - slow version.
 *
 * Send message asynchronously and return only after the reply arrives.
 *
 * @param phoneid Hash of the phone through which to make the call.
 * @param method  Method of the call.
 * @param arg1    Service-defined payload argument.
 * @param arg2    Service-defined payload argument.
 * @param arg3    Service-defined payload argument.
 * @param arg4    Service-defined payload argument.
 * @param arg5    Service-defined payload argument.
 * @param r1      If non-NULL, storage for the 1st reply argument.
 * @param r2      If non-NULL, storage for the 2nd reply argument.
 * @param r3      If non-NULL, storage for the 3rd reply argument.
 * @param r4      If non-NULL, storage for the 4th reply argument.
 * @param r5      If non-NULL, storage for the 5th reply argument.
 *
 * @return Return code of the reply or a negative error code.
 *
 */
ipcarg_t async_req_slow(int phoneid, ipcarg_t method, ipcarg_t arg1,
    ipcarg_t arg2, ipcarg_t arg3, ipcarg_t arg4, ipcarg_t arg5, ipcarg_t *r1,
    ipcarg_t *r2, ipcarg_t *r3, ipcarg_t *r4, ipcarg_t *r5)
{
	ipc_call_t result;
	aid_t eid = async_send_5(phoneid, method, arg1, arg2, arg3, arg4, arg5,
	    &result);
	
	ipcarg_t rc;
	async_wait_for(eid, &rc);
	
	if (r1)
		*r1 = IPC_GET_ARG1(result);
	
	if (r2)
		*r2 = IPC_GET_ARG2(result);
	
	if (r3)
		*r3 = IPC_GET_ARG3(result);
	
	if (r4)
		*r4 = IPC_GET_ARG4(result);
	
	if (r5)
		*r5 = IPC_GET_ARG5(result);
	
	return rc;
}

/** Wrapper for making IPC_M_CONNECT_ME_TO calls using the async framework.
 * 
 * Ask through phone for a new connection to some service.
 *
 * @param phoneid	Phone handle used for contacting the other side.
 * @param arg1		User defined argument.
 * @param arg2		User defined argument.
 * @param arg3		User defined argument.
 *
 * @return		New phone handle on success or a negative error code.
 */
int
async_connect_me_to(int phoneid, ipcarg_t arg1, ipcarg_t arg2, ipcarg_t arg3)
{
	int rc;
	ipcarg_t newphid;

	rc = async_req_3_5(phoneid, IPC_M_CONNECT_ME_TO, arg1, arg2, arg3, NULL,
	    NULL, NULL, NULL, &newphid);
	
	if (rc != EOK)	
		return rc;

	return newphid;
}

/** Wrapper for making IPC_M_CONNECT_ME_TO calls using the async framework.
 * 
 * Ask through phone for a new connection to some service and block until
 * success.
 *
 * @param phoneid	Phone handle used for contacting the other side.
 * @param arg1		User defined argument.
 * @param arg2		User defined argument.
 * @param arg3		User defined argument.
 *
 * @return		New phone handle on success or a negative error code.
 */
int
async_connect_me_to_blocking(int phoneid, ipcarg_t arg1, ipcarg_t arg2,
    ipcarg_t arg3)
{
	int rc;
	ipcarg_t newphid;

	rc = async_req_4_5(phoneid, IPC_M_CONNECT_ME_TO, arg1, arg2, arg3,
	    IPC_FLAG_BLOCKING, NULL, NULL, NULL, NULL, &newphid);
	
	if (rc != EOK)	
		return rc;

	return newphid;
}

/** Wrapper for making IPC_M_SHARE_IN calls using the async framework.
 *
 * @param phoneid	Phone that will be used to contact the receiving side.
 * @param dst		Destination address space area base.
 * @param size		Size of the destination address space area.
 * @param arg		User defined argument.
 * @param flags		Storage where the received flags will be stored. Can be
 *			NULL.
 *
 * @return		Zero on success or a negative error code from errno.h.
 */
int async_share_in_start(int phoneid, void *dst, size_t size, ipcarg_t arg,
    int *flags)
{
	int res;
	sysarg_t tmp_flags;
	res = async_req_3_2(phoneid, IPC_M_SHARE_IN, (ipcarg_t) dst,
	    (ipcarg_t) size, arg, NULL, &tmp_flags);
	if (flags)
		*flags = tmp_flags;
	return res;
}

/** Wrapper for receiving the IPC_M_SHARE_IN calls using the async framework.
 *
 * This wrapper only makes it more comfortable to receive IPC_M_SHARE_IN calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * So far, this wrapper is to be used from within a connection fibril.
 *
 * @param callid	Storage where the hash of the IPC_M_SHARE_IN call will
 * 			be stored.
 * @param size		Destination address space area size.	
 *
 * @return		Non-zero on success, zero on failure.
 */
int async_share_in_receive(ipc_callid_t *callid, size_t *size)
{
	ipc_call_t data;
	
	assert(callid);
	assert(size);

	*callid = async_get_call(&data);
	if (IPC_GET_METHOD(data) != IPC_M_SHARE_IN)
		return 0;
	*size = (size_t) IPC_GET_ARG2(data);
	return 1;
}

/** Wrapper for answering the IPC_M_SHARE_IN calls using the async framework.
 *
 * This wrapper only makes it more comfortable to answer IPC_M_DATA_READ calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * @param callid	Hash of the IPC_M_DATA_READ call to answer.
 * @param src		Source address space base.
 * @param flags		Flags to be used for sharing. Bits can be only cleared.
 *
 * @return		Zero on success or a value from @ref errno.h on failure.
 */
int async_share_in_finalize(ipc_callid_t callid, void *src, int flags)
{
	return ipc_share_in_finalize(callid, src, flags);
}

/** Wrapper for making IPC_M_SHARE_OUT calls using the async framework.
 *
 * @param phoneid	Phone that will be used to contact the receiving side.
 * @param src		Source address space area base address.
 * @param flags		Flags to be used for sharing. Bits can be only cleared.
 *
 * @return		Zero on success or a negative error code from errno.h.
 */
int async_share_out_start(int phoneid, void *src, int flags)
{
	return async_req_3_0(phoneid, IPC_M_SHARE_OUT, (ipcarg_t) src, 0,
	    (ipcarg_t) flags);
}

/** Wrapper for receiving the IPC_M_SHARE_OUT calls using the async framework.
 *
 * This wrapper only makes it more comfortable to receive IPC_M_SHARE_OUT calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * So far, this wrapper is to be used from within a connection fibril.
 *
 * @param callid	Storage where the hash of the IPC_M_SHARE_OUT call will
 * 			be stored.
 * @param size		Storage where the source address space area size will be
 *			stored.
 * @param flags		Storage where the sharing flags will be stored.
 *
 * @return		Non-zero on success, zero on failure.
 */
int async_share_out_receive(ipc_callid_t *callid, size_t *size, int *flags)
{
	ipc_call_t data;
	
	assert(callid);
	assert(size);
	assert(flags);

	*callid = async_get_call(&data);
	if (IPC_GET_METHOD(data) != IPC_M_SHARE_OUT)
		return 0;
	*size = (size_t) IPC_GET_ARG2(data);
	*flags = (int) IPC_GET_ARG3(data);
	return 1;
}

/** Wrapper for answering the IPC_M_SHARE_OUT calls using the async framework.
 *
 * This wrapper only makes it more comfortable to answer IPC_M_SHARE_OUT calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * @param callid	Hash of the IPC_M_DATA_WRITE call to answer.
 * @param dst		Destination address space area base address.	
 *
 * @return		Zero on success or a value from @ref errno.h on failure.
 */
int async_share_out_finalize(ipc_callid_t callid, void *dst)
{
	return ipc_share_out_finalize(callid, dst);
}


/** Wrapper for making IPC_M_DATA_READ calls using the async framework.
 *
 * @param phoneid	Phone that will be used to contact the receiving side.
 * @param dst		Address of the beginning of the destination buffer.
 * @param size		Size of the destination buffer.
 *
 * @return		Zero on success or a negative error code from errno.h.
 */
int async_data_read_start(int phoneid, void *dst, size_t size)
{
	return async_req_2_0(phoneid, IPC_M_DATA_READ, (ipcarg_t) dst,
	    (ipcarg_t) size);
}

/** Wrapper for receiving the IPC_M_DATA_READ calls using the async framework.
 *
 * This wrapper only makes it more comfortable to receive IPC_M_DATA_READ calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * So far, this wrapper is to be used from within a connection fibril.
 *
 * @param callid	Storage where the hash of the IPC_M_DATA_READ call will
 * 			be stored.
 * @param size		Storage where the maximum size will be stored. Can be
 *			NULL.
 *
 * @return		Non-zero on success, zero on failure.
 */
int async_data_read_receive(ipc_callid_t *callid, size_t *size)
{
	ipc_call_t data;
	
	assert(callid);

	*callid = async_get_call(&data);
	if (IPC_GET_METHOD(data) != IPC_M_DATA_READ)
		return 0;
	if (size)
		*size = (size_t) IPC_GET_ARG2(data);
	return 1;
}

/** Wrapper for answering the IPC_M_DATA_READ calls using the async framework.
 *
 * This wrapper only makes it more comfortable to answer IPC_M_DATA_READ calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * @param callid	Hash of the IPC_M_DATA_READ call to answer.
 * @param src		Source address for the IPC_M_DATA_READ call.
 * @param size		Size for the IPC_M_DATA_READ call. Can be smaller than
 *			the maximum size announced by the sender.
 *
 * @return		Zero on success or a value from @ref errno.h on failure.
 */
int async_data_read_finalize(ipc_callid_t callid, const void *src, size_t size)
{
	return ipc_data_read_finalize(callid, src, size);
}

/** Wrapper for forwarding any read request
 *
 *
 */
int async_data_read_forward_fast(int phoneid, ipcarg_t method, ipcarg_t arg1,
    ipcarg_t arg2, ipcarg_t arg3, ipcarg_t arg4, ipc_call_t *dataptr)
{
	ipc_callid_t callid;
	if (!async_data_read_receive(&callid, NULL)) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	aid_t msg = async_send_fast(phoneid, method, arg1, arg2, arg3, arg4,
	    dataptr);
	if (msg == 0) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	int retval = ipc_forward_fast(callid, phoneid, 0, 0, 0,
	    IPC_FF_ROUTE_FROM_ME);
	if (retval != EOK) {
		async_wait_for(msg, NULL);
		ipc_answer_0(callid, retval);
		return retval;
	}
	
	ipcarg_t rc;
	async_wait_for(msg, &rc);
	
	return (int) rc;
}

/** Wrapper for making IPC_M_DATA_WRITE calls using the async framework.
 *
 * @param phoneid Phone that will be used to contact the receiving side.
 * @param src     Address of the beginning of the source buffer.
 * @param size    Size of the source buffer.
 *
 * @return Zero on success or a negative error code from errno.h.
 *
 */
int async_data_write_start(int phoneid, const void *src, size_t size)
{
	return async_req_2_0(phoneid, IPC_M_DATA_WRITE, (ipcarg_t) src,
	    (ipcarg_t) size);
}

/** Wrapper for receiving the IPC_M_DATA_WRITE calls using the async framework.
 *
 * This wrapper only makes it more comfortable to receive IPC_M_DATA_WRITE calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * So far, this wrapper is to be used from within a connection fibril.
 *
 * @param callid Storage where the hash of the IPC_M_DATA_WRITE call will
 *               be stored.
 * @param size   Storage where the suggested size will be stored. May be
 *               NULL
 *
 * @return Non-zero on success, zero on failure.
 *
 */
int async_data_write_receive(ipc_callid_t *callid, size_t *size)
{
	ipc_call_t data;
	
	assert(callid);
	
	*callid = async_get_call(&data);
	if (IPC_GET_METHOD(data) != IPC_M_DATA_WRITE)
		return 0;
	
	if (size)
		*size = (size_t) IPC_GET_ARG2(data);
	
	return 1;
}

/** Wrapper for answering the IPC_M_DATA_WRITE calls using the async framework.
 *
 * This wrapper only makes it more comfortable to answer IPC_M_DATA_WRITE calls
 * so that the user doesn't have to remember the meaning of each IPC argument.
 *
 * @param callid Hash of the IPC_M_DATA_WRITE call to answer.
 * @param dst    Final destination address for the IPC_M_DATA_WRITE call.
 * @param size   Final size for the IPC_M_DATA_WRITE call.
 *
 * @return Zero on success or a value from @ref errno.h on failure.
 *
 */
int async_data_write_finalize(ipc_callid_t callid, void *dst, size_t size)
{
	return ipc_data_write_finalize(callid, dst, size);
}

/** Wrapper for receiving binary data or strings
 *
 * This wrapper only makes it more comfortable to use async_data_write_*
 * functions to receive binary data or strings.
 *
 * @param data       Pointer to data pointer (which should be later disposed
 *                   by free()). If the operation fails, the pointer is not
 *                   touched.
 * @param nullterm   If true then the received data is always zero terminated.
 *                   This also causes to allocate one extra byte beyond the
 *                   raw transmitted data.
 * @param min_size   Minimum size (in bytes) of the data to receive.
 * @param max_size   Maximum size (in bytes) of the data to receive. 0 means
 *                   no limit.
 * @param granulariy If non-zero then the size of the received data has to
 *                   be divisible by this value.
 * @param received   If not NULL, the size of the received data is stored here.
 *
 * @return Zero on success or a value from @ref errno.h on failure.
 *
 */
int async_data_write_accept(void **data, const bool nullterm,
    const size_t min_size, const size_t max_size, const size_t granularity,
    size_t *received)
{
	ipc_callid_t callid;
	size_t size;
	if (!async_data_write_receive(&callid, &size)) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	if (size < min_size) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	if ((max_size > 0) && (size > max_size)) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	if ((granularity > 0) && ((size % granularity) != 0)) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	void *_data;
	
	if (nullterm)
		_data = malloc(size + 1);
	else
		_data = malloc(size);
	
	if (_data == NULL) {
		ipc_answer_0(callid, ENOMEM);
		return ENOMEM;
	}
	
	int rc = async_data_write_finalize(callid, _data, size);
	if (rc != EOK) {
		free(_data);
		return rc;
	}
	
	if (nullterm)
		((char *) _data)[size] = 0;
	
	*data = _data;
	if (received != NULL)
		*received = size;
	
	return EOK;
}

/** Wrapper for voiding any data that is about to be received
 *
 * This wrapper can be used to void any pending data
 *
 * @param retval Error value from @ref errno.h to be returned to the caller.
 *
 */
void async_data_write_void(const int retval)
{
	ipc_callid_t callid;
	async_data_write_receive(&callid, NULL);
	ipc_answer_0(callid, retval);
}

/** Wrapper for forwarding any data that is about to be received
 *
 *
 */
int async_data_write_forward_fast(int phoneid, ipcarg_t method, ipcarg_t arg1,
    ipcarg_t arg2, ipcarg_t arg3, ipcarg_t arg4, ipc_call_t *dataptr)
{
	ipc_callid_t callid;
	if (!async_data_write_receive(&callid, NULL)) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	aid_t msg = async_send_fast(phoneid, method, arg1, arg2, arg3, arg4,
	    dataptr);
	if (msg == 0) {
		ipc_answer_0(callid, EINVAL);
		return EINVAL;
	}
	
	int retval = ipc_forward_fast(callid, phoneid, 0, 0, 0,
	    IPC_FF_ROUTE_FROM_ME);
	if (retval != EOK) {
		async_wait_for(msg, NULL);
		ipc_answer_0(callid, retval);
		return retval;
	}
	
	ipcarg_t rc;
	async_wait_for(msg, &rc);
	
	return (int) rc;
}

/** @}
 */