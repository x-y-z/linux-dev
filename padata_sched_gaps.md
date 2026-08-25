# Scheduler Gaps for padata CPU-Time Attribution

Companion to `mtcopy_scheduling_plan.md` (AC-4 and its "The Enforcement Gap" section). That plan states the gaps tersely for a reader who already knows the scheduler. This document explains each one from scratch, for a kernel developer who does not work in `kernel/sched/`.

The problem being solved throughout: a thread calls `migrate_pages()`; padata farms the page-copy work out to helper kworkers; those helpers burn real CPU on that thread's behalf; and today none of that CPU time reaches the initiating thread's cgroup. The cost disappears from the **initiator's** `cpu.stat` and escapes its `cpu.max`. (It is not silently moved to root's `cpu.stat` either — root's figure is computed from global kcpustat on read, not from the rstat tree; see Gap 2.)

---

## Gap 1: Association

### The one struct you need

`struct rq` is the per-CPU runqueue — the scheduler's per-CPU anchor, structurally analogous to what `pglist_data` is to a node in mm. Two of its fields matter here:

- **`rq->curr`** — the task physically executing on this CPU right now.
- **`rq->donor`** — the task whose *scheduling context* is being used.

"Scheduling context" is the bundle the scheduler budgets and bills against: weight (priority), cgroup membership, accumulated fairness credit (vruntime). It answers *"how long do I get to run, and who pays for it?"*

In the ordinary case `curr == donor`: you run, you are billed, done.

### What we want

We want the helper kworker's CPU time billed to the initiating thread's cgroup — i.e. we want `curr` (the helper) and `donor` (the initiator) to differ, with accounting following `donor`.

### The good news: the concept already exists

`rq->curr != rq->donor` is a real, supported state. It exists to solve **priority inversion**.

The classic scenario: high-priority task H blocks on a mutex held by low-priority task L. A medium-priority task M then preempts L. H — the most important task on the machine — is now stuck indefinitely behind M, because L cannot get scheduled to release the lock.

The traditional fix is priority inheritance: temporarily boost L to H's priority. Linux's newer and more general answer is **proxy execution**: when the scheduler picks H and finds H blocked, it runs **L instead, on H's scheduling context**. L borrows H's turn, H's weight, H's cgroup.

That relationship is recorded as `donor = H` (picked, pays) and `curr = L` (physically running).

Crucially, **everything downstream already honours it**. `update_se()` (`kernel/sched/fair.c:1355-1394`) sends per-task runtime to the runner but routes cgroup CPU accounting to the donor, under an in-tree comment reading *"cgroup time is always accounted against the donor."*

### The bad news: there is no door to it

This is the only place `rq->donor` is ever set (`kernel/sched/core.c:7148-7156`):

```c
next = pick_next_task(rq, &rf);          /* scheduler picks someone */
if (sched_proxy_exec()) {
        rq_set_donor(rq, next);          /* the picked task is the donor */
        if (unlikely(next->is_blocked))  /* ...but it is stuck on a mutex? */
                next = find_proxy_task(rq, next, &rf);  /* run the lock owner instead */
```

It is set *inside the scheduler's own task-picking path*, and only when the picked task turns out to be blocked on a mutex. `rq_set_donor()` itself lives in the private `kernel/sched/sched.h` (`:1444`) and is not exported. No subsystem can declare the relationship.

And padata does not fit the trigger regardless:

- The initiator is not blocked on a mutex — it waits on a `completion`, a different mechanism.
- The helper is not "the one thing standing between the initiator and progress" — there are N of them.
- The initiator is *also running* (padata's caller-participation), so it is not blocked in the sense proxy execution means.

### Gap 1, stated

> The kernel has a first-class notion of "task A is running on behalf of task B," and all downstream accounting already bills correctly when it is set — but the only way to establish it is *"the scheduler picked you and you turned out to be blocked on a mutex,"* and there is no API for anything else to declare it.

Nothing that relies on the *scheduler* knowing the relationship can be attempted until it can be told. (Gap 2 is the exception that proves the rule: cgroup statistics can be fixed without ever closing Gap 1, precisely because statistics carry no obligations — see below.)

---

## Gap 1: questions explored

Four questions about whether proxy execution can be bent to serve padata. The short answer is no, and the reason turns out to be the most useful finding in this document — it tells us what to ask for instead.

### Q1. Can we hack it by setting the issuing thread's `->is_blocked` so padata kthreads get charged to it?

**No — `is_blocked` is only a flag; the real association lives in a mutex pointer.**

`p->is_blocked` is a `u8` (`include/linux/sched.h:857`), but it merely gates entry into `find_proxy_task()`, which immediately does the real work (`kernel/sched/core.c:6880-6913`):

```c
for (p = donor; p->is_blocked; p = owner) {
        struct mutex *mutex = p->blocked_on;
        if (!mutex) {
                clear_task_blocked_on(p, mutex);
                ...
        }
        ...
        owner = __mutex_owner(mutex);
```

`p->blocked_on` is typed `struct mutex *` (`include/linux/sched.h:1252`) — it cannot point at a task. The runner is *derived* by dereferencing the mutex and reading its real owner field. Setting `is_blocked = 1` with `blocked_on = NULL` simply takes the `if (!mutex)` path, which clears the flag and gives up.

Making this "work" would require a genuine mutex that a helper genuinely owns and the initiator genuinely blocks on. That is constructible — a per-job mutex — but it buys nothing, because a mutex has exactly one owner at a time, which lands straight in Q4's problem. It would also lie to lockdep and to the deadlock detector about a resource dependency that does not exist.

### Q2. How do we set a padata kthread's donor to the issuing thread?

**We cannot, from outside the scheduler — and forcing it would corrupt the accounting we are trying to fix.**

Two obstacles, the second more fundamental than the first:

1. `rq_set_donor()` is a static inline in the private `kernel/sched/sched.h` and is called only from `__schedule()`. That is merely an export problem.
2. The donor is, by construction, *the task `pick_next_task()` returned*. A task can only be picked on the CPU where its `sched_entity` is enqueued. Writing `rq->donor = initiator` on some other CPU would leave every downstream consumer inconsistent: `update_curr()` operates on `cfs_rq->curr`, which is established during the pick; `task_tick_fair()` walks the donor's cgroup hierarchy; vruntime updates would be applied to an entity that is not on that runqueue. The initiator's single entity would be accounted on several CPUs at once — a *worse* accounting bug than the one being fixed.

### Q3. Would `if (unlikely(next->is_blocked))` need to become `if (next->is_blocked || <padata marker>)`?

**No — that hook is in the wrong place and asks the wrong question, because it redirects the wrong thing.**

The distinction that matters:

- Proxy execution redirects **execution**: *"the task I picked cannot run, so run this other task using its context."* The question is asked about the **donor**, at pick time.
- padata needs to redirect **accounting**: *"this runner is legitimately running in its own right, but bill someone else."* The question needs to be asked about the **runner**, at charge time.

Proxy execution bundles the two only because its exclusivity makes them equivalent: the runner has no independent right to run, so whoever supplied the execution slot is necessarily whoever should be billed. That equivalence does not hold for a padata helper, which is an ordinary runnable kworker that the scheduler would have run anyway.

So the correct hook is not in the pick path at all. It belongs in the charging path — "when accounting this runner's `delta_exec`, is a charge context active on it?" — which is precisely the `cpu_charge_begin()`/`cpu_charge_end()` primitive proposed in `mtcopy_scheduling_plan.md`'s "The Enforcement Gap" section.

### Q4. Does current scheduler code support multiple runners sharing one donor?

**No, and this is the decisive answer: proxy execution is 1:1 *on a single CPU*, and it actively collapses work onto that CPU — the exact opposite of what mtcopy wants.**

Three independent confirmations in the code:

1. **The chain is linear, not a fan-out.** `find_proxy_task()` walks `for (p = donor; p->is_blocked; p = owner)` — one successor per step (donor → mutex → owner → possibly another mutex → …). The back-link `owner->blocked_donor = p` (`kernel/sched/core.c:7001`) is documented as *"a back link for the current donor **stack**"* — a chain, not a set.
2. **`rq->donor` is one field per CPU.** For N helpers on N CPUs to run on the initiator's behalf, all N runqueues would need `rq->donor == initiator`. But the initiator is one task with one `sched_entity` enqueued on one `cfs_rq`, so it can be picked — and therefore be donor — on at most one CPU at a time.
3. **The design deliberately serializes onto one runqueue.** When the mutex owner is on a different CPU than the donor, `find_proxy_task()` does not run them in parallel — it migrates **the blocked donor to the owner's CPU** (`proxy_migrate_task(rq, rf, p, owner_cpu)`, whose comment reads *"Since we are migrating a blocked donor…"*) and returns `NULL` so `__schedule()` picks again. The waiter chases the owner, not the reverse. Either way the invariant is the same: **donor and runner must end up on the same runqueue.**

The consequence is worth stating plainly: proxy execution is a mechanism for **redirecting one CPU's worth of execution, not for multiplying it**. Its premise is that the donor is blocked and therefore not consuming CPU, so its slot is free to lend. Building mtcopy on it would cap throughput at one runner — i.e. exactly the single-threaded copy we started with. The parallelism, which is the entire point, would be gone.

### What these four answers establish

Proxy execution cannot be borrowed, extended, or tricked into serving this case. But working out *why* pins down what to ask for instead:

- The mechanism we need redirects **billing**, not **execution** (Q3).
- It must tolerate **N concurrent runners** for one payer, and a payer that is **itself running** (Q4).
- It cannot be expressed through mutex ownership, so it needs its own explicit, caller-driven association (Q1, Q2).

Those three constraints are exactly the shape of the `cpu_charge_begin()`/`cpu_charge_end()` primitive proposed in the plan — and the reason it is proposed as *new* rather than as a reuse of proxy execution.

---

## Deep dive: how proxy execution works

Background for Gap 1, and the basis for the one experiment proxy execution *does* enable.

### The insight

**A blocked task's turn on the CPU is going to be wasted anyway.** So rather than discard it, lend it to the one task whose progress the blocked task actually needs.

Think of it as a **queue ticket**. H holds a ticket for CPU time — its place in line, its weight, its cgroup, its accumulated fairness credit. H cannot use it, because it is stuck waiting on L. So when H's number comes up, H hands the ticket to L. L runs *on H's ticket*, and the billing goes to H's account — which is correct, because L making progress is exactly what H is paying for.

### The implementation trick: one line of *not* doing something

Normally a blocked task is *dequeued* — removed from the runqueue, invisible to the scheduler until woken. Under proxy execution a mutex-blocked task **stays enqueued**, keeping its full weight and fairness credit, still selectable by `pick_next_task()` (`kernel/sched/core.c:6713-6725`):

```c
p->is_blocked = 1;
/*
 * ... if should_block is false, its likely due to the task being
 * blocked on a mutex, and we want to keep it on the runqueue
 * to be selectable for proxy-execution.
 */
if (!should_block)
        return false;      /* <-- do NOT dequeue */
block_task(rq, p, task_state);
```

That is the entire foundation. Everything else follows from "the blocked task is still in the queue."

### Lifecycle, end to end

1. **H blocks.** The `mutex_lock()` slow path records `__set_task_blocked_on(current, lock)` (`kernel/locking/mutex.c:692`).
2. **H stays on the runqueue** with `is_blocked = 1`, per the code above.
3. **The scheduler picks H** normally, later — as far as the runqueue is concerned, H is still there.
4. **H becomes the donor**: `rq_set_donor(rq, next)`, then `if (unlikely(next->is_blocked))` fires.
5. **`find_proxy_task()` walks the chain**: H → mutex → `__mutex_owner()` = L; if L is itself blocked on another mutex, keep walking. If L is on a different CPU, migrate **H to L's CPU** and return `NULL` so `__schedule()` picks again. Record the back-link `L->blocked_donor = H`.
6. **L runs.** `rq->curr = L`, `rq->donor = H`. `update_se()` bills cgroup time to H; `account_cfs_rq_runtime()` docks H's cgroup quota; if H's cgroup is throttled, H stops being picked and L — having no ticket of its own — stops with it.
7. **L unlocks** (`kernel/locking/mutex.c:1032-1051`): sees `blocked_donor == H`, confirms H is blocked on *this* lock, and hands the lock straight to H — *"the highest waiter, as selected by the scheduling function."* `proxy_needs_return()` (`kernel/sched/core.c:3765-3795`) sends H back to its original CPU if it was migrated.

Contrast with priority inheritance: PI copies a single number (priority). Proxy execution lends the *entire* scheduling context — priority, cgroup, fairness credit — which is why the accounting comes out right for free rather than needing separate machinery.

### The one experiment this enables

The initiator genuinely *is* blocked during `wait_for_completion()`, after draining its own chunks (INV-15 in the plan). Its slot really is free. So a **one-helper** configuration is legitimately expressible today, with **zero kernel changes**:

- a per-job `struct mutex M`;
- the helper acquires `M` before starting work;
- the initiator calls `mutex_lock(&M)` instead of `wait_for_completion()`;
- proxy execution does the rest — the helper runs on the initiator's context, billed to its cgroup, subject to its quota;
- the helper unlocks, and `M` is handed back to the initiator.

This is not a production design — it caps throughput at one helper, i.e. single-threaded copy with extra steps. Its value is entirely as **evidence**:

> When the relationship is expressible, the kernel already accounts and enforces it perfectly, with no new code. Here is the measurement proving it. What is missing is exactly one thing: a way to express the relationship for N runners with a payer that is itself running.

That reframes the upstream ask from *"please build us a mechanism"* to *"please extend the expressiveness of a mechanism you already have and already trust"* — a materially easier request.

### Two constraints from the Kconfig

```
config SCHED_PROXY_EXEC
	bool "Proxy Execution"
	depends on !PREEMPT_RT
	# Need to investigate how to inform sched_ext of split contexts
	depends on !SCHED_CLASS_EXT
	# Not particularly useful until we get to multi-rq proxying
	depends on EXPERT
```

**`depends on !SCHED_CLASS_EXT` — proxy execution and sched_ext are mutually exclusive.** The proxy-exec experiment above and the sched_ext demonstrator (plan AC-4(c)) **cannot run in the same kernel build**. They are two separate builds and two separate experiments. This affects task sequencing in `mtcopy_scheduling_plan.md` and should be settled before either is scheduled.

**`depends on EXPERT`, "Not particularly useful until we get to multi-rq proxying"** — see below. Note also that proxy execution is not enabled in distro kernels; it needs an EXPERT build. When compiled in it is on by default at runtime (`DEFINE_STATIC_KEY_TRUE(__sched_proxy_exec)`), disableable with `sched_proxy_exec=0`.

### What "multi-rq" means, and why it matters here

"rq" is `struct rq`, the **per-CPU runqueue**. So *multi-rq proxying* means proxying **across runqueues** — donor parked on CPU A's runqueue while the runner executes on CPU B's, with the scheduling context applied across that boundary.

Today proxy execution is **single-rq**: donor and runner must end up on the *same* runqueue. When they start apart, `find_proxy_task()` resolves it by migrating the blocked donor to the owner's CPU rather than by proxying across the gap. That is why the maintainers judge it "not particularly useful" yet — every cross-CPU mutex contention costs a migration (cache and TLB damage, plus serialization), which can outweigh the priority-inversion benefit it buys.

Lifting the restriction is hard for concrete reasons: `rq->donor` and `rq->curr` are per-rq fields whose pairing currently lives wholly inside one rq; `cfs_rq->curr` is established during the pick on that rq and `update_curr()` walks *that* rq's hierarchy; and cross-rq operation needs either both rq locks or a careful lockless protocol, with load balancing and the fairness math to match.

For padata, multi-rq proxying is **necessary but not sufficient**:

- **Necessary** — N runners can never occupy N CPUs while the design forces donor and runner onto one runqueue. Multi-rq removes that hard blocker.
- **Not sufficient** — even multi-rq proxying remains **1 donor : 1 runner**: `rq->donor` is a single field per rq, and the blocked-on chain is linear (Q4). padata needs N runqueues to name the *same* donor concurrently, which additionally raises a fairness-model question, not just plumbing: the donor's single `sched_entity` would be in use on N CPUs at once, effectively multiplying its weight and racing vruntime updates from N CPUs.

The strategic value is that the direction is already sanctioned. The scheduler maintainers have written into the Kconfig that the single-runqueue restriction is a known limitation they intend to lift. An RFC asking for N:1 is therefore not proposing an unwanted direction — it is proposing the natural step *after* one already on the roadmap, with a concrete workload motivating it.

---

## Gap 2: Native cgroup `cpu.stat`

### Where the numbers come from

`cpu.stat` is the cgroup-v2 file reporting a cgroup's CPU consumption:

```
usage_usec  12345678
user_usec    9876543
system_usec  2469135
nice_usec          0
```

The write side is about as simple as kernel accounting gets (`kernel/cgroup/rstat.c:629-637`):

```c
void __cgroup_account_cputime(struct cgroup *cgrp, u64 delta_exec)
{
	rstatbc = cgroup_base_stat_cputime_account_begin(cgrp, &flags);
	rstatbc->bstat.cputime.sum_exec_runtime += delta_exec;
	cgroup_base_stat_cputime_account_end(cgrp, rstatbc, flags);
}
```

A `+=` on a per-CPU counter, plus a "this cgroup is dirty" mark. Aggregation up the hierarchy is deferred until somebody actually reads the file (rstat's flush-on-read model), which is why the write side costs almost nothing and needs no global lock.

On the read side (`kernel/cgroup/rstat.c:730-750`), `usage_usec` is `sum_exec_runtime` directly; `user_usec` and `system_usec` are produced by `cputime_adjust()` — the same ratio-rescaling seen in INV-1, applied at cgroup level. So `sum_exec_runtime` is the authoritative total and the user/system split is derived from it.

### Why helper time is missing

Normally the scheduler calls `cgroup_account_cputime(task, delta)`, which derives the cgroup with `task_dfl_cgroup(task)` and forwards to the function above. For a padata helper, `task` is a kworker living in the **root cgroup** — and the wrapper then does nothing at all, because it guards the charge with `if (cgroup_parent(cgrp))` and the root cgroup has no parent. Root-cgroup tasks never enter the rstat tree; root's own `cpu.stat` is computed separately from global kcpustat on read (`kernel/cgroup/rstat.c:729-736`).

So the helper's time is not "charged to the wrong cgroup" so much as **charged nowhere in the rstat tree**, and the initiator's cgroup never sees it. That distinction matters: it is exactly why adding an explicit charge later cannot double-count (INV-29).

### Why this gap is the easy one

**Statistics are pure addition, and no decision depends on them.**

`cpu.stat` has no consumer inside the kernel. Nothing throttles, preempts, or load-balances based on `bstat.cputime.sum_exec_runtime`. CFS bandwidth enforcement uses an entirely separate counter — `cfs_rq->runtime_remaining` in `struct cfs_rq` — which is why Gap 3 is hard and this one is not.

So there is no consistency invariant to preserve, no lock ordering against the scheduler, and no risk of confusing anything by adding a number. And `__cgroup_account_cputime()` already takes a **cgroup pointer**, not a task, so an arbitrary cgroup can be charged directly.

The fix is therefore a padata-side change with no scheduler involvement at all (plan AC-4(a)): capture the initiator's cgroup at job submission — *not* inside the helper, where `current` is the kworker and the answer would be the root cgroup — then charge each helper's measured exec-time delta to it. This is the one part of the whole problem that is independently mergeable today.

Three details make the difference between a sketch and working code, all covered in the plan:

- **Capture it RCU-safely.** `task->cgroups` is RCU-protected and `css_get()` requires the caller to *already* hold a reference, so a bare `cgroup_get(task_dfl_cgroup(current))` is wrong. Use the in-tree `rcu_read_lock()` + `cgroup_tryget(cset->dfl_cgrp)` retry loop from `cgroup_sk_alloc()` (INV-34).
- **Charge both fields.** `__cgroup_account_cputime()` alone gets `usage_usec` right but lets `cputime_adjust()` smear the added kernel time across `user_usec`/`system_usec` in the cgroup's pre-existing ratio. Mirror the scheduler and also call `__cgroup_account_cputime_field(cgrp, CPUTIME_SYSTEM, delta)` (INV-35).
- **Charge before signalling completion**, or the last helper's `complete()` lets the initiator drop the reference out from under a charge still in flight.

And the assumption the no-double-count argument rests on is stronger than it first appears: workqueue workers carry `PF_NO_SETAFFINITY`, and cgroup migration explicitly refuses such tasks with `-EINVAL`, so padata's kworkers **cannot** be moved out of root (INV-33).

### The trap next door: the same trick does *not* work per-task

`/proc/<pid>/stat`, `top`, `ps` and `getrusage()` are also "just statistics," and the natural instinct is to fix them the same way. That instinct is wrong, and the reason is worth internalizing because the two operations look identical.

For a task, the equivalent lever is `p->se.sum_exec_runtime` (bumping `utime`/`stime` does nothing — INV-1). But unlike a cgroup's `bstat`, a task's `sum_exec_runtime` **does** have consumers that make decisions from it (INV-7):

- POSIX CPU timers and `RLIMIT_CPU` — a task could take `SIGXCPU` because of migration work
- NUMA-balancing scan-period heuristics — perturbed, ironically, since NUMA balancing is one of migration's main callers
- `RLIMIT_RTTIME` for RT tasks

Same-shaped `+=`, entirely different consequences — purely because of who reads the number. That asymmetry is why the plan does AC-4(a) and explicitly declines AC-4(b).

### Gap 2, stated

> The plumbing to charge one task's CPU time to a different cgroup already exists and is trivially callable — `__cgroup_account_cputime()` takes a cgroup, not a task. The gap is only that nothing in padata currently calls it. It closes without any scheduler change.

It is listed as a gap at all because it is *conceptually* downstream of Gap 1: if the association from Gap 1 existed, `update_se()` would route cgroup time to the donor automatically (INV-2) and no padata-side call would be needed. Gap 2 is what you fix when you cannot fix Gap 1 — a legitimate shortcut, available precisely because statistics carry no obligations.

### One caveat under sched_ext

This is the single thing a BPF scheduler cannot do (INV-27, INV-28). With `CONFIG_SCHED_CLASS_EXT` the `donor`/`curr` split is compiled out — they are the same union member — and no scx kfunc writes cgroup cputime. A BPF scheduler can therefore do fully delegated billing *for its own dispatch decisions*, but cannot feed `cpu.stat`. Making it do so needs a new `scx_bpf_cgroup_account_cputime()`-style kfunc, i.e. a kernel change — which is why plan AC-4(c.4) is ordered last and gated (TODO-4, DEC-3), while AC-4(a) is not.

---

## Gap 3: Enforcement without borrowed context

This is the hard one — the only gap that genuinely needs a kernel change, and the reason the plan has a demonstrator at all.

### What "enforcement" means here

`cpu.max` says *"this cgroup may use at most Q microseconds of CPU per P-microsecond period."* Enforcement is the part that actually **stops** it when the limit is reached. Statistics (Gap 2) merely record; enforcement acts.

### How CFS bandwidth actually works

Think of it as a **prepaid meter**.

- Each cgroup has a central account refilled once per period: `cfs_b->quota`, replenished by a timer.
- Each CPU draws a small chunk of that into a **local wallet**: `cfs_rq->runtime_remaining`, topped up by `__assign_cfs_rq_runtime()` (`kernel/sched/fair.c:6507-6532`).
- Running spends from the local wallet — `cfs_rq->runtime_remaining -= delta_exec` in `__account_cfs_rq_runtime()` (`:6536-6551`), reached from `update_curr()` at `:2022`.
- When the wallet empties, the CPU goes back to the central account. If that is empty too, `throttle_cfs_rq()` (`:6876+`) dequeues that cgroup's runqueue and walks the task-group tree marking it throttled — the lights go out for that cgroup on that CPU until the next period.

The whole structure is per-cgroup **and** per-CPU, which is what makes it cheap: no global lock on the fast path, just a decrement on a local counter.

### Where padata breaks it

The helper is spending, but the meter it decrements belongs to **root** — the kworker's own cgroup. The initiator's meter never moves, so the initiator's `cpu.max` is never reached no matter how much copying is done on its behalf.

The obvious response is to dock the initiator's meter instead. But then:

> **the meter and the consumer are different objects.**

Emptying the initiator's wallet dequeues the *initiator's* cgroup on that CPU — correctly stopping the initiator's own threads — while the helper, drawing on root's meter, keeps running at full speed. You would switch off the lights in the wrong room.

That asymmetry is the entire gap. It is also why Gap 3 is hard while Gap 2 is easy: `cpu.stat` is a number nobody reads to make a decision, so adding to it carries no obligation. `runtime_remaining` going negative **must** result in somebody stopping, and that somebody has to be whoever is actually burning the CPU.

### Why proxy execution does not have this problem

Under proxy execution the runner has **no meter of its own** — it only runs because the donor was picked (see "Deep dive: how proxy execution works" above). Empty the donor's wallet, the donor stops being picked, and the runner — having no independent right to run — stops with it. Enforcement is not implemented; it *falls out* of the exclusivity.

A padata helper is an ordinary runnable kworker with its own scheduling entity, its own runqueue and its own meter. Nothing about stopping the initiator reaches it.

### The correction: charging alone is insufficient, not wrong

An earlier revision of the plan concluded that charging the initiator without co-locating the helper was *"strictly worse than not charging"* — it throttles the initiator's threads while the helper runs on. That is true **only of charging in isolation**.

Pair it with a helper that voluntarily stops, and the pair is correct:

- the initiator's threads throttle — **correct**, the cgroup genuinely did consume that CPU;
- the helper stops too, because it checks — **correct**, it is the consumer.

Both stop. Nothing is enforced against the wrong party. The real obstacle is therefore narrower than "cross-hierarchy throttling is impossible": it is simply that **no stop signal currently reaches the helper**.

### Why cooperative enforcement is viable here specifically

Cooperative back-off would be a poor answer for arbitrary tasks — you cannot ask a userspace process to poll a budget. It works here because of three properties that happen to line up:

1. **The consumer is cooperative kernel code.** A padata helper can be made to check something between chunks; a random application cannot.
2. **There is already a natural boundary, and bailing is already safe.** `padata_mt_helper()` claims one chunk per iteration and, on loop exit, increments `nworks_fini` and completes if last. A helper that breaks out early simply leaves unclaimed chunks for others — and the initiator's own inline run only exits when `job->size` reaches 0, so the job always completes regardless of helper behaviour (INV-24). No new completion machinery is required.
3. **The query already exists.** `cfs_rq_throttled()` (`kernel/sched/fair.c:6562-6564`) is a plain predicate: `cfs_bandwidth_used() && cfs_rq->throttled`.

And there is direct precedent in mm: memcg's `memory.high` does not ask the scheduler to stop an over-limit task either. `__mem_cgroup_handle_over_high()` accumulates over-limit state and applies a `penalty_jiffies` delay at a safe blocking point (INV-22). Cooperative enforcement of a cgroup limit is an established pattern, not an invention.

### Gap 3, stated

> Charging a cgroup and stopping a task are the same operation in CFS — `runtime_remaining` hitting zero dequeues *that* runqueue — so a consumer outside the charged hierarchy cannot be reached. Proxy execution avoids this by giving the runner no independent right to run; a padata helper has one. Closing the gap does not require cross-hierarchy preemptive throttling: it requires only that the helper be able to *ask* whether the context it is charging is over budget, and stop.

### What this implies for the fix

The minimal ask (plan AC-4(d) / TODO-4) follows directly:

1. `cpu_charge_begin(cgroup)` / `cpu_charge_end()` — the association Gap 1 lacks.
2. Route `update_curr()`'s charging to that context while active — reuses the existing meter, subject to INV-26's hierarchy-walk question.
3. `cpu_charge_over_budget()` — a new query built on the existing `cfs_rq_throttled()` predicate. **Not a thin wrapper**: it must decide *which* per-CPU `cfs_rq` of the charged hierarchy to inspect (the initiator's cgroup has one per CPU; the helper is on a different CPU) and under what locking, since CFS bandwidth state is manipulated under rq-lock assumptions on the current hierarchy. That is the mirror image of piece 2's hierarchy-walk question (INV-26), and settling both is the demonstrator's main job.

Piece 3 **dissolves** Gap 3 rather than solving it: no new enforcement mechanism is added, because the consumer stops itself. That is why the ask is small, and it is the single most important thing the sched_ext demonstrator exists to validate before it is made.

---

## Gap 4: 1:N with a *running* donor

The last gap, and the one that most rewards being split apart — three questions have been travelling together under one name, and they have completely different answers.

### What makes this configuration new

Proxy execution's premise is that the donor is **blocked**, so its CPU slot is free to lend, and exactly one runner borrows it. padata inverts both halves:

- there are **N helpers running concurrently**, not one;
- the initiator is **itself running**, draining its own chunks via caller-participation (INV-15), so its slot is not free to lend.

So this is not "redirect my unused slot to someone else." It is "**add these N other CPUs' worth of work to my bill, while I keep working too.**" A different operation, not a variation on the same one.

### 4a — Plumbing: can N runners name one payer?

**Under CFS, no.** `rq->donor` is a single field per runqueue, the `blocked_on` chain is linear, and the design migrates the waiter onto the owner's CPU rather than proxying across the gap (INV-6, INV-25). Everything about the mechanism is one-to-one and single-runqueue.

**Under sched_ext, trivially yes.** scx does not use `rq->donor` at all — the field is compiled out (INV-27). The association lives in BPF task-local storage or a map, where "8 helpers point at initiator T" is eight entries. No structural limit exists to run into (INV-31).

This part is genuinely closed by the demonstrator, and genuinely still open in CFS.

### 4b — Quota (`cpu.max`): not actually a gap

Recall Gap 3's prepaid meter. A quota is denominated in **absolute microseconds**, so spending composes across CPUs by construction — which is precisely what `cfs_b->quota` plus per-CPU `runtime_remaining` already does today. "Eight helpers consumed 400 ms between them" is a perfectly well-formed statement against a budget, under either scheduler.

There is nothing to invent here. What there *is*, is a consequence worth stating out loud:

> A cgroup capped at 1 CPU that fans out to 8 helpers exhausts its period budget in roughly one-eighth of a period, and is then throttled.

**Parallel copy therefore buys a tightly-capped cgroup essentially nothing.** That is not a defect — it is what a quota means; you said this cgroup gets one CPU, and it should not get eight merely by routing work through migration. But it does bound who the feature helps: mtcopy's benefit is available exactly to the extent the cgroup has headroom.

### 4c — Weight (`cpu.weight`): the genuinely hard one

Here the analogy from Gap 3 stops working, and that is the point.

A quota is a meter. A **weight is not a meter at all — it is your share of a table.** At each CPU there is a table where the runnable tasks divide up time in proportion to their weights. It is a *ratio*, recomputed locally, per runqueue.

Now put the initiator at table 0 and its helpers at tables 1 through 7. There is no way to express that at table 0. You can shrink the initiator's share of table 0 all the way to nothing — it is still eating seven other tables' worth of food. The unit available at table 0 is "some fraction of one CPU"; the quantity you need to offset is "seven whole CPUs." **The units do not match.** Weight does not compose across CPUs the way absolute microseconds do.

**sched_ext can express it — by not being CFS.** Most BPF schedulers discard per-runqueue weight competition entirely in favour of globally-ordered virtual time. Vtime-ordered dispatch queues are first-class in scx (`p->scx.dsq_vtime`, the `scx_dsq_priq_less()` comparator at `kernel/sched/ext/ext.c:1352-1360`, the `scx_bpf_dsq_insert_vtime` and `scx_bpf_dsq_move_set_vtime` kfuncs), and a DSQ can be shared across all CPUs. In that model there is one global line ordered by "how much have you consumed so far," so eating at seven other tables simply pushes you further back in the single queue. Charging helper runtime into the initiator's vtime delays the initiator on *every* CPU. That is a real, working answer (INV-32).

CFS has no global line to push anyone back in.

### The strategically important consequence

> **The part sched_ext fixes best is the part that cannot be asked for.**

A demonstrator showing global-vtime weight fairness would be impressive and would prove something true — and it would be evidence for a change CFS cannot adopt without replacing its fairness model, which is exactly the large, open-ended scheduler-core change the plan's AC-4(d) is forbidden from requesting.

So the demonstrator is deliberately **scoped to quota only**. Quota composes across CPUs, therefore CFS could plausibly adopt it, therefore the three-piece ask can be about it. Weight-based 1:N is an explicit non-goal, enforced in the plan by a positive test, a negative test, and a scoping note — because without a guardrail the natural pull is toward the more impressive demonstration.

### Gap 4, stated

> One payer with N concurrent runners, itself still running, has no representation in CFS: the plumbing is one-to-one and single-runqueue (4a), and while budgets compose across CPUs perfectly well (4b), weights structurally cannot (4c). sched_ext closes 4a outright and 4c only by substituting a different fairness model — so the portable ask covers 4a and 4b, and 4c is left alone on purpose.
