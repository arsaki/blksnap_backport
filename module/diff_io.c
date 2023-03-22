// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME "-diff-io: " fmt

#ifdef HAVE_GENHD_H
#include <linux/genhd.h>
#endif
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/version.h>
#include "memory_checker.h"
#include "diff_io.h"
#include "diff_buffer.h"
#include "log.h"

#ifdef STANDALONE_BDEVFILTER
#ifndef PAGE_SECTORS
#define PAGE_SECTORS	(1 << (PAGE_SHIFT - SECTOR_SHIFT))
#endif
#endif

struct bio_set diff_io_bioset;

int diff_io_init(void)
{
	return bioset_init(&diff_io_bioset, 64, 0,
			   BIOSET_NEED_BVECS | BIOSET_NEED_RESCUER);
}

void diff_io_done(void)
{
	bioset_exit(&diff_io_bioset);
}

static void diff_io_notify_cb(struct work_struct *work)
{
	struct diff_io_async *async =
		container_of(work, struct diff_io_async, work);

	might_sleep();
	async->notify_cb(async->ctx);
}

#ifdef STANDALONE_BDEVFILTER
void diff_io_endio(struct bio *bio)
#else
static void diff_io_endio(struct bio *bio)
#endif
{
	struct diff_io *diff_io = bio->bi_private;

	if (bio->bi_status != BLK_STS_OK)
		diff_io->error = -EIO;

	if (atomic_dec_and_test(&diff_io->bio_count)) {
		if (diff_io->is_sync_io)
			complete(&diff_io->notify.sync.completion);
		else
			queue_work(system_wq, &diff_io->notify.async.work);
	}

	bio_put(bio);
}

static inline struct diff_io *diff_io_new(bool is_write, bool is_nowait)
{
	struct diff_io *diff_io;
	gfp_t gfp_mask = is_nowait ? (GFP_NOIO | GFP_NOWAIT) : GFP_NOIO;

	diff_io = kzalloc(sizeof(struct diff_io), gfp_mask);
	if (unlikely(!diff_io))
		return NULL;
	memory_object_inc(memory_object_diff_io);

	diff_io->error = 0;
	diff_io->is_write = is_write;
	atomic_set(&diff_io->bio_count, 0);

	return diff_io;
}

struct diff_io *diff_io_new_sync(bool is_write)
{
	struct diff_io *diff_io;

	diff_io = diff_io_new(is_write, false);
	if (unlikely(!diff_io))
		return NULL;

	diff_io->is_sync_io = true;
	init_completion(&diff_io->notify.sync.completion);
	return diff_io;
}

struct diff_io *diff_io_new_async(bool is_write, bool is_nowait,
				  void (*notify_cb)(void *ctx), void *ctx)
{
	struct diff_io *diff_io;

	diff_io = diff_io_new(is_write, is_nowait);
	if (unlikely(!diff_io))
		return NULL;

	diff_io->is_sync_io = false;
	INIT_WORK(&diff_io->notify.async.work, diff_io_notify_cb);
	diff_io->notify.async.ctx = ctx;
	diff_io->notify.async.notify_cb = notify_cb;
	return diff_io;
}

static inline bool check_page_aligned(sector_t sector)
{
	return !(sector & ((1ull << (PAGE_SHIFT - SECTOR_SHIFT)) - 1));
}

static inline unsigned short calc_page_count(sector_t sectors)
{
	return round_up(sectors, PAGE_SECTORS) / PAGE_SECTORS;
}

#ifdef HAVE_BIO_MAX_PAGES
static inline unsigned int bio_max_segs(unsigned int nr_segs)
{
	return min(nr_segs, (unsigned int)BIO_MAX_PAGES);
}
#endif

/*
 * diff_io_do() - Perform an I/O operation.
 *
 * Returns zero if successful. Failure is possible if the is_nowait flag is set
 * and a failure was occured when allocating memory. In this case, the error
 * code -EAGAIN is returned. The error code -EINVAL means that the input
 * arguments are incorrect.
 */
int diff_io_do(struct diff_io *diff_io, struct diff_region *diff_region,
	       struct diff_buffer *diff_buffer, const bool is_nowait)
{
	struct bio *bio;
	struct bio_list bio_list_head = BIO_EMPTY_LIST;
	struct page **current_page_ptr;
	sector_t processed = 0;
	gfp_t gfp = GFP_NOIO | (is_nowait ? GFP_NOWAIT : 0);
	unsigned int opf = diff_io->is_write ? REQ_OP_WRITE : REQ_OP_READ;
	unsigned op_flags = REQ_SYNC | (diff_io->is_write ? REQ_FUA : 0);

	if (unlikely(!check_page_aligned(diff_region->sector))) {
		pr_err("Difference storage block should be aligned to PAGE_SIZE\n");
		return -EINVAL;
	}

	if (unlikely(calc_page_count(diff_region->count) >
		     diff_buffer->page_count)) {
		pr_err("The difference storage block is larger than the buffer size\n");
		return -EINVAL;
	}

	/* Append bio with datas to bio_list */
	current_page_ptr = diff_buffer->pages;
	while (processed < diff_region->count) {
		sector_t offset = 0;
		sector_t portion;
		unsigned short nr_iovecs;

		portion = diff_region->count - processed;
		nr_iovecs = calc_page_count(portion);

		if (nr_iovecs > bio_max_segs(nr_iovecs)) {
			nr_iovecs = bio_max_segs(nr_iovecs);
			portion = nr_iovecs * PAGE_SECTORS;
		}

#ifdef HAVE_BDEV_BIO_ALLOC
		bio = bio_alloc_bioset(diff_region->bdev, nr_iovecs,
				       opf | op_flags,  gfp, &diff_io_bioset);
#else
		bio = bio_alloc_bioset(gfp, nr_iovecs, &diff_io_bioset);
#endif
		if (unlikely(!bio))
			goto fail;

#ifndef STANDALONE_BDEVFILTER
		bio_set_flag(bio, BIO_FILTERED);
#endif
		bio->bi_end_io = diff_io_endio;
		bio->bi_private = diff_io;
		bio_set_dev(bio, diff_region->bdev);
		bio->bi_iter.bi_sector = diff_region->sector + processed;

#ifndef HAVE_BDEV_BIO_ALLOC
		bio_set_op_attrs(bio, opf, op_flags);
#endif

		while (offset < portion) {
			sector_t bvec_len_sect;
			unsigned int bvec_len;

			bvec_len_sect = min_t(sector_t, PAGE_SECTORS,
					      portion - offset);
			bvec_len =
				(unsigned int)(bvec_len_sect << SECTOR_SHIFT);

			/* All pages offset aligned to PAGE_SIZE */
			__bio_add_page(bio, *current_page_ptr, bvec_len, 0);

			current_page_ptr++;
			offset += bvec_len_sect;
		}

		bio_list_add(&bio_list_head, bio);
		atomic_inc(&diff_io->bio_count);

		processed += offset;
	}

	/* sumbit all bios */
	while ((bio = bio_list_pop(&bio_list_head)))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
		submit_bio_noacct(bio);
#else
		generic_make_request(bio);
#endif
	if (diff_io->is_sync_io)
		wait_for_completion_io(&diff_io->notify.sync.completion);

	return 0;
fail:
	while ((bio = bio_list_pop(&bio_list_head)))
		bio_put(bio);
	return -EAGAIN;
}
