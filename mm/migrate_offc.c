// SPDX-License-Identifier: GPL-2.0
#include <linux/migrate.h>
#include <linux/migrate_offc.h>
#include <linux/rculist.h>
#include <linux/static_call.h>

atomic_t dispatch_to_offc = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(dispatch_to_offc);

DEFINE_MUTEX(migrator_mut);
DEFINE_SRCU(mig_srcu);

struct migrator migrator = {
	.name = "kernel",
	.migrate_offc = folios_mc_copy,
	.can_migrate_offc = can_offc_migrate,
	.srcu_head.func = srcu_mig_cb,
	.owner = NULL,
};

bool can_offc_migrate(struct folio *dst, struct folio *src)
{
	return true;
}
EXPORT_SYMBOL_GPL(can_offc_migrate);

void start_offloading(struct migrator *m)
{
	int offloading = 0;

	pr_info("starting migration offload by %s\n", m->name);
	offc_update_migrator(m);
	atomic_try_cmpxchg(&dispatch_to_offc, &offloading, 1);
}
EXPORT_SYMBOL_GPL(start_offloading);

void stop_offloading(void)
{
	int offloading = 1;

	pr_info("stopping migration offload by %s\n", migrator.name);
	offc_update_migrator(NULL);
	atomic_try_cmpxchg(&dispatch_to_offc, &offloading, 0);
}
EXPORT_SYMBOL_GPL(stop_offloading);

unsigned char *get_active_migrator_name(void)
{
	return migrator.name;
}
EXPORT_SYMBOL_GPL(get_active_migrator_name);
