/* Copyright (C) 2004 Timo Sirainen */

/*
   Modifying mbox can be slow, so we try to do it all at once minimizing the
   required disk I/O. We may need to:

   - Update message flags in Status, X-Status and X-Keywords headers
   - Write missing X-UID and X-IMAPbase headers
   - Write missing or broken Content-Length header if there's space
   - Expunge specified messages

   Here's how we do it:

   - Start reading the mails from the beginning
   - X-Keywords, X-UID and X-IMAPbase headers may contain padding at the end
     of them, remember how much each message has and offset to beginning of the
     padding
   - If header needs to be rewritten and there's enough space, do it
       - If we didn't have enough space, remember how much was missing
   - Continue reading and counting the padding in each message. If available
     padding is enough to rewrite all the previous messages needing it, do it
   - When we encounter expunged message, treat all of it as padding and
     rewrite previous messages if needed (and there's enough space).
     Afterwards keep moving messages backwards to fill the expunged space.
     Moving is done by rewriting each message's headers, with possibly adding
     missing Content-Length header and padding. Message bodies are moved
     without modifications.
   - If we encounter end of file, grow the file and rewrite needed messages
   - Rewriting is done by moving message body forward, rewriting message's
     header and doing the same for previous message, until all of them are
     rewritten.
*/

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "buffer.h"
#include "hostpid.h"
#include "istream.h"
#include "file-set-size.h"
#include "str.h"
#include "read-full.h"
#include "write-full.h"
#include "message-date.h"
#include "istream-raw-mbox.h"
#include "mbox-storage.h"
#include "mbox-from.h"
#include "mbox-file.h"
#include "mbox-lock.h"
#include "mbox-sync-private.h"

#include <stddef.h>
#include <stdlib.h>
#include <utime.h>
#include <sys/stat.h>

/* The text below was taken exactly as c-client wrote it to my mailbox,
   so it's probably copyrighted by University of Washington. */
#define PSEUDO_MESSAGE_BODY \
"This text is part of the internal format of your mail folder, and is not\n" \
"a real message.  It is created automatically by the mail system software.\n" \
"If deleted, important folder data will be lost, and it will be re-created\n" \
"with the data reset to initial values.\n"

int mbox_sync_seek(struct mbox_sync_context *sync_ctx, uoff_t from_offset)
{
	if (istream_raw_mbox_seek(sync_ctx->input, from_offset) < 0) {
		mail_storage_set_critical(STORAGE(sync_ctx->mbox->storage),
			"Unexpectedly lost From-line at offset %"PRIuUOFF_T
			" from mbox file %s", from_offset,
			sync_ctx->mbox->path);
		return -1;
	}
	return 0;
}

static void mbox_sync_array_delete_to(ARRAY_TYPE(sync_recs) *syncs_arr,
				      uint32_t last_uid)
{
	struct mail_index_sync_rec *syncs;
	unsigned int src, dest, count;

	syncs = array_get_modifiable(syncs_arr, &count);

	for (src = dest = 0; src < count; src++) {
		i_assert(last_uid >= syncs[src].uid1);
		if (last_uid <= syncs[src].uid2) {
			/* keep it */
			if (src != dest)
				syncs[dest] = syncs[src];
			dest++;
		}
	}

	array_delete(syncs_arr, dest, count - dest);
}

static int
mbox_sync_read_next_mail(struct mbox_sync_context *sync_ctx,
			 struct mbox_sync_mail_context *mail_ctx)
{
	/* get EOF */
	(void)istream_raw_mbox_get_header_offset(sync_ctx->input);
	if (istream_raw_mbox_is_eof(sync_ctx->input))
		return 0;

	p_clear(sync_ctx->mail_keyword_pool);
	memset(mail_ctx, 0, sizeof(*mail_ctx));
	mail_ctx->sync_ctx = sync_ctx;
	mail_ctx->seq = ++sync_ctx->seq;
	mail_ctx->header = sync_ctx->header;

	mail_ctx->mail.from_offset =
		istream_raw_mbox_get_start_offset(sync_ctx->input);
	mail_ctx->mail.offset =
		istream_raw_mbox_get_header_offset(sync_ctx->input);

	mbox_sync_parse_next_mail(sync_ctx->input, mail_ctx);
	i_assert(sync_ctx->input->v_offset != mail_ctx->mail.from_offset ||
		 sync_ctx->input->eof);

	mail_ctx->mail.body_size =
		istream_raw_mbox_get_body_size(sync_ctx->input,
					       mail_ctx->content_length);
	i_assert(mail_ctx->mail.body_size < OFF_T_MAX);

	if ((mail_ctx->mail.flags & MAIL_RECENT) != 0 && !mail_ctx->pseudo) {
		if (!sync_ctx->mbox->ibox.keep_recent) {
			/* need to add 'O' flag to Status-header */
			mail_ctx->need_rewrite = TRUE;
		}
		mail_ctx->recent = TRUE;
	}
	return 1;
}

static bool mbox_sync_buf_have_expunges(ARRAY_TYPE(sync_recs) *syncs_arr)
{
	const struct mail_index_sync_rec *syncs;
	unsigned int i, count;

	syncs = array_get(syncs_arr, &count);
	for (i = 0; i < count; i++) {
		if (syncs[i].type == MAIL_INDEX_SYNC_TYPE_EXPUNGE)
			return TRUE;
	}
	return FALSE;
}

static int mbox_sync_read_index_syncs(struct mbox_sync_context *sync_ctx,
				      uint32_t uid, bool *sync_expunge_r)
{
	struct mail_index_sync_rec *sync_rec = &sync_ctx->sync_rec;
	int ret;

	*sync_expunge_r = FALSE;

	if (sync_ctx->index_sync_ctx == NULL)
		return 0;

	if (uid == 0) {
		/* nothing for this or the future ones */
		uid = (uint32_t)-1;
	}

	mbox_sync_array_delete_to(&sync_ctx->syncs, uid);
	while (uid >= sync_rec->uid1) {
		if (uid <= sync_rec->uid2 &&
		    sync_rec->type != MAIL_INDEX_SYNC_TYPE_APPEND &&
		    (sync_rec->type != MAIL_INDEX_SYNC_TYPE_EXPUNGE ||
		     !sync_ctx->mbox->mbox_readonly)) {
			array_append(&sync_ctx->syncs, sync_rec, 1);

			if (sync_rec->type == MAIL_INDEX_SYNC_TYPE_EXPUNGE)
				*sync_expunge_r = TRUE;
		}

		ret = mail_index_sync_next(sync_ctx->index_sync_ctx, sync_rec);
		if (ret < 0) {
			mail_storage_set_index_error(&sync_ctx->mbox->ibox);
			return -1;
		}

		if (ret == 0) {
			memset(sync_rec, 0, sizeof(*sync_rec));
			break;
		}

		switch (sync_rec->type) {
		case MAIL_INDEX_SYNC_TYPE_APPEND:
			if (sync_rec->uid2 >= sync_ctx->next_uid)
				sync_ctx->next_uid = sync_rec->uid2 + 1;
			memset(sync_rec, 0, sizeof(*sync_rec));
			break;
		case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
			break;
		case MAIL_INDEX_SYNC_TYPE_FLAGS:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET:
			if (sync_ctx->delay_writes) {
				/* we're not going to write these yet */
				uint32_t seq1, seq2;

				if (mail_index_lookup_uid_range(
						sync_ctx->sync_view,
						sync_rec->uid1, sync_rec->uid2,
						&seq1, &seq2) < 0) {
					return -1;
				}

				if (seq1 > 0) {
					mail_index_update_flags_range(
						sync_ctx->t,
						seq1, seq2, MODIFY_ADD,
						MAIL_INDEX_MAIL_FLAG_DIRTY);
				}

				memset(sync_rec, 0, sizeof(*sync_rec));
			}
			break;
		}
	}

	if (!*sync_expunge_r)
		*sync_expunge_r = mbox_sync_buf_have_expunges(&sync_ctx->syncs);

	return 0;
}

void mbox_sync_apply_index_syncs(struct mbox_sync_context *sync_ctx,
				 struct mbox_sync_mail *mail,
				 bool *keywords_changed_r)
{
	const struct mail_index_sync_rec *syncs;
	unsigned int i, count;

	*keywords_changed_r = FALSE;

	syncs = array_get(&sync_ctx->syncs, &count);
	for (i = 0; i < count; i++) {
		switch (syncs[i].type) {
		case MAIL_INDEX_SYNC_TYPE_FLAGS:
			mail_index_sync_flags_apply(&syncs[i], &mail->flags);
			break;
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET:
			if (!array_is_created(&mail->keywords)) {
				/* no existing keywords */
				if (syncs[i].type !=
				    MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD)
					break;

				/* adding, create the array */
				p_array_init(&mail->keywords,
					     sync_ctx->mail_keyword_pool,
					     I_MIN(10, count - i));
			}
			if (mail_index_sync_keywords_apply(&syncs[i],
							   &mail->keywords))
				*keywords_changed_r = TRUE;
			break;
		default:
			break;
		}
	}
}

static int
mbox_sync_read_index_rec(struct mbox_sync_context *sync_ctx,
			 uint32_t uid, const struct mail_index_record **rec_r)
{
        const struct mail_index_record *rec = NULL;
	uint32_t messages_count;
	int ret = 0;

	messages_count =
		mail_index_view_get_messages_count(sync_ctx->sync_view);
	while (sync_ctx->idx_seq <= messages_count) {
		ret = mail_index_lookup(sync_ctx->sync_view,
					sync_ctx->idx_seq, &rec);
		if (ret < 0) {
			mail_storage_set_index_error(&sync_ctx->mbox->ibox);
			return -1;
		}
		i_assert(ret != 0); /* we should be looking at head index */

		if (uid <= rec->uid)
			break;

		/* externally expunged message, remove from index */
		mail_index_expunge(sync_ctx->t, sync_ctx->idx_seq);
                sync_ctx->idx_seq++;
		rec = NULL;
	}

	if (rec == NULL && uid < sync_ctx->idx_next_uid) {
		/* this UID was already in index and it was expunged */
		mail_storage_set_critical(STORAGE(sync_ctx->mbox->storage),
			"mbox sync: Expunged message reappeared in mailbox %s "
			"(UID %u < %u, seq=%u, idx_msgs=%u)",
			sync_ctx->mbox->path, uid, sync_ctx->idx_next_uid,
			sync_ctx->seq, messages_count);
		ret = 0; rec = NULL;
	} else if (rec != NULL && rec->uid != uid) {
		/* new UID in the middle of the mailbox - shouldn't happen */
		mail_storage_set_critical(STORAGE(sync_ctx->mbox->storage),
			"mbox sync: UID inserted in the middle of mailbox %s "
			"(%u > %u, seq=%u, idx_msgs=%u)", sync_ctx->mbox->path,
			rec->uid, uid, sync_ctx->seq, messages_count);
		ret = 0; rec = NULL;
	} else {
		ret = 1;
	}

	*rec_r = rec;
	return ret;
}

static int mbox_sync_find_index_md5(struct mbox_sync_context *sync_ctx,
				    unsigned char hdr_md5_sum[],
				    const struct mail_index_record **rec_r)
{
        const struct mail_index_record *rec = NULL;
	uint32_t messages_count;
	const void *data;
	int ret;

	messages_count =
		mail_index_view_get_messages_count(sync_ctx->sync_view);
	while (sync_ctx->idx_seq <= messages_count) {
		ret = mail_index_lookup(sync_ctx->sync_view,
					sync_ctx->idx_seq, &rec);
		if (ret < 0) {
			mail_storage_set_index_error(&sync_ctx->mbox->ibox);
			return -1;
		}

		if (mail_index_lookup_ext(sync_ctx->sync_view,
					  sync_ctx->idx_seq,
					  sync_ctx->mbox->ibox.md5hdr_ext_idx,
					  &data) < 0) {
			mail_storage_set_index_error(&sync_ctx->mbox->ibox);
			return -1;
		}

		if (data != NULL && memcmp(data, hdr_md5_sum, 16) == 0)
			break;

		/* externally expunged message, remove from index */
		mail_index_expunge(sync_ctx->t, sync_ctx->idx_seq);
                sync_ctx->idx_seq++;
		rec = NULL;
	}

	*rec_r = rec;
	return 0;
}

static int
mbox_sync_update_from_offset(struct mbox_sync_context *sync_ctx,
                             struct mbox_sync_mail *mail,
			     bool nocheck)
{
	const void *data;
	uint64_t offset;

	if (!nocheck) {
		/* see if from_offset needs updating */
		if (mail_index_lookup_ext(sync_ctx->sync_view,
					  sync_ctx->idx_seq,
					  sync_ctx->mbox->mbox_ext_idx,
					  &data) < 0) {
			mail_storage_set_index_error(&sync_ctx->mbox->ibox);
			return -1;
		}

		if (data != NULL &&
		    *((const uint64_t *)data) == mail->from_offset)
			return 0;
	}

	offset = mail->from_offset;
	mail_index_update_ext(sync_ctx->t, sync_ctx->idx_seq,
			      sync_ctx->mbox->mbox_ext_idx, &offset, NULL);
	return 0;
}

static void
mbox_sync_update_index_keywords(struct mbox_sync_mail_context *mail_ctx)
{
        struct mbox_sync_context *sync_ctx = mail_ctx->sync_ctx;
	struct mail_keywords *keywords;

	keywords = !array_is_created(&mail_ctx->mail.keywords) ?
		mail_index_keywords_create(sync_ctx->t, NULL) :
		mail_index_keywords_create_from_indexes(sync_ctx->t,
						&mail_ctx->mail.keywords);
	mail_index_update_keywords(sync_ctx->t, sync_ctx->idx_seq,
				   MODIFY_REPLACE, keywords);
	mail_index_keywords_free(&keywords);
}

static int
mbox_sync_update_md5_if_changed(struct mbox_sync_mail_context *mail_ctx)
{
        struct mbox_sync_context *sync_ctx = mail_ctx->sync_ctx;
	const void *ext_data;

	if (mail_index_lookup_ext(sync_ctx->sync_view, sync_ctx->idx_seq,
				  sync_ctx->mbox->ibox.md5hdr_ext_idx,
				  &ext_data) < 0) {
		mail_storage_set_index_error(&sync_ctx->mbox->ibox);
		return -1;
	}

	if (ext_data == NULL ||
	    memcmp(mail_ctx->hdr_md5_sum, ext_data, 16) != 0) {
		mail_index_update_ext(sync_ctx->t, sync_ctx->idx_seq,
				      sync_ctx->mbox->ibox.md5hdr_ext_idx,
				      mail_ctx->hdr_md5_sum, NULL);
	}
	return 0;
}

static int mbox_sync_update_index(struct mbox_sync_mail_context *mail_ctx,
				  const struct mail_index_record *rec)
{
        struct mbox_sync_context *sync_ctx = mail_ctx->sync_ctx;
	struct mbox_sync_mail *mail = &mail_ctx->mail;
	uint8_t mbox_flags;

	mbox_flags = mail->flags & MAIL_FLAGS_MASK;

	if (mail_ctx->dirty)
		mbox_flags |= MAIL_INDEX_MAIL_FLAG_DIRTY;
	else if (!sync_ctx->delay_writes)
		mbox_flags &= ~MAIL_INDEX_MAIL_FLAG_DIRTY;

	if (rec == NULL) {
		/* new message */
		mail_index_append(sync_ctx->t, mail->uid, &sync_ctx->idx_seq);
		mail_index_update_flags(sync_ctx->t, sync_ctx->idx_seq,
					MODIFY_REPLACE, mbox_flags);
		mbox_sync_update_index_keywords(mail_ctx);

		if (sync_ctx->mbox->mbox_save_md5 != 0) {
			mail_index_update_ext(sync_ctx->t, sync_ctx->idx_seq,
				sync_ctx->mbox->ibox.md5hdr_ext_idx,
				mail_ctx->hdr_md5_sum, NULL);
		}
	} else {
		/* see if we need to update flags in index file. the flags in
		   sync records are automatically applied to rec->flags at the
		   end of index syncing, so calculate those new flags first */
		struct mbox_sync_mail idx_mail;
		bool keywords_changed;

		memset(&idx_mail, 0, sizeof(idx_mail));
		idx_mail.flags = rec->flags;

		/* get old keywords */
		t_push();
		t_array_init(&idx_mail.keywords, 32);
		if (mail_index_lookup_keywords(sync_ctx->sync_view,
					       sync_ctx->idx_seq,
					       &idx_mail.keywords) < 0) {
			mail_storage_set_index_error(&sync_ctx->mbox->ibox);
			t_pop();
			return -1;
		}
		mbox_sync_apply_index_syncs(sync_ctx, &idx_mail,
					    &keywords_changed);

#define SYNC_FLAGS (MAIL_RECENT | MAIL_INDEX_MAIL_FLAG_DIRTY)
		if ((idx_mail.flags & MAIL_INDEX_MAIL_FLAG_DIRTY) != 0) {
			/* flags are dirty. ignore whatever was in the mbox,
			   but update recent/dirty flag states if needed. */
			mbox_flags &= SYNC_FLAGS;
			mbox_flags |= idx_mail.flags & ~SYNC_FLAGS;
			if (sync_ctx->delay_writes)
				mbox_flags |= MAIL_INDEX_MAIL_FLAG_DIRTY;
		} else {
			/* keep index's internal flags */
			mbox_flags &= MAIL_FLAGS_MASK | SYNC_FLAGS;
			mbox_flags |= idx_mail.flags &
				~(MAIL_FLAGS_MASK | SYNC_FLAGS);
		}

		if ((idx_mail.flags & ~SYNC_FLAGS) !=
		    (mbox_flags & ~SYNC_FLAGS)) {
			/* flags other than recent/dirty have changed */
			mail_index_update_flags(sync_ctx->t, sync_ctx->idx_seq,
						MODIFY_REPLACE, mbox_flags);
		} else {
			if (((idx_mail.flags ^ mbox_flags) &
			     MAIL_RECENT) != 0) {
				/* drop recent flag (it can only be dropped) */
				mail_index_update_flags(sync_ctx->t,
					sync_ctx->idx_seq,
					MODIFY_REMOVE, MAIL_RECENT);
			}
			if (((idx_mail.flags ^ mbox_flags) &
			     MAIL_INDEX_MAIL_FLAG_DIRTY) != 0) {
				/* dirty flag state changed */
				bool dirty = (mbox_flags &
					      MAIL_INDEX_MAIL_FLAG_DIRTY) != 0;
				mail_index_update_flags(sync_ctx->t,
					sync_ctx->idx_seq,
					dirty ? MODIFY_ADD : MODIFY_REMOVE,
					MAIL_INDEX_MAIL_FLAG_DIRTY);
			}
		}

		if ((idx_mail.flags & MAIL_INDEX_MAIL_FLAG_DIRTY) == 0 &&
		    !index_keyword_array_cmp(&idx_mail.keywords,
					     &mail_ctx->mail.keywords))
			mbox_sync_update_index_keywords(mail_ctx);
		t_pop();

		/* see if we need to update md5 sum. */
		if (sync_ctx->mbox->mbox_save_md5 != 0) {
			if (mbox_sync_update_md5_if_changed(mail_ctx) < 0)
				return -1;
		}
	}

	if (mail_ctx->recent &&
	    (rec == NULL || (rec->flags & MAIL_INDEX_MAIL_FLAG_DIRTY) == 0 ||
	     (rec->flags & MAIL_RECENT) != 0)) {
		index_mailbox_set_recent(&sync_ctx->mbox->ibox,
					 sync_ctx->idx_seq);
	}

	/* update from_offsets, but not if we're going to rewrite this message.
	   rewriting would just move it anyway. */
	if (sync_ctx->need_space_seq == 0) {
		bool nocheck = rec == NULL || sync_ctx->expunged_space > 0;
		if (mbox_sync_update_from_offset(sync_ctx, mail, nocheck) < 0)
			return -1;
	}
	return 0;
}

static int mbox_read_from_line(struct mbox_sync_mail_context *ctx)
{
	struct istream *input = ctx->sync_ctx->file_input;
	const unsigned char *data;
	size_t size, from_line_size;

	buffer_set_used_size(ctx->sync_ctx->from_line, 0);
	from_line_size = ctx->hdr_offset - ctx->mail.from_offset;

	i_stream_seek(input, ctx->mail.from_offset);
	for (;;) {
		data = i_stream_get_data(input, &size);
		if (size >= from_line_size)
			size = from_line_size;

		buffer_append(ctx->sync_ctx->from_line, data, size);
		i_stream_skip(input, size);
		from_line_size -= size;

		if (from_line_size == 0)
			break;

		if (i_stream_read(input) < 0)
			return -1;
	}

	return 0;
}

static int mbox_rewrite_base_uid_last(struct mbox_sync_context *sync_ctx)
{
	unsigned char buf[10];
	const char *str;
	uint32_t uid_last;
	unsigned int i;
	int ret;

	i_assert(sync_ctx->base_uid_last_offset != 0);

	/* first check that the 10 bytes are there and they're exactly as
	   expected. just an extra safety check to make sure we never write
	   to wrong location in the mbox file. */
	ret = pread_full(sync_ctx->write_fd, buf, sizeof(buf),
			 sync_ctx->base_uid_last_offset);
	if (ret < 0) {
		mbox_set_syscall_error(sync_ctx->mbox, "pread_full()");
		return -1;
	}
	if (ret == 0) {
		mail_storage_set_critical(STORAGE(sync_ctx->mbox->storage),
			"X-IMAPbase uid-last unexpectedly points outside "
			"mbox file %s", sync_ctx->mbox->path);
		return -1;
	}

	for (i = 0, uid_last = 0; i < sizeof(buf); i++) {
		if (buf[i] < '0' || buf[i] > '9') {
			uid_last = (uint32_t)-1;
			break;
		}
		uid_last = uid_last * 10 + (buf[i] - '0');
	}

	if (uid_last != sync_ctx->base_uid_last) {
		mail_storage_set_critical(STORAGE(sync_ctx->mbox->storage),
			"X-IMAPbase uid-last unexpectedly lost in mbox file %s",
			sync_ctx->mbox->path);
		return -1;
	}

	/* and write it */
	str = t_strdup_printf("%010u", sync_ctx->next_uid - 1);
	if (pwrite_full(sync_ctx->write_fd, str, 10,
			sync_ctx->base_uid_last_offset) < 0) {
		mbox_set_syscall_error(sync_ctx->mbox, "pwrite_full()");
		return -1;
	}

	sync_ctx->base_uid_last = sync_ctx->next_uid - 1;
	return 0;
}

static int
mbox_write_from_line(struct mbox_sync_mail_context *ctx)
{
	string_t *str = ctx->sync_ctx->from_line;

	if (pwrite_full(ctx->sync_ctx->write_fd, str_data(str), str_len(str),
			ctx->mail.from_offset) < 0) {
		mbox_set_syscall_error(ctx->sync_ctx->mbox, "pwrite_full()");
		return -1;
	}

	i_stream_sync(ctx->sync_ctx->input);
	return 0;
}

static void update_from_offsets(struct mbox_sync_context *sync_ctx)
{
	const struct mbox_sync_mail *mails;
	unsigned int i, count;
	uint32_t ext_idx;
	uint64_t offset;

	ext_idx = sync_ctx->mbox->mbox_ext_idx;

	mails = array_get(&sync_ctx->mails, &count);
	for (i = 0; i < count; i++) {
		if (mails[i].idx_seq == 0 ||
		    (mails[i].flags & MBOX_EXPUNGED) != 0)
			continue;

		sync_ctx->moved_offsets = TRUE;
		offset = mails[i].from_offset;
		mail_index_update_ext(sync_ctx->t, mails[i].idx_seq,
				      ext_idx, &offset, NULL);
	}
}

static void mbox_sync_handle_expunge(struct mbox_sync_mail_context *mail_ctx)
{
	struct mbox_sync_context *sync_ctx = mail_ctx->sync_ctx;

	mail_ctx->mail.flags = MBOX_EXPUNGED;
	mail_ctx->mail.offset = mail_ctx->mail.from_offset;
	mail_ctx->mail.space =
		mail_ctx->body_offset - mail_ctx->mail.from_offset +
		mail_ctx->mail.body_size;
	mail_ctx->mail.body_size = 0;

	if (sync_ctx->seq == 1) {
		/* expunging first message, fix space to contain next
		   message's \n header too since it will be removed. */
		mail_ctx->mail.space++;
		if (istream_raw_mbox_has_crlf_ending(sync_ctx->input)) {
			mail_ctx->mail.space++;
			sync_ctx->first_mail_crlf_expunged = TRUE;
		}

		/* uid-last offset is invalid now */
                sync_ctx->base_uid_last_offset = 0;
	}

	sync_ctx->expunged_space += mail_ctx->mail.space;
}

static int mbox_sync_handle_header(struct mbox_sync_mail_context *mail_ctx)
{
	struct mbox_sync_context *sync_ctx = mail_ctx->sync_ctx;
	uoff_t orig_from_offset;
	off_t move_diff;
	int ret;

	if (sync_ctx->expunged_space > 0 && sync_ctx->need_space_seq == 0) {
		/* move the header backwards to fill expunged space */
		move_diff = -sync_ctx->expunged_space;

		orig_from_offset = mail_ctx->mail.from_offset;
		if (sync_ctx->dest_first_mail) {
			/* we're moving this mail to beginning of file.
			   skip the initial \n (it's already counted in
			   expunged_space) */
			mail_ctx->mail.from_offset++;
			if (sync_ctx->first_mail_crlf_expunged)
				mail_ctx->mail.from_offset++;
		}

		/* read the From-line before rewriting overwrites it */
		if (mbox_read_from_line(mail_ctx) < 0)
			return -1;

		mbox_sync_update_header(mail_ctx);
		ret = mbox_sync_try_rewrite(mail_ctx, move_diff);
		if (ret < 0)
			return -1;

		if (ret > 0) {
			/* rewrite successful, write From-line to
			   new location */
			i_assert(move_diff > 0 ||
				 (off_t)mail_ctx->mail.from_offset >=
				 -move_diff);
			mail_ctx->mail.from_offset += move_diff;
			mail_ctx->mail.offset += move_diff;
			if (mbox_write_from_line(mail_ctx) < 0)
				return -1;
		} else {
			if (sync_ctx->dest_first_mail) {
				/* didn't have enough space, move the offset
				   back so seeking into it doesn't fail */
				mail_ctx->mail.from_offset = orig_from_offset;
			}
		}
	} else if (mail_ctx->need_rewrite ||
		   array_count(&sync_ctx->syncs) != 0) {
		mbox_sync_update_header(mail_ctx);
		if (sync_ctx->delay_writes) {
			/* mark it dirty and do it later */
			mail_ctx->dirty = TRUE;
			return 0;
		}

		if ((ret = mbox_sync_try_rewrite(mail_ctx, 0)) < 0)
			return -1;
	} else {
		/* nothing to do */
		return 0;
	}

	if (ret == 0 && sync_ctx->need_space_seq == 0) {
		/* first mail with no space to write it */
		sync_ctx->need_space_seq = sync_ctx->seq;
		sync_ctx->space_diff = 0;

		if (sync_ctx->expunged_space > 0) {
			/* create dummy message to describe the expunged data */
			struct mbox_sync_mail mail;

			memset(&mail, 0, sizeof(mail));
			mail.flags = MBOX_EXPUNGED;
			mail.offset = mail.from_offset =
				(sync_ctx->dest_first_mail ? 1 : 0) +
				mail_ctx->mail.from_offset -
				sync_ctx->expunged_space;
			mail.space = sync_ctx->expunged_space;

                        sync_ctx->space_diff = sync_ctx->expunged_space;
			sync_ctx->expunged_space = 0;
			i_assert(sync_ctx->space_diff < -mail_ctx->mail.space);

			sync_ctx->need_space_seq--;
			array_append(&sync_ctx->mails, &mail, 1);
		}
	}
	return 0;
}

static int
mbox_sync_handle_missing_space(struct mbox_sync_mail_context *mail_ctx)
{
	struct mbox_sync_context *sync_ctx = mail_ctx->sync_ctx;
	uoff_t end_offset, move_diff, extra_space, needed_space;
	uint32_t last_seq;
	ARRAY_TYPE(keyword_indexes) keywords_copy;

	i_assert(mail_ctx->mail.uid == 0 || mail_ctx->mail.space > 0 ||
		 mail_ctx->mail.offset == mail_ctx->hdr_offset);

	if (array_is_created(&mail_ctx->mail.keywords)) {
		/* mail's keywords are allocated from a pool that's cleared
		   for each mail. we'll need to copy it to something more
		   permanent. */
		p_array_init(&keywords_copy, sync_ctx->saved_keywords_pool,
			     array_count(&mail_ctx->mail.keywords));
		array_append_array(&keywords_copy, &mail_ctx->mail.keywords);
		mail_ctx->mail.keywords = keywords_copy;
	}
	array_append(&sync_ctx->mails, &mail_ctx->mail, 1);

	sync_ctx->space_diff += mail_ctx->mail.space;
	if (sync_ctx->space_diff < 0) {
		if (sync_ctx->expunged_space > 0) {
			i_assert(sync_ctx->expunged_space ==
				 mail_ctx->mail.space);
                        sync_ctx->expunged_space = 0;
		}
		return 0;
	}

	/* we have enough space now */
	if (mail_ctx->mail.uid == 0) {
		/* this message was expunged. fill more or less of the space.
		   space_diff now consists of a negative "bytes needed" sum,
		   plus the expunged space of this message. so it contains how
		   many bytes of _extra_ space we have. */
		i_assert(mail_ctx->mail.space >= sync_ctx->space_diff);
		extra_space = MBOX_HEADER_PADDING *
			(sync_ctx->seq - sync_ctx->need_space_seq + 1);
		needed_space = mail_ctx->mail.space - sync_ctx->space_diff;
		if ((uoff_t)sync_ctx->space_diff > needed_space + extra_space) {
			/* don't waste too much on padding */
			move_diff = needed_space + extra_space;
			sync_ctx->expunged_space =
				mail_ctx->mail.space - move_diff;
		} else {
			move_diff = mail_ctx->mail.space;
			extra_space = sync_ctx->space_diff;
			sync_ctx->expunged_space = 0;
		}
		last_seq = sync_ctx->seq - 1;
		array_delete(&sync_ctx->mails,
			     array_count(&sync_ctx->mails) - 1, 1);
		end_offset = mail_ctx->mail.from_offset;
	} else {
		/* this message gave enough space from headers. rewriting stops
		   at the end of this message's headers. */
		sync_ctx->expunged_space = 0;
		last_seq = sync_ctx->seq;
		end_offset = mail_ctx->body_offset;

		move_diff = 0;
		extra_space = sync_ctx->space_diff;
	}

	if (mbox_sync_rewrite(sync_ctx,
			      last_seq == sync_ctx->seq ? mail_ctx : NULL,
			      end_offset, move_diff, extra_space,
			      sync_ctx->need_space_seq, last_seq) < 0)
		return -1;

	update_from_offsets(sync_ctx);

	/* mail_ctx may contain wrong data after rewrite, so make sure we
	   don't try to access it */
	memset(mail_ctx, 0, sizeof(*mail_ctx));

	sync_ctx->need_space_seq = 0;
	sync_ctx->space_diff = 0;
	array_clear(&sync_ctx->mails);
	p_clear(sync_ctx->saved_keywords_pool);
	return 0;
}

static int
mbox_sync_seek_to_seq(struct mbox_sync_context *sync_ctx, uint32_t seq)
{
	struct mbox_mailbox *mbox = sync_ctx->mbox;
	uoff_t old_offset;
	uint32_t uid;
	int ret;
        bool deleted;

	if (seq == 0) {
		if (istream_raw_mbox_seek(mbox->mbox_stream, 0) < 0) {
			mail_storage_set_error(STORAGE(mbox->storage),
				"Mailbox isn't a valid mbox file");
			return -1;
		}
		seq++;
	} else {
		old_offset = istream_raw_mbox_get_start_offset(sync_ctx->input);

		ret = mbox_file_seek(mbox, sync_ctx->sync_view, seq, &deleted);
		if (ret < 0)
			return -1;

		if (ret == 0) {
			if (istream_raw_mbox_seek(mbox->mbox_stream,
						  old_offset) < 0) {
				mail_storage_set_critical(
					STORAGE(mbox->storage),
					"Error seeking back to original "
					"offset %s in mbox file %s",
					dec2str(old_offset), mbox->path);
				return -1;
			}
			return 0;
		}
	}

	if (seq <= 1)
		uid = 0;
	else if (mail_index_lookup_uid(sync_ctx->sync_view, seq-1, &uid) < 0) {
		mail_storage_set_index_error(&mbox->ibox);
		return -1;
	}

	sync_ctx->prev_msg_uid = uid;

        /* set to -1, since it's always increased later */
	sync_ctx->seq = seq-1;
	if (sync_ctx->seq == 0 &&
	    istream_raw_mbox_get_start_offset(sync_ctx->input) != 0) {
		/* this mbox has pseudo mail which contains the X-IMAP header */
		sync_ctx->seq++;
	}

        sync_ctx->idx_seq = seq;
	sync_ctx->dest_first_mail = sync_ctx->seq == 0;
        (void)istream_raw_mbox_get_body_offset(sync_ctx->input);
	return 1;
}

static int
mbox_sync_seek_to_uid(struct mbox_sync_context *sync_ctx, uint32_t uid)
{
	struct mail_index_view *sync_view = sync_ctx->sync_view;
	uint32_t seq1, seq2;
	const struct stat *st;

	if (mail_index_lookup_uid_range(sync_view, uid, (uint32_t)-1,
					&seq1, &seq2) < 0) {
		mail_storage_set_index_error(&sync_ctx->mbox->ibox);
		return -1;
	}

	if (seq1 == 0) {
		/* doesn't exist anymore, seek to end of file */
		st = i_stream_stat(sync_ctx->file_input, TRUE);
		if (st == NULL) {
			mbox_set_syscall_error(sync_ctx->mbox,
					       "i_stream_stat()");
			return -1;
		}

		if (istream_raw_mbox_seek(sync_ctx->mbox->mbox_stream,
					  st->st_size) < 0) {
			mail_storage_set_critical(
				STORAGE(sync_ctx->mbox->storage),
				"Error seeking to end of mbox file %s",
				sync_ctx->mbox->path);
			return -1;
		}
		sync_ctx->idx_seq =
			mail_index_view_get_messages_count(sync_view) + 1;
		return 1;
	}

	return mbox_sync_seek_to_seq(sync_ctx, seq1);
}

static int mbox_sync_partial_seek_next(struct mbox_sync_context *sync_ctx,
				       uint32_t next_uid, bool *partial,
				       bool *skipped_mails)
{
	uint32_t messages_count;
	int ret;

	/* delete sync records up to next message. so if there's still
	   something left in array, it means the next message needs modifying */
	mbox_sync_array_delete_to(&sync_ctx->syncs, next_uid);
	if (array_count(&sync_ctx->syncs) > 0)
		return 1;

	if (sync_ctx->sync_rec.uid1 != 0) {
		/* we can skip forward to next record which needs updating. */
		if (sync_ctx->sync_rec.uid1 != next_uid) {
			*skipped_mails = TRUE;
			next_uid = sync_ctx->sync_rec.uid1;
		}
		ret = mbox_sync_seek_to_uid(sync_ctx, next_uid);
	} else {
		/* if there's no sync records left, we can stop. except if
		   this is a dirty sync, check if there are new messages. */
		if (!sync_ctx->mbox->mbox_sync_dirty)
			return 0;

		messages_count =
			mail_index_view_get_messages_count(sync_ctx->sync_view);
		if (sync_ctx->seq + 1 != messages_count) {
			ret = mbox_sync_seek_to_seq(sync_ctx, messages_count);
			*skipped_mails = TRUE;
		} else {
			ret = 1;
		}
		*partial = FALSE;
	}

	if (ret == 0) {
		/* seek failed because the offset is dirty. just ignore and
		   continue from where we are now. */
		*partial = FALSE;
		ret = 1;
	}
	return ret;
}

static int mbox_sync_loop(struct mbox_sync_context *sync_ctx,
                          struct mbox_sync_mail_context *mail_ctx,
			  bool partial)
{
	const struct mail_index_record *rec;
	uint32_t uid, messages_count;
	uoff_t offset;
	int ret;
	bool expunged, skipped_mails, uids_broken;

	messages_count =
		mail_index_view_get_messages_count(sync_ctx->sync_view);

	/* always start from first message so we can read X-IMAP or
	   X-IMAPbase header */
	ret = mbox_sync_seek_to_seq(sync_ctx, 0);
	if (ret <= 0)
		return ret;

	if (sync_ctx->renumber_uids) {
		/* expunge everything */
		while (sync_ctx->idx_seq <= messages_count) {
			mail_index_expunge(sync_ctx->t,
					   sync_ctx->idx_seq++);
		}
	}

	skipped_mails = uids_broken = FALSE;
	while ((ret = mbox_sync_read_next_mail(sync_ctx, mail_ctx)) > 0) {
		uid = mail_ctx->mail.uid;

		if (mail_ctx->seq == 1 && sync_ctx->base_uid_validity != 0 &&
		    sync_ctx->hdr->uid_validity != 0 &&
		    sync_ctx->base_uid_validity !=
		    sync_ctx->hdr->uid_validity) {
			mail_storage_set_critical(
				STORAGE(sync_ctx->mbox->storage),
				"UIDVALIDITY changed (%u -> %u) "
				"in mbox file %s",
				sync_ctx->hdr->uid_validity,
				sync_ctx->base_uid_validity,
				sync_ctx->mbox->path);

			mail_index_mark_corrupted(sync_ctx->mbox->ibox.index);
			return -1;
		}

		if (mail_ctx->mail.uid_broken && partial) {
			/* UID ordering problems, resync everything to make
			   sure we get everything right */
			if (sync_ctx->mbox->mbox_sync_dirty)
				return 0;

			mail_storage_set_critical(
				STORAGE(sync_ctx->mbox->storage),
				"UIDs broken with partial sync in mbox file %s",
				sync_ctx->mbox->path);

			sync_ctx->mbox->mbox_sync_dirty = TRUE;
			return 0;
		}
		if (mail_ctx->mail.uid_broken)
			uids_broken = TRUE;

		if (mail_ctx->pseudo)
			uid = 0;

		rec = NULL; ret = 1;
		if (uid != 0) {
			ret = mbox_sync_read_index_rec(sync_ctx, uid, &rec);
			if (ret < 0)
				return -1;
		}

		if (ret == 0) {
			/* UID found but it's broken */
			uid = 0;
		} else if (uid == 0 &&
			   !mail_ctx->pseudo &&
			   (sync_ctx->delay_writes ||
			    sync_ctx->idx_seq <= messages_count)) {
			/* If we can't use/store X-UID header, use MD5 sum.
			   Also check for existing MD5 sums when we're actually
			   able to write X-UIDs. */
			sync_ctx->mbox->mbox_save_md5 = TRUE;

			if (mbox_sync_find_index_md5(sync_ctx,
						     mail_ctx->hdr_md5_sum,
						     &rec) < 0)
				return -1;

			if (rec != NULL)
				uid = mail_ctx->mail.uid = rec->uid;
		}

		/* get all sync records related to this message. with pseudo
		   message just get the first sync record so we can jump to
		   it with partial seeking. */
		if (mbox_sync_read_index_syncs(sync_ctx,
					       mail_ctx->pseudo ? 1 : uid,
					       &expunged) < 0)
			return -1;

		if (mail_ctx->pseudo) {
			/* if it was set, it was for the next message */
			expunged = FALSE;
		} else {
			if (rec == NULL) {
				/* message wasn't found from index. we have to
				   read everything from now on, no skipping */
				partial = FALSE;
			}
		}

		if (uid == 0 && !mail_ctx->pseudo) {
			/* missing/broken X-UID. all the rest of the mails
			   need new UIDs. */
			while (sync_ctx->idx_seq <= messages_count) {
				mail_index_expunge(sync_ctx->t,
						   sync_ctx->idx_seq++);
			}

			if (sync_ctx->next_uid == (uint32_t)-1) {
				/* oh no, we're out of UIDs. this shouldn't
				   happen normally, so just try to get it fixed
				   without crashing. */
				mail_storage_set_critical(
					STORAGE(sync_ctx->mbox->storage),
					"Out of UIDs, renumbering them in mbox "
					"file %s", sync_ctx->mbox->path);
				sync_ctx->renumber_uids = TRUE;
				return 0;
			}

			mail_ctx->need_rewrite = TRUE;
			mail_ctx->mail.uid = sync_ctx->next_uid++;
			sync_ctx->prev_msg_uid = mail_ctx->mail.uid;
		}

		if (!mail_ctx->pseudo)
			mail_ctx->mail.idx_seq = sync_ctx->idx_seq;

		if (!expunged) {
			if (mbox_sync_handle_header(mail_ctx) < 0)
				return -1;
			sync_ctx->dest_first_mail = FALSE;
		} else {
			mail_ctx->mail.uid = 0;
			mbox_sync_handle_expunge(mail_ctx);
		}

		if (!mail_ctx->pseudo) {
			if (!expunged) {
				if (mbox_sync_update_index(mail_ctx, rec) < 0)
					return -1;
			}
			sync_ctx->idx_seq++;
		}

		istream_raw_mbox_next(sync_ctx->input,
				      mail_ctx->mail.body_size);
		offset = istream_raw_mbox_get_start_offset(sync_ctx->input);

		if (sync_ctx->need_space_seq != 0) {
			if (mbox_sync_handle_missing_space(mail_ctx) < 0)
				return -1;
			if (mbox_sync_seek(sync_ctx, offset) < 0)
				return -1;
		} else if (sync_ctx->expunged_space > 0) {
			if (!expunged) {
				/* move the body */
				if (mbox_move(sync_ctx,
					      mail_ctx->body_offset -
					      sync_ctx->expunged_space,
					      mail_ctx->body_offset,
					      mail_ctx->mail.body_size) < 0)
					return -1;
				if (mbox_sync_seek(sync_ctx, offset) < 0)
					return -1;
			}
		} else if (partial) {
			ret = mbox_sync_partial_seek_next(sync_ctx, uid + 1,
							  &partial,
							  &skipped_mails);
			if (ret <= 0) {
				if (ret < 0)
					return -1;
				break;
			}
		}
	}

	if (istream_raw_mbox_is_eof(sync_ctx->input)) {
		/* rest of the messages in index don't exist -> expunge them */
		while (sync_ctx->idx_seq <= messages_count)
			mail_index_expunge(sync_ctx->t, sync_ctx->idx_seq++);
	}

	if (!skipped_mails)
		sync_ctx->mbox->mbox_sync_dirty = FALSE;

	if (uids_broken && sync_ctx->delay_writes) {
		/* once we get around to writing the changes, we'll need to do
		   a full sync to avoid the "UIDs broken in partial sync"
		   error */
		sync_ctx->mbox->mbox_sync_dirty = TRUE;
	}
	return 1;
}

static int mbox_write_pseudo(struct mbox_sync_context *sync_ctx)
{
	string_t *str;
	unsigned int uid_validity;

	i_assert(sync_ctx->write_fd != -1);

	uid_validity = sync_ctx->base_uid_validity != 0 ?
		sync_ctx->base_uid_validity : sync_ctx->hdr->uid_validity;
	i_assert(uid_validity != 0);

	str = t_str_new(1024);
	str_printfa(str, "%sDate: %s\n"
		    "From: Mail System Internal Data <MAILER-DAEMON@%s>\n"
		    "Subject: DON'T DELETE THIS MESSAGE -- FOLDER INTERNAL DATA"
		    "\nMessage-ID: <%s@%s>\n"
		    "X-IMAP: %u %010u\n"
		    "Status: RO\n"
		    "\n"
		    PSEUDO_MESSAGE_BODY
		    "\n",
                    mbox_from_create("MAILER_DAEMON", ioloop_time),
		    message_date_create(ioloop_time),
		    my_hostname, dec2str(ioloop_time), my_hostname,
		    uid_validity, sync_ctx->next_uid-1);

	if (pwrite_full(sync_ctx->write_fd,
			str_data(str), str_len(str), 0) < 0) {
		if (!ENOSPACE(errno)) {
			mbox_set_syscall_error(sync_ctx->mbox,
					       "pwrite_full()");
			return -1;
		}

		/* out of disk space, truncate to empty */
		if (ftruncate(sync_ctx->write_fd, 0) < 0)
			mbox_set_syscall_error(sync_ctx->mbox, "ftruncate()");
	}

	sync_ctx->base_uid_last_offset = 0; /* don't bother calculating */
	sync_ctx->base_uid_last = sync_ctx->next_uid-1;
	return 0;
}

static int mbox_sync_handle_eof_updates(struct mbox_sync_context *sync_ctx,
					struct mbox_sync_mail_context *mail_ctx)
{
	const struct stat *st;
	uoff_t file_size, offset, padding, trailer_size;

	if (!istream_raw_mbox_is_eof(sync_ctx->input)) {
		i_assert(sync_ctx->need_space_seq == 0);
		i_assert(sync_ctx->expunged_space == 0);
		return 0;
	}

	/* make sure i_stream_stat() doesn't try to use cached file size */
	i_stream_sync(sync_ctx->file_input);

	st = i_stream_stat(sync_ctx->file_input, TRUE);
	if (st == NULL) {
		mbox_set_syscall_error(sync_ctx->mbox, "i_stream_stat()");
		return -1;
	}
	file_size = st->st_size;
	if (file_size < sync_ctx->file_input->v_offset) {
		mail_storage_set_critical(STORAGE(sync_ctx->mbox->storage),
			"file size unexpectedly shrinked in mbox file %s "
			"(%"PRIuUOFF_T" vs %"PRIuUOFF_T")",
			sync_ctx->mbox->path, file_size,
			sync_ctx->file_input->v_offset);
		return -1;
	}
	trailer_size = file_size - sync_ctx->file_input->v_offset;
	i_assert(trailer_size <= 2);

	if (sync_ctx->need_space_seq != 0) {
		i_assert(sync_ctx->write_fd != -1);

		i_assert(sync_ctx->space_diff < 0);
		padding = MBOX_HEADER_PADDING *
			(sync_ctx->seq - sync_ctx->need_space_seq + 1);
		sync_ctx->space_diff -= padding;

		i_assert(sync_ctx->expunged_space <= -sync_ctx->space_diff);
		sync_ctx->space_diff += sync_ctx->expunged_space;
		sync_ctx->expunged_space = 0;

		if (mail_ctx->have_eoh && !mail_ctx->updated)
			str_append_c(mail_ctx->header, '\n');

		i_assert(sync_ctx->space_diff < 0);

		if (file_set_size(sync_ctx->write_fd,
				  file_size + -sync_ctx->space_diff) < 0) {
			mbox_set_syscall_error(sync_ctx->mbox,
					       "file_set_size()");
			if (ftruncate(sync_ctx->write_fd, file_size) < 0) {
				mbox_set_syscall_error(sync_ctx->mbox,
						       "ftruncate()");
			}
			return -1;
		}
		i_stream_sync(sync_ctx->input);

		if (mbox_sync_rewrite(sync_ctx, mail_ctx, file_size,
				      -sync_ctx->space_diff, padding,
				      sync_ctx->need_space_seq,
				      sync_ctx->seq) < 0)
			return -1;

		update_from_offsets(sync_ctx);

		sync_ctx->need_space_seq = 0;
		array_clear(&sync_ctx->mails);
		p_clear(sync_ctx->saved_keywords_pool);
	}

	if (sync_ctx->expunged_space > 0) {
		i_assert(sync_ctx->write_fd != -1);

		/* copy trailer, then truncate the file */
		st = i_stream_stat(sync_ctx->file_input, TRUE);
		if (st == NULL) {
			mbox_set_syscall_error(sync_ctx->mbox,
					       "i_stream_stat()");
			return -1;
		}

		file_size = st->st_size;
		if (file_size == (uoff_t)sync_ctx->expunged_space) {
			/* everything deleted, the trailer_size still contains
			   the \n trailer though */
			trailer_size = 0;
		}

		i_assert(file_size >= sync_ctx->expunged_space + trailer_size);
		offset = file_size - sync_ctx->expunged_space - trailer_size;
		i_assert(offset == 0 || offset > 31);

		if (mbox_move(sync_ctx, offset,
			      offset + sync_ctx->expunged_space,
			      trailer_size) < 0)
			return -1;
		if (ftruncate(sync_ctx->write_fd,
			      offset + trailer_size) < 0) {
			mbox_set_syscall_error(sync_ctx->mbox, "ftruncate()");
			return -1;
		}

		if (offset == 0) {
			if (mbox_write_pseudo(sync_ctx) < 0)
				return -1;
		}

                sync_ctx->expunged_space = 0;
		i_stream_sync(sync_ctx->input);
	}
	return 0;
}

static int mbox_sync_update_index_header(struct mbox_sync_context *sync_ctx)
{
	const struct stat *st;

	st = i_stream_stat(sync_ctx->file_input, FALSE);
	if (st == NULL) {
		mbox_set_syscall_error(sync_ctx->mbox, "i_stream_stat()");
		return -1;
	}

	if (sync_ctx->moved_offsets &&
	    ((uint64_t)st->st_size == sync_ctx->hdr->sync_size ||
	     (uint64_t)st->st_size == sync_ctx->orig_size)) {
		/* We moved messages inside the mbox file without changing
		   the file's size. If mtime doesn't change, another process
		   not using the same index file as us can't know that the file
		   was changed. So make sure the mtime changes. This should
		   happen rarely enough that the sleeping doesn't become a
		   performance problem.

		   Note that to do this perfectly safe we should do this wait
		   whenever mails are moved or expunged, regardless of whether
		   the file's size changed. That however could become a
		   performance problem and the consequences of being wrong are
		   quite minimal (an extra logged error message). */
		while (sync_ctx->orig_mtime == st->st_mtime) {
			usleep(500000);
			if (utime(sync_ctx->mbox->path, NULL) < 0) {
				mbox_set_syscall_error(sync_ctx->mbox,
						       "utime()");
				return -1;
			}

			st = i_stream_stat(sync_ctx->file_input, FALSE);
			if (st == NULL) {
				mbox_set_syscall_error(sync_ctx->mbox,
						       "i_stream_stat()");
				return -1;
			}
		}
	}

	/* only reason not to have UID validity at this point is if the file
	   is entirely empty. In that case just make up a new one if needed. */
	i_assert(sync_ctx->base_uid_validity != 0 || st->st_size == 0);

	if (sync_ctx->base_uid_validity != sync_ctx->hdr->uid_validity ||
	    sync_ctx->base_uid_validity == 0) {
		if (sync_ctx->base_uid_validity == 0) {
                        sync_ctx->base_uid_validity =
				sync_ctx->hdr->uid_validity != 0 ?
				sync_ctx->hdr->uid_validity :
				(unsigned int)ioloop_time;
		}

		mail_index_update_header(sync_ctx->t,
			offsetof(struct mail_index_header, uid_validity),
			&sync_ctx->base_uid_validity,
			sizeof(sync_ctx->base_uid_validity), TRUE);
	}

	if (istream_raw_mbox_is_eof(sync_ctx->input) &&
	    sync_ctx->next_uid != sync_ctx->hdr->next_uid) {
		i_assert(sync_ctx->next_uid != 0);
		mail_index_update_header(sync_ctx->t,
			offsetof(struct mail_index_header, next_uid),
			&sync_ctx->next_uid, sizeof(sync_ctx->next_uid), FALSE);
	}

	if ((uint32_t)st->st_mtime != sync_ctx->hdr->sync_stamp &&
	    !sync_ctx->mbox->mbox_sync_dirty) {
		uint32_t sync_stamp = st->st_mtime;

		mail_index_update_header(sync_ctx->t,
			offsetof(struct mail_index_header, sync_stamp),
			&sync_stamp, sizeof(sync_stamp), TRUE);
	}

	if ((uint64_t)st->st_size != sync_ctx->hdr->sync_size &&
	    !sync_ctx->mbox->mbox_sync_dirty) {
		uint64_t sync_size = st->st_size;

		mail_index_update_header(sync_ctx->t,
			offsetof(struct mail_index_header, sync_size),
			&sync_size, sizeof(sync_size), TRUE);
	}

	sync_ctx->mbox->mbox_dirty_stamp = st->st_mtime;
	sync_ctx->mbox->mbox_dirty_size = st->st_size;

	return 0;
}

static void mbox_sync_restart(struct mbox_sync_context *sync_ctx)
{
	sync_ctx->base_uid_validity = 0;
	sync_ctx->base_uid_last = 0;
	sync_ctx->base_uid_last_offset = 0;

	array_clear(&sync_ctx->mails);
	array_clear(&sync_ctx->syncs);
	p_clear(sync_ctx->saved_keywords_pool);

	memset(&sync_ctx->sync_rec, 0, sizeof(sync_ctx->sync_rec));
        mail_index_sync_reset(sync_ctx->index_sync_ctx);

	sync_ctx->prev_msg_uid = 0;
	sync_ctx->next_uid = sync_ctx->hdr->next_uid;
	sync_ctx->idx_next_uid = sync_ctx->hdr->next_uid;
	sync_ctx->seq = 0;
	sync_ctx->idx_seq = 1;
	sync_ctx->need_space_seq = 0;
	sync_ctx->expunged_space = 0;
	sync_ctx->space_diff = 0;

	sync_ctx->dest_first_mail = TRUE;
}

static int mbox_sync_do(struct mbox_sync_context *sync_ctx,
			enum mbox_sync_flags flags)
{
	struct mbox_sync_mail_context mail_ctx;
	const struct stat *st;
	unsigned int i;
	int ret, partial;

	st = i_stream_stat(sync_ctx->file_input, FALSE);
	if (st == NULL) {
		mbox_set_syscall_error(sync_ctx->mbox, "i_stream_stat()");
		return -1;
	}
	sync_ctx->orig_size = st->st_size;
	sync_ctx->orig_mtime = st->st_mtime;

	if ((flags & MBOX_SYNC_FORCE_SYNC) != 0) {
		/* forcing a full sync. assume file has changed. */
		partial = FALSE;
		sync_ctx->mbox->mbox_sync_dirty = TRUE;
	} else if ((uint32_t)st->st_mtime == sync_ctx->hdr->sync_stamp &&
		   (uint64_t)st->st_size == sync_ctx->hdr->sync_size) {
		/* file is fully synced */
		partial = TRUE;
		sync_ctx->mbox->mbox_sync_dirty = FALSE;
	} else if ((flags & MBOX_SYNC_UNDIRTY) != 0 ||
		   (uint64_t)st->st_size == sync_ctx->hdr->sync_size) {
		/* we want to do full syncing. always do this if
		   file size hasn't changed but timestamp has. it most
		   likely means that someone had modified some header
		   and we probably want to know about it */
		partial = FALSE;
		sync_ctx->mbox->mbox_sync_dirty = TRUE;
	} else {
		/* see if we can delay syncing the whole file.
		   normally we only notice expunges and appends
		   in partial syncing. */
		partial = TRUE;
		sync_ctx->mbox->mbox_sync_dirty = TRUE;
	}

	mbox_sync_restart(sync_ctx);
	for (i = 0; i < 3; i++) {
		ret = mbox_sync_loop(sync_ctx, &mail_ctx, partial);
		if (ret > 0)
			break;
		if (ret < 0)
			return -1;

		/* partial syncing didn't work, do it again. we get here
		   also if we ran out of UIDs. */
		i_assert(sync_ctx->mbox->mbox_sync_dirty);
		mbox_sync_restart(sync_ctx);

		mail_index_transaction_rollback(&sync_ctx->t);
		sync_ctx->t = mail_index_transaction_begin(sync_ctx->sync_view,
							   FALSE, TRUE);
		partial = FALSE;
	}

	if (mbox_sync_handle_eof_updates(sync_ctx, &mail_ctx) < 0)
		return -1;

	/* only syncs left should be just appends (and their updates)
	   which weren't synced yet for some reason (crash). we'll just
	   ignore them, as we've overwritten them above. */
	array_clear(&sync_ctx->syncs);
	memset(&sync_ctx->sync_rec, 0, sizeof(sync_ctx->sync_rec));

	if (mbox_sync_update_index_header(sync_ctx) < 0)
		return -1;

	return 0;
}

int mbox_sync_has_changed(struct mbox_mailbox *mbox, bool leave_dirty)
{
	const struct mail_index_header *hdr;
	const struct stat *st;
	struct stat statbuf;

	if (mbox->mbox_file_stream != NULL && mbox->mbox_fd == -1) {
		/* read-only stream */
		st = i_stream_stat(mbox->mbox_file_stream, FALSE);
		if (st == NULL) {
			mbox_set_syscall_error(mbox, "i_stream_stat()");
			return -1;
		}
	} else {
		if (stat(mbox->path, &statbuf) < 0) {
			mbox_set_syscall_error(mbox, "stat()");
			return -1;
		}
		st = &statbuf;
	}

	hdr = mail_index_get_header(mbox->ibox.view);

	if ((uint32_t)st->st_mtime == hdr->sync_stamp &&
	    (uint64_t)st->st_size == hdr->sync_size) {
		/* fully synced */
		mbox->mbox_sync_dirty = FALSE;
		return 0;
	}

	if (!mbox->mbox_sync_dirty || !leave_dirty) {
		mbox->mbox_sync_dirty = TRUE;
		return 1;
	}

	return st->st_mtime != mbox->mbox_dirty_stamp ||
		st->st_size != mbox->mbox_dirty_size;
}

static void mbox_sync_context_free(struct mbox_sync_context *sync_ctx)
{
	if (sync_ctx->t != NULL)
		mail_index_transaction_rollback(&sync_ctx->t);
	if (sync_ctx->index_sync_ctx != NULL)
		mail_index_sync_rollback(&sync_ctx->index_sync_ctx);
	pool_unref(sync_ctx->mail_keyword_pool);
	pool_unref(sync_ctx->saved_keywords_pool);
	str_free(&sync_ctx->header);
	str_free(&sync_ctx->from_line);
	array_free(&sync_ctx->mails);
	array_free(&sync_ctx->syncs);
}

int mbox_sync(struct mbox_mailbox *mbox, enum mbox_sync_flags flags)
{
	struct mail_index_sync_ctx *index_sync_ctx;
	struct mail_index_view *sync_view;
	struct mbox_sync_context sync_ctx;
	uint32_t seq;
	uoff_t offset;
	unsigned int lock_id = 0;
	int ret, changed;
	bool delay_writes;

	delay_writes = mbox->mbox_readonly ||
		((flags & MBOX_SYNC_REWRITE) == 0 &&
		 getenv("MBOX_LAZY_WRITES") != NULL);

	mbox->ibox.sync_last_check = ioloop_time;

	if (!mbox->mbox_do_dirty_syncs)
		flags |= MBOX_SYNC_UNDIRTY;

	if ((flags & MBOX_SYNC_LOCK_READING) != 0) {
		if (mbox_lock(mbox, F_RDLCK, &lock_id) <= 0)
			return -1;
	}

	if ((flags & MBOX_SYNC_HEADER) != 0 ||
	    (flags & MBOX_SYNC_FORCE_SYNC) != 0)
		changed = 1;
	else {
		bool leave_dirty = (flags & MBOX_SYNC_UNDIRTY) == 0;
		if ((changed = mbox_sync_has_changed(mbox, leave_dirty)) < 0) {
			if ((flags & MBOX_SYNC_LOCK_READING) != 0)
				(void)mbox_unlock(mbox, lock_id);
			return -1;
		}
	}

	if ((flags & MBOX_SYNC_LOCK_READING) != 0) {
		/* we just want to lock it for reading. if mbox hasn't been
		   modified don't do any syncing. */
		if (!changed)
			return 0;

		/* have to sync to make sure offsets have stayed the same */
		(void)mbox_unlock(mbox, lock_id);
		lock_id = 0;
	}

	/* reopen input stream to make sure it has nothing buffered */
        mbox_file_close_stream(mbox);

__again:
	if (changed) {
		/* we're most likely modifying the mbox while syncing, just
		   lock it for writing immediately. the mbox must be locked
		   before index syncing is started to avoid deadlocks, so we
		   don't have much choice either (well, easy ones anyway). */
		int lock_type = mbox->mbox_readonly ? F_RDLCK : F_WRLCK;
		if (mbox_lock(mbox, lock_type, &lock_id) <= 0)
			return -1;
	}

	if ((flags & MBOX_SYNC_LAST_COMMIT) != 0) {
		seq = mbox->ibox.commit_log_file_seq;
		offset = mbox->ibox.commit_log_file_offset;
	} else {
		seq = (uint32_t)-1;
		offset = (uoff_t)-1;
	}

	ret = mail_index_sync_begin(mbox->ibox.index, &index_sync_ctx,
				    &sync_view, seq, offset,
				    !mbox->ibox.keep_recent,
				    (flags & MBOX_SYNC_REWRITE) != 0);
	if (ret <= 0) {
		if (ret < 0)
			mail_storage_set_index_error(&mbox->ibox);
		if (lock_id != 0)
			(void)mbox_unlock(mbox, lock_id);
		return ret;
	}

	if (!changed && !mail_index_sync_have_more(index_sync_ctx)) {
		/* nothing to do */
	__nothing_to_do:
		if (lock_id != 0)
			(void)mbox_unlock(mbox, lock_id);

		/* index may need to do internal syncing though, so commit
		   instead of rollbacking. */
		if (mail_index_sync_commit(&index_sync_ctx) < 0) {
			mail_storage_set_index_error(&mbox->ibox);
			return -1;
		}
		return 0;
	}

	memset(&sync_ctx, 0, sizeof(sync_ctx));
	sync_ctx.mbox = mbox;

	sync_ctx.hdr = mail_index_get_header(sync_view);
	sync_ctx.from_line = str_new(default_pool, 256);
	sync_ctx.header = str_new(default_pool, 4096);

	sync_ctx.index_sync_ctx = index_sync_ctx;
	sync_ctx.sync_view = sync_view;
	sync_ctx.t = mail_index_transaction_begin(sync_view, FALSE, TRUE);
	sync_ctx.mail_keyword_pool =
		pool_alloconly_create("mbox keywords", 256);
	sync_ctx.saved_keywords_pool =
		pool_alloconly_create("mbox saved keywords", 4096);

	/* make sure we've read the latest keywords in index */
	(void)mail_index_get_keywords(mbox->ibox.index);

	i_array_init(&sync_ctx.mails, 64);
	i_array_init(&sync_ctx.syncs, 32);

	sync_ctx.flags = flags;
	sync_ctx.delay_writes = delay_writes || sync_ctx.mbox->mbox_readonly;

	if (!changed && delay_writes) {
		/* if we have only flag changes, we don't need to open the
		   mbox file */
		bool expunged;

		if (mbox_sync_read_index_syncs(&sync_ctx, 1, &expunged) < 0)
			return -1;
		if (sync_ctx.sync_rec.uid1 == 0) {
			if (mail_index_transaction_commit(&sync_ctx.t,
							  &seq, &offset) < 0) {
				mail_storage_set_index_error(&mbox->ibox);
				mbox_sync_context_free(&sync_ctx);
				if (lock_id != 0)
					(void)mbox_unlock(mbox, lock_id);
				return -1;
			}
			sync_ctx.t = NULL;

			sync_ctx.index_sync_ctx = NULL;
			mbox_sync_context_free(&sync_ctx);
			goto __nothing_to_do;
		}
	}

	if (lock_id == 0) {
		/* ok, we have something to do but no locks. we'll have to
		   restart syncing to avoid deadlocking. */
		mbox_sync_context_free(&sync_ctx);
		changed = 1;
		goto __again;
	}

	if (mbox_file_open_stream(mbox) < 0) {
		mbox_sync_context_free(&sync_ctx);
		(void)mbox_unlock(mbox, lock_id);
		return -1;
	}

	sync_ctx.file_input = sync_ctx.mbox->mbox_file_stream;
	sync_ctx.input = sync_ctx.mbox->mbox_stream;
	sync_ctx.write_fd = sync_ctx.mbox->mbox_lock_type != F_WRLCK ? -1 :
		sync_ctx.mbox->mbox_fd;

	ret = mbox_sync_do(&sync_ctx, flags);

	if (ret < 0)
		mail_index_transaction_rollback(&sync_ctx.t);
	else if (mail_index_transaction_commit(&sync_ctx.t,
					       &seq, &offset) < 0) {
		mail_storage_set_index_error(&mbox->ibox);
		ret = -1;
	} else {
		mbox->ibox.commit_log_file_seq = 0;
		mbox->ibox.commit_log_file_offset = 0;
	}
	sync_ctx.t = NULL;

	if (ret < 0)
		mail_index_sync_rollback(&index_sync_ctx);
	else if (mail_index_sync_commit(&index_sync_ctx) < 0) {
		mail_storage_set_index_error(&mbox->ibox);
		ret = -1;
	}
	sync_ctx.index_sync_ctx = NULL;

	if (sync_ctx.base_uid_last != sync_ctx.next_uid-1 &&
	    ret == 0 && !sync_ctx.delay_writes &&
	    sync_ctx.base_uid_last_offset != 0) {
		/* Rewrite uid_last in X-IMAPbase header if we've seen it
		   (ie. the file isn't empty) */
                ret = mbox_rewrite_base_uid_last(&sync_ctx);
	}

	i_assert(lock_id != 0);

	if (mbox->mbox_lock_type != F_RDLCK) {
		/* drop to read lock */
		unsigned int read_lock_id = 0;

		if (mbox_lock(mbox, F_RDLCK, &read_lock_id) <= 0)
			ret = -1;
		else {
			if (mbox_unlock(mbox, lock_id) < 0)
				ret = -1;
			lock_id = read_lock_id;
		}
	}

	if ((flags & MBOX_SYNC_LOCK_READING) == 0) {
		if (mbox_unlock(mbox, lock_id) < 0)
			ret = -1;
	}

	mbox_sync_context_free(&sync_ctx);
	return ret;
}

struct mailbox_sync_context *
mbox_storage_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)box;
	enum mbox_sync_flags mbox_sync_flags = 0;
	int ret = 0;

	if (!box->opened)
		index_storage_mailbox_open(&mbox->ibox);

	if ((flags & MAILBOX_SYNC_FLAG_FAST) == 0 ||
	    mbox->ibox.sync_last_check + MAILBOX_FULL_SYNC_INTERVAL <=
	    ioloop_time) {
		if ((flags & MAILBOX_SYNC_FLAG_FULL_READ) != 0 &&
		    !mbox->mbox_very_dirty_syncs)
			mbox_sync_flags |= MBOX_SYNC_UNDIRTY;
		if ((flags & MAILBOX_SYNC_FLAG_FULL_WRITE) != 0)
			mbox_sync_flags |= MBOX_SYNC_REWRITE;
		ret = mbox_sync(mbox, mbox_sync_flags);
	}

	return index_mailbox_sync_init(box, flags, ret < 0);
}
