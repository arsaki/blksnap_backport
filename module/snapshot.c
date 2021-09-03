// SPDX-License-Identifier: GPL-2.0
#define BLK_SNAP_SECTION "-snapshot"
#include "common.h"
#include "snapshot.h"
#include "tracker.h"
#include "snapimage.h"
#include "tracking.h"

LIST_HEAD(snapshots);
DECLARE_RWSEM(snapshots_lock);

void snapshot_free(struct kref *kref)
{
	struct snapshot *snapshot = container_of(kref, struct snapshot, kref);
	struct event *event;
	int inx;

	for (inx = 0; inx < snapshot->count; ++inx) {
		if (!snapshot->snapimage_array[inx])
			continue;
		snapimage_put(snapshot->snapimage_array[inx]);
	}
	kfree(snapshot->snapimage_array);

	for (inx = 0; inx < snapshot->count; ++inx) {
		struct tracker *tracker = snapshot->tracker_array[inx];

		if (!tracker)
			continue;

#if defined(HAVE_SUPER_BLOCK_FREEZE)
		_freeze_bdev(tracker->diff_area->orig_bdev, &snapshot->superblock_array[inx]);
#else
		if (freeze_bdev(tracker->diff_area->orig_bdev))
			pr_err("Failed to freeze device [%d:%d]\n",
			       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
#endif
	}

	for (inx = 0; inx < snapshot->count; ++inx) {
		if (!snapshot->tracker_array[inx])
			continue;
		tracker_release_snapshot(snapshot->tracker_array[inx]);	
	}

	for (inx = 0; inx < snapshot->count; ++inx) {
		struct tracker *tracker = snapshot->tracker_array[inx];

		if (!tracker)
			continue;
#if defined(HAVE_SUPER_BLOCK_FREEZE)
		_thaw_bdev(tracker->diff_area->orig_bdev, snapshot->superblock_array[inx]);
#else
		if (thaw_bdev(tracker->diff_area->orig_bdev))
			pr_err("Failed to thaw device [%d:%d]\n",
			       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
#endif
	}

	for (inx = 0; inx < snapshot->count; ++inx)
		tracker_put(snapshot->tracker_array[inx]);
	kfree(snapshot->tracker_array);

	if (snapshot->diff_storage)
		diff_storage_put(snapshot->diff_storage);

	kfree(snapshot);	
}

static inline void snapshot_get(struct snapshot *snapshot)
{
	kref_get(&snapshot->kref);
}
static inline void snapshot_put(struct snapshot *snapshot)
{
	if (likely(snapshot))
		kref_put(&snapshot->kref, snapshot_free);
}
/*
static void _snapshot_destroy(struct snapshot *snapshot)
{
	size_t inx;

	for (inx = 0; inx < snapshot->dev_id_set_size; ++inx)
		snapimage_stop(snapshot->dev_id_set[inx]);

	pr_info("Release snapshot [0x%llx]\n", snapshot->id);

	tracker_release_snapshot(snapshot->dev_id_set, snapshot->dev_id_set_size);

	for (inx = 0; inx < snapshot->dev_id_set_size; ++inx)
		snapimage_destroy(snapshot->dev_id_set[inx]);

	snapshot_put(snapshot);
}
*/
static struct snapshot * snapshot_new(unsigned int count)
{
	struct snapshot *snapshot = NULL;
	dev_t *snap_set = NULL;

	snapshot = kzalloc(sizeof(struct snapshot), GFP_KERNEL);
	if (!snapshot)
		return ERR_PTR(-ENOMEM);

	snapshot->tracker_array = kcalloc(count, sizeof(void *), GFP_KERNEL);
	if (!snapshot->tracker_array) {
		kfree(snapshot);
		return ERR_PTR(-ENOMEM);
	}

	snapshot->snapimage_array = kcalloc(count, sizeof(void *), GFP_KERNEL);
	if (!snapshot->snapimage_array) {
		kfree(snapshot->tracker_array);
		kfree(snapshot);
		return ERR_PTR(-ENOMEM);
	}

#if defined(HAVE_SUPER_BLOCK_FREEZE)
	snapshot->superblock_array = kcalloc(count, sizeof(void *), GFP_KERNEL);
	if (!snapshot->superblock_array) {
		kfree(snapshot->snapimage_array);
		kfree(snapshot->tracker_array);
		snapshot_put(snapshot);
		return ERR_PTR(-ENOMEM);
	}

#endif
	INIT_LIST_HEAD(&snapshot->link);
	kref_init(&snapshot->kref);
	uuid_gen(snapshot->id);

	down_write(&snapshots_lock);
	list_add_tail(&snapshots, &snapshot->link);
	up_write(&snapshots_lock);

	return snapshot;
}

void snapshot_done(void)
{
	struct snapshot *snapshot;

	pr_info("Removing all snapshots\n");
	down_write(&snapshots_lock);
	while((snap = list_first_entry_or_null(snapshots, struct snapshot, list))) {
		list_del(&snapshot->link);
		snapshot_put(snapshot);
	}
	up_write(&snapshots_lock);
}

int snapshot_create(dev_t *dev_id_array, unsigned int count, uuid_t *id)
{
	struct snapshot *snapshot = NULL;
	int ret;
	unsigned int inx;

	pr_info("Create snapshot for devices:\n");
	for (inx = 0; inx < count; ++inx)
		pr_info("\t%d:%d\n", MAJOR(dev_id_array[inx]), MINOR(dev_id_array[inx]));

	snapshot = snapshot_new(count);
	if (IS_ERR(snapshot)) {
		pr_err("Unable to create snapshot: failed to allocate snapshot structure\n");
		return PTR_ERR(snapshot);
	}

	ret = -ENODEV;
	for (inx = 0; inx < snapshot->count; ++inx) {
		struct tracker *tracker;

		tracker = tracker_create_or_get(dev_id_array[inx]);
		if (IS_ERR(tracker)){
			pr_err("Unable to create snapshot\n");
			pr_err("Failed to add device [%d:%d] to snapshot tracking\n",
			       MAJOR(dev_id_array[inx]), MINOR(dev_id_array[inx]));
			ret = PTR_ERR(tracker);
			goto fail;
		}

		snapshot->tracker_array[inx] = tracker;
	}

	uuid_copy(id, snapshot->id);
	pr_info("Snapshot %pUb was created\n", snapshot->id);
	return 0;
fail:
	pr_info("Snapshot cannot be created\n");

	down_write(&snapshots_lock);
	list_del(&snapshot->link);
	up_write(&snapshots_lock);

	snapshot_put(snapshot);
	return ret;
}

struct snapshot *snapshot_get_by_id(uuid_t *id)
{
	struct snapshot *snapshot = NULL;
	struct snapshot *_snap;

	down_read(&snapshots_lock);
	if (list_empty(&snapshots))
		goto out;

	list_for_each_entry(_snap, &snapshots, link) {
		if (_snap->id == id) {
			snapshot = _snap;
			snapshot_get(snapshot);
			break;
		}
	}
out:
	up_read(&snapshots_lock);
	return snapshot;
}



int snapshot_destroy(uuid_t *id)
{
	struct snapshot *snapshot = NULL;

	down_read(&snapshots_lock);
	if (!list_empty(&snapshots)) {
		struct snapshot *_snap = NULL;

		list_for_each_entry(_snap, &snapshots, link) {
			if (_snap->id == id) {
				snapshot = _snap;
				list_del(&snapshot->link);
				break;
			}
		}
	}
	up_read(&snapshots_lock);

	if (!snapshot) {
		pr_err("Unable to destroy snapshot: cannot find snapshot by id %pUb\n", id);
		return -ENODEV;
	}

	pr_info("Destroy snapshot [0x%llx]\n", id);
	snapshot_put(snapshot);
	return 0;
}

int snapshot_append_storage(uuid_t *id, dev_t dev_id, sector_t sector, sector_t count)
{
	int ret = 0;
	struct snapshot *snapshot;

	snapshot = snapshot_get_by_id(id);
	if (!snapshot)
		return -ESRCH;

	ret = diff_storage_append_block(snapshot->diff_storage, dev_id, sector, count);

	snapshot_put(snapshot);
	return ret;
}

int snapshot_take(uuid_t *id)
{
	int ret = 0;
	struct snapshot *snapshot;
	int inx;

	snapshot = snapshot_get_by_id(id);
	if (!snapshot)
		return -ESRCH;

	/* allocate diff area for each device in snapshot */
	for (inx = 0; inx < snapshot->count; inx++) {
		struct tracker *tracker = snapshot->tracker_array[inx];
		struct diff_area *diff_area;

		if (!tracker)
			continue;

		diff_area = diff_area_new(tracker->dev_id, snapshot->diff_storage, &snapshot->event_queue);
		if (IS_ERR(diff_area)) {
			ret = PTR_ERR(diff_area);
			goto fail;
		}

		tracker->diff_area = diff_area;
	}

	for (inx = 0; inx < snapshot->count; inx++) {
		struct tracker *tracker = snapshot->tracker_array[inx];

		if (!tracker)
			continue;

#if defined(HAVE_SUPER_BLOCK_FREEZE)
		_freeze_bdev(tracker->diff_area->orig_bdev, &snapshot->superblock_array[inx]);
#else
		if (freeze_bdev(tracker->diff_area->orig_bdev))
			pr_err("Failed to freeze device [%d:%d]\n",
			       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
#endif
	}
	for (inx = 0; inx < snapshot->count; inx++) {
		if (!snapshot->tracker_array[inx])
			continue;
		ret = tracker_take_snapshot(snapshot->tracker_array[inx]);
		if (ret) {
			pr_err("Unable to take snapshot: failed to capture snapshot %pUb\n",
			       snapshot->id);
			goto fail;
		}
	}

	for (inx = 0; inx < snapshot->count; inx++) {
		struct tracker *tracker = snapshot->tracker_array[inx];

		if (!tracker)
			continue;

#if defined(HAVE_SUPER_BLOCK_FREEZE)
		_thaw_bdev(tracker->diff_area->orig_bdev, snapshot->superblock_array[inx]);
#else
		if (thaw_bdev(tracker->diff_area->orig_bdev))
			pr_err("Failed to thaw device [%d:%d]\n",
			       MAJOR(tracker->dev_id), MINOR(tracker->dev_id));
#endif

		if (snapstore_device_is_corrupted(tracker->snapdev)) {
			pr_err("Unable to freeze devices [%d:%d]: snapshot data is corrupted\n",
			       MAJOR(dev_id), MINOR(dev_id));
			ret = -EDEADLK;
			break;
		}
	}
	for (inx = 0; inx < snapshot->count; inx++) {
		struct snapimage *snapimage;
		struct tracker *tracker = snapshot->tracker_array[inx];

		snapimage = snapimage_create(tracker->diff_area, tracker->cbt_map);
		if (IS_ERR(snapimage)) {
			ret = PTR_ERR(snapimage);
			pr_err("Failed to create snapshot image for device [%d:%d] with error=%d\n",
			       MAJOR(tracker->dev_id), MINOR(tracker->dev_id), ret);
			break;
		}
		snapshot->snapimage_array[inx] = snapimage;
	}

	goto out;
fail:
	pr_err("Unable to take snapshot: failed to capture snapshot %pUb\n",
	       snapshot->id);
	/* release all taken snapshots */
	while ((--inx) >= 0) {
		if (!snapshot->tracker_array[inx])
			continue;
		tracker_release_snapshot(snapshot->tracker_array[inx]);
	}
	/* thaw all freeze devices */
	for (inx = 0; inx < snapshot->count; inx++) {
		if (!snapshot->tracker_array[inx])
			continue;
		tracker_thaw(snapshot->tracker_array[inx]);
	}
	/* release all diff_area */
	for (inx = 0; inx < snapshot->count; inx++) {
		struct tracker *tracker = snapshot->tracker_array[inx];

		if (!tracker)
			continue;
		if (tracker->diff_area)
			diff_area_put(tracker->diff_area);
	}
out:
	snapshot_put(snapshot);
	return ret;
}


int snapshot_wait_event(uuid_t *id, unsigned int timeout_ms, size_t data_size,
			ktime_t *time_label, unsigned int *code, u8 *data)
{
	struct snapshot *snapshot;
	struct event *event;

	snapshot = snapshot_get_by_id(uuid_t *id);
	if (!snapshot)
		return -ESRCH;

	event = event_wait(&snapshot->event_queue, timeout_ms);
	if (IS_ERR(event)) {
		ret = PTR_ERR(event);
		goto out;
	}

	if (unlikely(data_size < event->data_size))
		ret = -ENOSPC;
	else {
		*time_label = event->time;
		*code = event->code;
		memcpy(*data, event->data, event->data_size);
	}

	kfree(event);
out:
	snapshot_put(snapshot);
	return ret;
}

int snapshot_collect_images(uuid_t *id, struct blk_snap_image_info __user *user_image_info_array,
			     unsigned int *pcount)
{
	int ret = 0;
	int count = 0;
	unsigned long len;
	struct blk_snap_image_info *image_info_array = NULL;
	struct snapshot *snapshot;

	snapshot = snapshot_get_by_id(id);

	for (inx=0; inx<snapshot->count; inx++) {
		if (snapshot->snapimage_array)
			count++;
	}

	if (*pcount < count) {
		res = -ENODATA;
		goto out;
	}

	image_info_array = kcalloc(count, sizeof(struct blk_snap_image_info), GFP_KERNEL);
	if (!image_info_array) {
		pr_err("Unable to collect snapshot images: not enough memory.\n");
		res = -ENOMEM;
		goto out;
	}
	
	count = 0;
	for (inx=0; inx<snapshot->count; inx++) {
		if (!snapshot->snapimage_array[inx])
			continue;
		image_info_array[count].original_dev_id = snapshot->snapimage_array[inx].original_dev_id;
		image_info_array[count].image_dev_id = snapshot->snapimage_array[inx].image_dev_id;

		count++;
	}

	len = copy_to_user(user_image_info_array, image_info_array,
	                   real_count * sizeof(struct blk_snap_image_info));
	if (len != 0) {
		pr_err("Unable to collect snapshot images: failed to copy data to user buffer\n");
		res = -ENODATA;
	}
out:
	kfree(image_info_array);
	snapshot_put(snapshot);
	*pcount = count;
	return res;
}
