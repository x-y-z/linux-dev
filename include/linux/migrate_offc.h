/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _MIGRATE_OFFC_H
#define _MIGRATE_OFFC_H
#include <linux/migrate_mode.h>

#define MIGRATOR_NAME_LEN 32
struct migrator {
	char name[MIGRATOR_NAME_LEN];
	int (*migrate_offc)(struct list_head *dst_list, struct list_head *src_list,
			    unsigned int folio_cnt);
	struct rcu_head srcu_head;
	struct module *owner;
};

extern struct migrator migrator;
extern struct mutex migrator_mut;
extern struct srcu_struct mig_srcu;

#ifdef CONFIG_OFFC_MIGRATION
void srcu_mig_cb(struct rcu_head *head);
int offc_update_migrator(struct migrator *mig);
unsigned char *get_active_migrator_name(void);
int start_offloading(struct migrator *migrator);
int stop_offloading(void);
#else
static inline void srcu_mig_cb(struct rcu_head *head) { };
static inline int offc_update_migrator(struct migrator *mig) { return 0; };
static inline unsigned char *get_active_migrator_name(void) { return NULL; };
static inline void start_offloading(struct migrator *migrator) { return 0; };
static inline void stop_offloading(void) { return 0; };
#endif /* CONFIG_OFFC_MIGRATION */

#endif /* _MIGRATE_OFFC_H */
