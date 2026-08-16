/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_PANIC_DISK_STORE

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kmsg_dump.h>
#include <linux/timekeeping.h>

#define PANIC_DISK_STORE_BDEV_PATH "/dev/block/mmcblk0p45"
#define PANIC_DISK_STORE_PARTITION_SIZE (10 * 1024 * 1024)
#define PANIC_DISK_STORE_HEADER_SIZE PAGE_SIZE
#define PANIC_DISK_STORE_MAX_LOG_SIZE (8 * 1024 * 1024)
#define PANIC_DISK_STORE_IO_SIZE (64 * 1024)
#define PANIC_DISK_STORE_IO_TIMEOUT_MS 2000
#define PANIC_DISK_STORE_LINE_SIZE (16 * 1024)
#define PANIC_DISK_STORE_MAGIC "PDSTORE1"
#define PANIC_DISK_STORE_DONE "\nLOG DONE\n"
#define PANIC_DISK_STORE_SECTOR_SHIFT 9

struct panic_disk_store_header {
	char magic[8];
	__le32 version;
	__le32 header_size;
	__le32 status;
	__le32 reserved0;
	__le64 timestamp;
	__le32 log_size;
	__le32 log_crc32;
	__le32 header_crc32;
} __packed;

#define PANIC_DISK_STORE_STATUS_WRITING 1
#define PANIC_DISK_STORE_STATUS_COMPLETE 2

static struct block_device *panic_disk_store_bdev;
static char panic_disk_store_ring[PANIC_DISK_STORE_MAX_LOG_SIZE]
	__aligned(PAGE_SIZE);
static char panic_disk_store_line[PANIC_DISK_STORE_LINE_SIZE];
static char panic_disk_store_io[PANIC_DISK_STORE_IO_SIZE]
	__aligned(PAGE_SIZE);
static char panic_disk_store_header_page[PAGE_SIZE] __aligned(PAGE_SIZE);
static struct bio panic_disk_store_bio;
static struct bio_vec panic_disk_store_vecs[PANIC_DISK_STORE_IO_SIZE / PAGE_SIZE];
static atomic_t panic_disk_store_active = ATOMIC_INIT(0);
static atomic_t panic_disk_store_io_done = ATOMIC_INIT(0);
static size_t panic_disk_store_head;
static size_t panic_disk_store_tail;
static size_t panic_disk_store_used;
static bool panic_disk_store_captured;
static bool panic_disk_store_io_aborted;
static blk_status_t panic_disk_store_io_status;

static void panic_disk_store_ring_advance(size_t *offset, size_t count)
{
	*offset = (*offset + count) % PANIC_DISK_STORE_MAX_LOG_SIZE;
}

static void panic_disk_store_ring_drop_line(void)
{
	while (panic_disk_store_used) {
		char c = panic_disk_store_ring[panic_disk_store_head];

		panic_disk_store_ring_advance(&panic_disk_store_head, 1);
		panic_disk_store_used--;
		if (!c)
			return;
	}
}

static void panic_disk_store_ring_append(const char *line, size_t len)
{
	size_t capacity = PANIC_DISK_STORE_MAX_LOG_SIZE -
		(sizeof(PANIC_DISK_STORE_DONE) - 1);
	size_t i;

	if (len + 1 > capacity)
		return;

	while (capacity - panic_disk_store_used < len + 1)
		panic_disk_store_ring_drop_line();

	for (i = 0; i < len; i++) {
		panic_disk_store_ring[panic_disk_store_tail] = line[i];
		panic_disk_store_ring_advance(&panic_disk_store_tail, 1);
	}
	panic_disk_store_ring[panic_disk_store_tail] = '\0';
	panic_disk_store_ring_advance(&panic_disk_store_tail, 1);
	panic_disk_store_used += len + 1;
}

static void panic_disk_store_capture(struct kmsg_dumper *dumper,
					     enum kmsg_dump_reason reason)
{
	size_t len;

	if (reason != KMSG_DUMP_PANIC ||
		atomic_xchg(&panic_disk_store_active, 1))
		return;

	panic_disk_store_head = 0;
	panic_disk_store_tail = 0;
	panic_disk_store_used = 0;
	while (kmsg_dump_get_line(dumper, true, panic_disk_store_line,
				  sizeof(panic_disk_store_line), &len))
		panic_disk_store_ring_append(panic_disk_store_line, len);
	panic_disk_store_captured = true;
}

static void panic_disk_store_end_io(struct bio *bio)
{
	panic_disk_store_io_status = bio->bi_status;
	/* Publish bi_status before the polling CPU observes completion. */
	smp_wmb();
	atomic_set(&panic_disk_store_io_done, 1);
}

static int panic_disk_store_write_chunk(const void *buf, size_t len,
					 sector_t sector, ktime_t deadline,
					 unsigned int op_flags)
{
	struct page *page;
	unsigned int offset;
	unsigned long irq_flags;
	size_t left = len;
	int ret;

	if (panic_disk_store_io_aborted || ktime_after(ktime_get(), deadline))
		return -ETIMEDOUT;

	bio_init(&panic_disk_store_bio, panic_disk_store_vecs,
		 PANIC_DISK_STORE_IO_SIZE / PAGE_SIZE);
	bio_set_dev(&panic_disk_store_bio, panic_disk_store_bdev);
	bio_set_op_attrs(&panic_disk_store_bio, REQ_OP_WRITE,
			 op_flags | REQ_SYNC);
	panic_disk_store_bio.bi_iter.bi_sector = sector;
	panic_disk_store_bio.bi_end_io = panic_disk_store_end_io;

	while (left) {
		page = virt_to_page(buf + (len - left));
		offset = offset_in_page(buf + (len - left));
		ret = min_t(size_t, left, PAGE_SIZE - offset);
		if (bio_add_page(&panic_disk_store_bio, page, ret, offset) != ret)
			return -EIO;
		left -= ret;
	}

	panic_disk_store_io_status = BLK_STS_OK;
	atomic_set(&panic_disk_store_io_done, 0);
	local_irq_save(irq_flags);
	local_irq_enable();
	submit_bio(&panic_disk_store_bio);
	while (!atomic_read(&panic_disk_store_io_done) &&
	       !ktime_after(ktime_get(), deadline))
		cpu_relax();
	local_irq_restore(irq_flags);

	if (!atomic_read(&panic_disk_store_io_done)) {
		/* The block layer may still own this static BIO after timeout. */
		panic_disk_store_io_aborted = true;
		return -ETIMEDOUT;
	}
	smp_rmb();
	return panic_disk_store_io_status == BLK_STS_OK ? 0 : -EIO;
}

static int panic_disk_store_write_buffer(const char *buf, size_t len,
					 sector_t *sector, ktime_t deadline,
					 unsigned int op_flags, u32 *crc)
{
	size_t chunk;
	int ret;

	while (len) {
		chunk = min_t(size_t, len, PANIC_DISK_STORE_IO_SIZE);
		ret = panic_disk_store_write_chunk(buf, chunk, *sector,
						     deadline, op_flags);
		if (ret)
			return ret;
		if (crc)
			*crc = crc32_le(*crc, buf, chunk);
		*sector += chunk >> PANIC_DISK_STORE_SECTOR_SHIFT;
		buf += chunk;
		len -= chunk;
	}
	return 0;
}

static int panic_disk_store_write_log(ktime_t deadline, sector_t *sector,
					  u32 *size, u32 *crc)
{
	size_t offset = panic_disk_store_head;
	size_t left = panic_disk_store_used;
	size_t io_len = 0;
	int ret;

	while (left) {
		char c = panic_disk_store_ring[offset];

		panic_disk_store_ring_advance(&offset, 1);
		left--;
		if (!c)
			continue;
		panic_disk_store_io[io_len++] = c;
		if (io_len != PANIC_DISK_STORE_IO_SIZE)
			continue;
		ret = panic_disk_store_write_buffer(panic_disk_store_io, io_len,
							     sector, deadline, 0, crc);
		if (ret)
			return ret;
		*size += io_len;
		io_len = 0;
	}

	memcpy(panic_disk_store_io + io_len, PANIC_DISK_STORE_DONE,
	       sizeof(PANIC_DISK_STORE_DONE) - 1);
	io_len += sizeof(PANIC_DISK_STORE_DONE) - 1;
	/* Block BIOs must cover whole sectors; padding is excluded from the log. */
	memset(panic_disk_store_io + io_len, 0,
	       ALIGN(io_len, 1 << PANIC_DISK_STORE_SECTOR_SHIFT) - io_len);
	ret = panic_disk_store_write_buffer(panic_disk_store_io,
					     ALIGN(io_len,
						   1 << PANIC_DISK_STORE_SECTOR_SHIFT),
					     sector, deadline, 0, NULL);
	if (!ret)
		*size += io_len;
	if (!ret)
		*crc = crc32_le(*crc, panic_disk_store_io, io_len);
	return ret;
}

static int panic_disk_store_write_header(u32 status, u32 size, u32 crc,
					 unsigned int op_flags, ktime_t deadline)
{
	struct panic_disk_store_header *header =
		(struct panic_disk_store_header *)panic_disk_store_header_page;

	memset(panic_disk_store_header_page, 0, sizeof(panic_disk_store_header_page));
	memcpy(header->magic, PANIC_DISK_STORE_MAGIC, sizeof(header->magic));
	header->version = cpu_to_le32(1);
	header->header_size = cpu_to_le32(PANIC_DISK_STORE_HEADER_SIZE);
	header->status = cpu_to_le32(status);
	header->timestamp = cpu_to_le64(ktime_get_real_seconds());
	header->log_size = cpu_to_le32(size);
	header->log_crc32 = cpu_to_le32(crc);
	header->header_crc32 = cpu_to_le32(crc32_le(~0U,
						       (unsigned char *)header,
						       sizeof(*header)) ^ ~0U);

	return panic_disk_store_write_chunk(panic_disk_store_header_page,
		PANIC_DISK_STORE_HEADER_SIZE, 0, deadline, op_flags);
}

void panic_disk_store_write(void)
{
	ktime_t deadline;
	sector_t sector = PANIC_DISK_STORE_HEADER_SIZE >>
		PANIC_DISK_STORE_SECTOR_SHIFT;
	u32 size = 0;
	u32 crc = ~0U;

	if (!panic_disk_store_captured || !panic_disk_store_bdev)
		return;

	deadline = ktime_add_ms(ktime_get(), PANIC_DISK_STORE_IO_TIMEOUT_MS);
	/* Invalidate a prior COMPLETE record before replacing its payload. */
	if (panic_disk_store_write_header(PANIC_DISK_STORE_STATUS_WRITING,
					  0, 0, REQ_FUA, deadline))
		return;
	if (panic_disk_store_write_log(deadline, &sector, &size, &crc))
		return;

	panic_disk_store_write_header(PANIC_DISK_STORE_STATUS_COMPLETE, size,
				      crc ^ ~0U, REQ_PREFLUSH | REQ_FUA, deadline);
}

static struct kmsg_dumper panic_disk_store_dumper = {
	.dump = panic_disk_store_capture,
	.max_reason = KMSG_DUMP_PANIC,
};

static int __init panic_disk_store_init(void)
{
	if (PANIC_DISK_STORE_HEADER_SIZE + PANIC_DISK_STORE_MAX_LOG_SIZE >
	    PANIC_DISK_STORE_PARTITION_SIZE)
		return -EINVAL;

	panic_disk_store_bdev = blkdev_get_by_path(PANIC_DISK_STORE_BDEV_PATH,
		FMODE_READ | FMODE_WRITE, &panic_disk_store_bdev);
	if (IS_ERR(panic_disk_store_bdev)) {
		panic_disk_store_bdev = NULL;
		return 0;
	}
	return kmsg_dump_register(&panic_disk_store_dumper);
}
late_initcall(panic_disk_store_init);

#endif /* CONFIG_PANIC_DISK_STORE */
