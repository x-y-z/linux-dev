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
	.srcu_head.func = srcu_mig_cb,
	.owner = NULL,
};

int start_offloading(struct migrator *m)
{
	int offloading = 0;
	int ret;

	pr_info("starting migration offload by %s\n", m->name);
	ret = offc_update_migrator(m);
	if (ret < 0) {
		pr_err("failed to start migration offload by %s, err=%d\n",
		       m->name, ret);
		return ret;
	}
	atomic_try_cmpxchg(&dispatch_to_offc, &offloading, 1);
	return 0;
}
EXPORT_SYMBOL_GPL(start_offloading);

int stop_offloading(void)
{
	int offloading = 1;
	int ret;

	pr_info("stopping migration offload by %s\n", migrator.name);
	ret = offc_update_migrator(NULL);
	if (ret < 0) {
		pr_err("failed to stop migration offload by %s, err=%d\n",
		       migrator.name, ret);
		return ret;
	}
	atomic_try_cmpxchg(&dispatch_to_offc, &offloading, 0);
	return 0;
}
EXPORT_SYMBOL_GPL(stop_offloading);

unsigned char *get_active_migrator_name(void)
{
	return migrator.name;
}
EXPORT_SYMBOL_GPL(get_active_migrator_name);
