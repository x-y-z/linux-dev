// SPDX-License-Identifier: GPL-2.0
/*
 * Parallel page copy routine.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/highmem.h>
#include <linux/padata.h>
#include <linux/slab.h>
#include <linux/migrate.h>
#include <linux/migrate_offc.h>

#define MAX_NUM_COPY_THREADS 64

unsigned int limit_mt_num = 4;
static int is_dispatching;

static int copy_page_lists_mt(struct list_head *dst_folios,
		struct list_head *src_folios, int nr_items);
static bool can_migrate_mt(struct folio *dst, struct folio *src);

static DEFINE_MUTEX(migratecfg_mutex);

/* CPU Multithreaded Batch Migrator */
struct migrator cpu_migrator = {
	.name = "CPU_MT_COPY\0",
	.migrate_offc = copy_page_lists_mt,
	.can_migrate_offc = can_migrate_mt,
	.owner = THIS_MODULE,
};

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	int ret;
	unsigned long num_items;
	struct copy_item item_list[];
};

static unsigned long copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	return copy_mc_to_kernel(vto, vfrom, chunk_size);
}

static void copy_page_work_thread(unsigned long start, unsigned long end, void *arg)
{
	unsigned long i, j;
	struct copy_page_info **work_item = (struct copy_page_info**)arg;

	for (i = start; i < end; i++)
		for (j = 0; j < work_item[i]->num_items; j++)
			work_item[i]->ret |= !!copy_page_routine(
				work_item[i]->item_list[j].to,
				work_item[i]->item_list[j].from,
				work_item[i]->item_list[j].chunk_size);
}

static ssize_t mt_offloading_set(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	int ccode;
	int action;

	ccode = kstrtoint(buf, 0, &action);
	if (ccode) {
		pr_debug("(%s:) error parsing input %s\n", __func__, buf);
		return count;
	}

	/*
	 * action is 0: User wants to disable MT offloading.
	 * action is 1: User wants to enable MT offloading.
	 */
	switch (action) {
	case 0:
		mutex_lock(&migratecfg_mutex);
		if (is_dispatching == 1) {
			stop_offloading();
			is_dispatching = 0;
		} else
			pr_debug("MT migration offloading is already OFF\n");
		mutex_unlock(&migratecfg_mutex);
		break;
	case 1:
		mutex_lock(&migratecfg_mutex);
		if (is_dispatching == 0) {
			start_offloading(&cpu_migrator);
			is_dispatching = 1;
		} else
			pr_debug("MT migration offloading is already ON\n");
		mutex_unlock(&migratecfg_mutex);
		break;
	default:
		pr_debug("input should be zero or one, parsed as %d\n", action);
	}
	return count;
}

static ssize_t mt_offloading_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", is_dispatching);
}

static ssize_t mt_threads_set(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	int ccode;
	unsigned int threads;

	ccode = kstrtouint(buf, 0, &threads);
	if (ccode) {
		pr_debug("(%s:) error parsing input %s\n", __func__, buf);
		return ccode;
	}

	if (threads > 0 && threads <= MAX_NUM_COPY_THREADS) {
		mutex_lock(&migratecfg_mutex);
		limit_mt_num = threads;
		mutex_unlock(&migratecfg_mutex);
		pr_debug("MT threads set to %u\n", limit_mt_num);
	} else {
		pr_debug("Invalid thread count. Must be between 1 and %d\n",MAX_NUM_COPY_THREADS);
		return -EINVAL;
	}

	return count;
}

static ssize_t mt_threads_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", limit_mt_num);
}

static bool can_migrate_mt(struct folio *dst, struct folio *src)
{
	return true;
}

int copy_page_lists_mt(struct list_head *dst_folios,
		struct list_head *src_folios, int nr_items)
{
	struct copy_page_info *work_items[MAX_NUM_COPY_THREADS] = {0};
	unsigned int total_mt_num = limit_mt_num;
	struct folio *src, *src2, *dst, *dst2;
	struct padata_mt_job job = {
		.thread_fn = copy_page_work_thread,
		.fn_arg = &work_items,
		.start = 0,
		.align = 1,
		.min_chunk = 1,
		.numa_aware = true,
	};
	int max_items_per_thread;
	int item_idx;
	int err = 0;
	int cpu;
	int i;

	if (IS_ENABLED(CONFIG_HIGHMEM))
		return -ENOTSUPP;

	if (total_mt_num > MAX_NUM_COPY_THREADS)
		total_mt_num = MAX_NUM_COPY_THREADS;

	/* Each threads get part of each page, if nr_items < totla_mt_num */
	if (nr_items < total_mt_num)
		max_items_per_thread = nr_items;
	else
		max_items_per_thread = (nr_items / total_mt_num) +
				((nr_items % total_mt_num) ? 1 : 0);


	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info) +
						sizeof(struct copy_item) *
							max_items_per_thread,
					  GFP_NOWAIT);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	job.max_threads = total_mt_num;
	job.size = total_mt_num;

	if (nr_items < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu)
			work_items[cpu]->num_items = max_items_per_thread;

		item_idx = 0;
		dst = list_first_entry(dst_folios, struct folio, lru);
		dst2 = list_next_entry(dst, lru);
		list_for_each_entry_safe(src, src2, src_folios, lru) {
			unsigned long chunk_size = PAGE_SIZE * folio_nr_pages(src) / total_mt_num;
			char *vfrom = page_address(&src->page);
			char *vto = page_address(&dst->page);

			VM_WARN_ON(PAGE_SIZE * folio_nr_pages(src) % total_mt_num);
			VM_WARN_ON(folio_nr_pages(dst) != folio_nr_pages(src));

			for (cpu = 0; cpu < total_mt_num; ++cpu) {
				work_items[cpu]->item_list[item_idx].to =
					vto + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].from =
					vfrom + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].chunk_size =
					chunk_size;
			}

			item_idx++;
			dst = dst2;
			dst2 = list_next_entry(dst, lru);
		}
	} else {
		int num_xfer_per_thread = nr_items / total_mt_num;
		int per_cpu_item_idx;

		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			work_items[cpu]->num_items = num_xfer_per_thread +
					(cpu < (nr_items % total_mt_num));
		}

		cpu = 0;
		per_cpu_item_idx = 0;
		item_idx = 0;
		dst = list_first_entry(dst_folios, struct folio, lru);
		dst2 = list_next_entry(dst, lru);
		list_for_each_entry_safe(src, src2, src_folios, lru) {
			work_items[cpu]->item_list[per_cpu_item_idx].to =
				page_address(&dst->page);
			work_items[cpu]->item_list[per_cpu_item_idx].from =
				page_address(&src->page);
			work_items[cpu]->item_list[per_cpu_item_idx].chunk_size =
				PAGE_SIZE * folio_nr_pages(src);

			VM_WARN_ON(folio_nr_pages(dst) !=
				   folio_nr_pages(src));

			per_cpu_item_idx++;
			item_idx++;
			dst = dst2;
			dst2 = list_next_entry(dst, lru);

			if (per_cpu_item_idx == work_items[cpu]->num_items) {
				per_cpu_item_idx = 0;
				cpu++;
			}
		}
		if (item_idx != nr_items)
			pr_warn("%s: only %d out of %d pages are transferred\n",
				__func__, item_idx - 1, nr_items);

	}

	padata_do_multithreaded(&job);

	for (i = 0; i < total_mt_num; ++i) {
		/* retry if any copy fails */
		if (work_items[i]->ret)
			err = -EAGAIN;
	}

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		kfree(work_items[cpu]);

	return err;
}

static struct kobject *mt_kobj_ref;
static struct kobj_attribute mt_offloading_attribute = __ATTR(offloading, 0664,
		mt_offloading_show, mt_offloading_set);
static struct kobj_attribute mt_threads_attribute = __ATTR(threads, 0664,
		mt_threads_show, mt_threads_set);

static int __init cpu_mt_module_init(void)
{
	int ret = 0;

	mt_kobj_ref = kobject_create_and_add("cpu_mt_copy", kernel_kobj);
	if (!mt_kobj_ref)
		return -ENOMEM;

	ret = sysfs_create_file(mt_kobj_ref, &mt_offloading_attribute.attr);
	if (ret)
		goto out_offloading;

	ret = sysfs_create_file(mt_kobj_ref, &mt_threads_attribute.attr);
	if (ret)
		goto out_threads;

	is_dispatching = 0;

	return 0;

out_threads:
	sysfs_remove_file(mt_kobj_ref, &mt_offloading_attribute.attr);
out_offloading:
	kobject_put(mt_kobj_ref);
	return ret;
}

static void __exit cpu_mt_module_exit(void)
{
	/* Stop the MT offloading to unload the module */
	mutex_lock(&migratecfg_mutex);
	if (is_dispatching == 1) {
		stop_offloading();
		is_dispatching = 0;
	}
	mutex_unlock(&migratecfg_mutex);

	sysfs_remove_file(mt_kobj_ref, &mt_threads_attribute.attr);
	sysfs_remove_file(mt_kobj_ref, &mt_offloading_attribute.attr);
	kobject_put(mt_kobj_ref);
}

module_init(cpu_mt_module_init);
module_exit(cpu_mt_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zi Yan");
MODULE_DESCRIPTION("CPU_MT_COPY"); /* CPU Multithreaded Batch Migrator */
