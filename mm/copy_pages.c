// SPDX-License-Identifier: GPL-2.0
/*
 * Parallel page copy routine.
 */

#include "linux/errno.h"
#include "linux/kconfig.h"
#include <linux/sysctl.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/migrate.h>


unsigned int limit_mt_num = 4;

#define MAX_NUM_COPY_THREADS 64

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	struct work_struct copy_page_work;
	int ret;
	unsigned long num_items;
	struct copy_item item_list[];
};

static unsigned long copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	return copy_mc_to_kernel(vto, vfrom, chunk_size);
}

static void copy_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info *)work;
	int i;

	my_work->ret = 0;
	for (i = 0; i < my_work->num_items; ++i)
		my_work->ret |= !!copy_page_routine(my_work->item_list[i].to,
					my_work->item_list[i].from,
					my_work->item_list[i].chunk_size);
}

int copy_page_lists_mt(struct list_head *dst_folios,
		struct list_head *src_folios, int nr_items)
{
	struct copy_page_info *work_items[MAX_NUM_COPY_THREADS] = {0};
	int cpu_id_list[MAX_NUM_COPY_THREADS] = {0};
	unsigned int total_mt_num = limit_mt_num;
	const struct cpumask *copy_node_cpumask;
	struct folio *src, *src2, *dst, *dst2;
	int max_items_per_thread;
	/* src node, dst node, current node */
	int copy_nodes[3];
	int item_idx;
	int err = 0;
	int cpu;
	int i;

	if (IS_ENABLED(CONFIG_HIGHMEM))
		return -ENOTSUPP;

	copy_nodes[0] = folio_nid(list_first_entry(src_folios, struct folio, lru));
	copy_nodes[1] = folio_nid(list_first_entry(dst_folios, struct folio, lru));
	copy_nodes[2] = numa_node_id();

	/* try to find a node to perform copy job */
	for (i = 0; i < 3; i++) {
		copy_node_cpumask = cpumask_of_node(copy_nodes[i]);
		if (cpumask_weight(copy_node_cpumask))
			break;
	}

	total_mt_num = min_t(unsigned int, total_mt_num,
			cpumask_weight(copy_node_cpumask));

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

	/* TODO: need a better cpu selection method */
	i = 0;
	for_each_cpu(cpu, copy_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	if (nr_items < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);
			work_items[cpu]->num_items = max_items_per_thread;
		}

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

		for (cpu = 0; cpu < total_mt_num; ++cpu)
			queue_work_on(cpu_id_list[cpu],
						  system_unbound_wq,
						  (struct work_struct *)work_items[cpu]);
	} else {
		int num_xfer_per_thread = nr_items / total_mt_num;
		int per_cpu_item_idx;


		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);

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
				queue_work_on(cpu_id_list[cpu],
					system_unbound_wq,
					(struct work_struct *)work_items[cpu]);
				per_cpu_item_idx = 0;
				cpu++;
			}
		}
		if (item_idx != nr_items)
			pr_warn("%s: only %d out of %d pages are transferred\n",
				__func__, item_idx - 1, nr_items);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i) {
		flush_work((struct work_struct *)work_items[i]);
		/* retry if any copy fails */
		if (work_items[i]->ret)
			err = -EAGAIN;
	}

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		kfree(work_items[cpu]);

	return err;
}
