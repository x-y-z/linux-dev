/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _MIGRATE_OFFC_H
#define _MIGRATE_OFFC_H
#include <linux/migrate_mode.h>

#define MIGRATOR_NAME_LEN 32
struct migrator {
	char name[MIGRATOR_NAME_LEN];
	int (*migrate_offc)(struct list_head *dst_list, struct list_head *src_list, int folio_cnt);
	bool (*can_migrate_offc)(struct folio *dst, struct folio *src);
	struct rcu_head srcu_head;
	struct module *owner;
};

extern struct migrator migrator;
extern struct mutex migrator_mut;
extern struct srcu_struct mig_srcu;

#ifdef CONFIG_OFFC_MIGRATION
void srcu_mig_cb(struct rcu_head *head);
void offc_update_migrator(struct migrator *mig);
unsigned char *get_active_migrator_name(void);
bool can_offc_migrate(struct folio *dst, struct folio *src);
void start_offloading(struct migrator *migrator);
void stop_offloading(void);
#else
static inline void srcu_mig_cb(struct rcu_head *head) { };
static inline void offc_update_migrator(struct migrator *mig) { };
static inline unsigned char *get_active_migrator_name(void) { return NULL; };
static inline bool can_offc_migrate(struct folio *dst, struct folio *src) {return true; };
static inline void start_offloading(struct migrator *migrator) { };
static inline void stop_offloading(void) { };
#endif /* CONFIG_OFFC_MIGRATION */

#endif /* _MIGRATE_OFFC_H */
