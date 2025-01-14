#ifndef INDEXER_QUEUE_H
#define INDEXER_QUEUE_H

#include "indexer.h"

typedef void indexer_queue_callback_t(int status, void *context);

enum indexer_request_type {
	/* index messages in the mailbox */
	INDEXER_REQUEST_TYPE_INDEX,
	/* optimize the mailbox */
	INDEXER_REQUEST_TYPE_OPTIMIZE,
};

struct indexer_request {
	/* Linked list of all requests - highest priority first */
	struct indexer_request *prev, *next;
	/* Linked list of the same username's requests */
	struct indexer_request *user_prev, *user_next;

	char *username;
	char *mailbox;
	char *session_id;
	unsigned int max_recent_msgs;

	enum indexer_request_type type;

	/* currently indexing this mailbox */
	bool working:1;
	/* after indexing is finished, add this request back to the queue and
	   reindex it (i.e. a new indexing request came while we were
	   working.) */
	bool reindex_head:1;
	bool reindex_tail:1;

	/* when working finished, call this number of contexts and leave the
	   rest to the reindexing. */
	unsigned int working_context_idx;

	ARRAY(void *) contexts;
};

struct indexer_queue *indexer_queue_init(indexer_queue_callback_t *callback);
void indexer_queue_deinit(struct indexer_queue **queue);

/* The callback is called whenever a new request is added to the queue. */
void indexer_queue_set_listen_callback(struct indexer_queue *queue,
				       void (*callback)(struct indexer_queue *));
	
void indexer_queue_append(struct indexer_queue *queue, bool append,
			  const char *username, const char *mailbox,
			  const char *session_id, unsigned int max_recent_msgs,
			  void *context);
void indexer_queue_append_optimize(struct indexer_queue *queue,
				   const char *username, const char *mailbox,
				   void *context);
/* Remove all queued requests for the user. If mailbox_mask is non-NULL, remove
   only requests that match the mailbox mask (with * and ? wildcards). Already
   running requests aren't removed, but their reindex flag is cleared. */
void indexer_queue_cancel(struct indexer_queue *queue,
			  const char *username, const char *mailbox_mask);
void indexer_queue_cancel_all(struct indexer_queue *queue);

bool indexer_queue_is_empty(struct indexer_queue *queue);
unsigned int indexer_queue_count(struct indexer_queue *queue);

/* Return the next request from the queue, without removing it. */
struct indexer_request *indexer_queue_request_peek(struct indexer_queue *queue);
/* Remove the next request from the queue. You must call
   indexer_queue_request_finish() to free its memory. */
void indexer_queue_request_remove(struct indexer_queue *queue);
/* Give a status update about how far the indexing is going on. */
void indexer_queue_request_status(struct indexer_queue *queue,
				  struct indexer_request *request,
				  int percentage);
/* Move the next request to the end of the queue. */
void indexer_queue_move_head_to_tail(struct indexer_queue *queue);
/* Start working on a request */
void indexer_queue_request_work(struct indexer_request *request);
/* Finish the request and free its memory. */
void indexer_queue_request_finish(struct indexer_queue *queue,
				  struct indexer_request **request,
				  bool success);

/* Iterate through all requests. First it returns the requests currently being
   worked on, followed by the queued requests in the priority order. If
   only_working=TRUE, return only the requests currently being worked on. */
struct indexer_queue_iter *
indexer_queue_iter_init(struct indexer_queue *queue, bool only_working);
struct indexer_request *indexer_queue_iter_next(struct indexer_queue_iter *iter);
void indexer_queue_iter_deinit(struct indexer_queue_iter **iter);

#endif
