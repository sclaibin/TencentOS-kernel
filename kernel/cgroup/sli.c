#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/cgroup.h>
#include <linux/module.h>
#include <linux/psi.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/sysctl.h>
#include <linux/stacktrace.h>
#include <asm/irq_regs.h>
#include "../sched/sched.h"
#include <linux/cgroup.h>
#include <linux/sli.h>
#include <linux/rculist.h>

#define MAX_STACK_TRACE_DEPTH	64

static DEFINE_STATIC_KEY_FALSE(sli_enabled);
static DEFINE_STATIC_KEY_FALSE(sli_monitor_enabled);

static struct sli_event_monitor default_sli_event_monitor;
static struct workqueue_struct *sli_workqueue;

struct sli_event_control {
	int event_type;
	int event_id;
	int period;
	int mbuf_enable;
	unsigned long long count;
	unsigned long long threshold;
};

static const char *schedlat_theshold_name[] = {
	"schedlat_wait_threshold=",
	"schedlat_block_threshold=",
	"schedlat_ioblock_threshold=",
	"schedlat_sleep_threshold=",
	"schedlat_longsys_threshold=",
	"schedlat_rundelay_threshold=",
	"schedlat_irqtime_threshold="
};

static const char *memlat_threshold_name[] = {
	"memlat_global_direct_reclaim_threshold=",
	"memlat_memcg_direct_reclaim_threshold=",
	"memlat_direct_compact_threshold=",
	"memlat_global_direct_swapout_threshold=",
	"memlat_memcg_direct_swapout_threshold=",
	"memlat_direct_swapin_threshold=",
	"memlat_page_alloc_threshold="
};

static const char *longterm_threshold_name[] = {
	"longterm_rundelay_threshold=",
	"longterm_irqtime_threshold="
};

static const char *sanity_check_abbr[] = {
	"schedlat_",
	"memlat_",
	"longterm_",
	"period=",
	"mbuf_enable="
};

static unsigned long sli_get_longterm_statistics(struct cgroup *cgrp,
						 enum sli_longterm_event event_id);

/*
 * Convert the ULLONG_MAX to zero when show them to userspace, and convert the zero to
 * ULLONG_MAX when write value to control interface.
 */
static inline u64 sli_convert_value(u64 value, bool control_show)
{
	if (control_show && value == ULLONG_MAX)
		return 0;

	if (!control_show && value == 0)
		value = ULLONG_MAX;

	return value;
}

static void sli_event_monitor_init(struct sli_event_monitor *event_monitor, struct cgroup *cgrp)
{
	INIT_LIST_HEAD_RCU(&event_monitor->event_head);

	memset(&event_monitor->schedlat_threshold, 0xff, sizeof(event_monitor->schedlat_threshold));
	memset(&event_monitor->schedlat_count, 0xff, sizeof(event_monitor->schedlat_count));
	memset(&event_monitor->memlat_threshold, 0xff, sizeof(event_monitor->memlat_threshold));
	memset(&event_monitor->memlat_count, 0xff, sizeof(event_monitor->memlat_count));
	memset(&event_monitor->longterm_threshold, 0xff, sizeof(event_monitor->longterm_threshold));

	event_monitor->last_update = jiffies;
	event_monitor->cgrp = cgrp;
}

/* Inherit the monitoring event from parent cgroup or global sli_event_monitor */
static int sli_event_inherit(struct cgroup *cgrp)
{
	struct sli_event *event, *event_tmp;
	struct sli_event_monitor *event_monitor = &default_sli_event_monitor;
	struct sli_event_monitor *cgrp_event_monitor = cgrp->cgrp_event_monitor;

	rcu_read_lock();
	list_for_each_entry_rcu(event, &event_monitor->event_head, event_node) {
		struct sli_event *new_event;

		new_event = kmalloc(sizeof(struct sli_event), GFP_ATOMIC);
		if (!new_event)
			goto failed;

		new_event->event_type = event->event_type;
		new_event->event_id = event->event_id;

		switch (event->event_type) {
		case SLI_SCHED_EVENT:
			cgrp_event_monitor->schedlat_threshold[event->event_id] =
					event_monitor->schedlat_threshold[event->event_id];
			cgrp_event_monitor->schedlat_count[event->event_id] =
					event_monitor->schedlat_count[event->event_id];
			break;
		case SLI_MEM_EVENT:
			cgrp_event_monitor->memlat_threshold[event->event_id] =
					event_monitor->memlat_threshold[event->event_id];
			cgrp_event_monitor->memlat_count[event->event_id] =
					event_monitor->memlat_count[event->event_id];
			break;
		case SLI_LONGTERM_EVENT:
			cgrp_event_monitor->longterm_threshold[event->event_id] =
					event_monitor->longterm_threshold[event->event_id];
			atomic_long_set(&cgrp_event_monitor->longterm_statistics[event->event_id],
					sli_get_longterm_statistics(cgrp, event->event_id));
			break;
		default:
			printk(KERN_ERR "%s: invalid sli_event type!\n", __func__);
			goto failed;

		}

		list_add(&new_event->event_node, &cgrp_event_monitor->event_head);
	}
	rcu_read_unlock();

	cgrp->cgrp_event_monitor->period = event_monitor->period;
	cgrp->cgrp_event_monitor->mbuf_enable = event_monitor->mbuf_enable;

	return 0;

failed:
	rcu_read_unlock();

	/* Free memory from the event list */
	list_for_each_entry_safe(event, event_tmp,
				 &cgrp->cgrp_event_monitor->event_head, event_node) {
		list_del(&event->event_node);
		kfree(event);
	}

	return -1;
}

static void store_task_stack(struct task_struct *task, char *reason,
			     u64 duration, unsigned int skipnr)
{
	unsigned long *entries;
	unsigned nr_entries = 0;
	unsigned long flags;
	int i;
	struct cgroup *cgrp;

	entries = kmalloc_array(MAX_STACK_TRACE_DEPTH, sizeof(*entries),
				GFP_ATOMIC);
	if (!entries)
		return;

	nr_entries = stack_trace_save_tsk(task, entries, MAX_STACK_TRACE_DEPTH, skipnr);

	cgrp = get_cgroup_from_task(task);
	spin_lock_irqsave(&cgrp->cgrp_mbuf_lock, flags);

	mbuf_print(cgrp, "record reason:%s comm:%s pid:%d duration=%lld\n",
		   reason, task->comm, task->pid, duration);

	for (i = 0; i < nr_entries; i++)
		mbuf_print(cgrp, "[<0>] %pB\n", (void *)entries[i]);

	spin_unlock_irqrestore(&cgrp->cgrp_mbuf_lock, flags);

	kfree(entries);
	return;
}

static char * get_memlat_name(enum sli_memlat_stat_item sidx)
{
	char *name = NULL;

	switch (sidx) {
	case MEM_LAT_GLOBAL_DIRECT_RECLAIM:
		name = "memlat_global_direct_reclaim";
		break;
	case MEM_LAT_MEMCG_DIRECT_RECLAIM:
		name =  "memlat_memcg_direct_reclaim";
		break;
	case MEM_LAT_DIRECT_COMPACT:
		name = "memlat_direct_compact";
		break;
	case MEM_LAT_GLOBAL_DIRECT_SWAPOUT:
		name = "memlat_global_direct_swapout";
		break;
	case MEM_LAT_MEMCG_DIRECT_SWAPOUT:
		name = "memlat_memcg_direct_swapout";
		break;
	case MEM_LAT_DIRECT_SWAPIN:
		name = "memlat_direct_swapin";
		break;
	case MEM_LAT_PAGE_ALLOC:
		name = "memlat_page_alloc";
		break;
	default:
		break;
	}

	return name;
}

static enum sli_lat_count get_lat_count_idx(u64 duration)
{
	enum sli_lat_count idx;

	duration = duration >> 20;
	if (duration < 1)
		idx = LAT_0_1;
	else if (duration < 4)
		idx = LAT_1_4;
	else if (duration < 8)
		idx = LAT_4_8;
	else if (duration < 16)
		idx = LAT_8_16;
	else if (duration < 32)
		idx = LAT_16_32;
	else if (duration < 64)
		idx = LAT_32_64;
	else if (duration < 128)
		idx = LAT_64_128;
	else
		idx = LAT_128_INF;

	return idx;
}

static char * get_schedlat_name(enum sli_memlat_stat_item sidx)
{
	char *name = NULL;

	switch (sidx) {
	case SCHEDLAT_WAIT:
		name = "schedlat_wait";
		break;
	case SCHEDLAT_BLOCK:
		name =  "schedlat_block";
		break;
	case SCHEDLAT_IOBLOCK:
		name = "schedlat_ioblock";
		break;
	case SCHEDLAT_SLEEP:
		name = "schedlat_sleep";
		break;
	case SCHEDLAT_RUNDELAY:
		name = "schedlat_rundelay";
		break;
	case SCHEDLAT_LONGSYS:
		name = "schedlat_longsys";
		break;
	case SCHEDLAT_IRQTIME:
		name = "schedlat_irqtime";
		break;
	default:
		break;
	}

	return name;
}

static char *get_longterm_name(enum sli_longterm_event sidx)
{
	char *name = NULL;

	switch (sidx) {
	case SLI_LONGTERM_RUNDELAY:
		name = "longterm_rundelay";
		break;
	case SLI_LONGTERM_IRQTIME:
		name = "longterm_irqtime";
		break;
	default:
		break;
	}

	return name;
}

static u64 sli_memlat_stat_gather(struct cgroup *cgrp,
				 enum sli_memlat_stat_item sidx,
				 enum sli_lat_count cidx)
{
	u64 sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu_ptr(cgrp->sli_memlat_stat_percpu, cpu)->item[sidx][cidx];

	return sum;
}

int sli_memlat_stat_show(struct seq_file *m, struct cgroup *cgrp)
{
	enum sli_memlat_stat_item sidx;

	if (!static_branch_likely(&sli_enabled)) {
		seq_printf(m, "sli is not enabled, please echo 1 > /proc/sli/sli_enabled\n");
		return 0;
	}

	if (!cgrp->sli_memlat_stat_percpu)
		return 0;

	for (sidx = MEM_LAT_GLOBAL_DIRECT_RECLAIM;sidx < MEM_LAT_STAT_NR;sidx++) {
		seq_printf(m, "%s:\n", get_memlat_name(sidx));
		seq_printf(m, "0-1ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_0_1));
		seq_printf(m, "1-4ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_1_4));
		seq_printf(m, "4-8ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_4_8));
		seq_printf(m, "8-16ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_8_16));
		seq_printf(m, "16-32ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_16_32));
		seq_printf(m, "32-64ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_32_64));
		seq_printf(m, "64-128ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_64_128));
		seq_printf(m, ">=128ms: %llu\n", sli_memlat_stat_gather(cgrp, sidx, LAT_128_INF));
	}

	return 0;
}

int sli_memlat_max_show(struct seq_file *m, struct cgroup *cgrp)
{
	enum sli_memlat_stat_item sidx;

	if (!static_branch_likely(&sli_enabled)) {
		seq_printf(m, "sli is not enabled, please echo 1 > /proc/sli/sli_enabled\n");
		return 0;
	}

	if (!cgrp->sli_memlat_stat_percpu)
		return 0;

	for (sidx = MEM_LAT_GLOBAL_DIRECT_RECLAIM; sidx < MEM_LAT_STAT_NR; sidx++) {
		int cpu;
		unsigned long latency_sum = 0;

		for_each_possible_cpu(cpu)
			latency_sum += per_cpu_ptr(cgrp->sli_memlat_stat_percpu, cpu)->latency_max[sidx];

		seq_printf(m, "%s: %lu\n", get_memlat_name(sidx), latency_sum);
	}

	return 0;
}

void sli_memlat_stat_start(u64 *start)
{
	if (!static_branch_likely(&sli_enabled))
		*start = 0;
	else
		*start = local_clock();
}

void sli_memlat_stat_end(enum sli_memlat_stat_item sidx, u64 start)
{
	struct mem_cgroup *memcg;
	struct cgroup *cgrp;

	if (!static_branch_likely(&sli_enabled) || start == 0)
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_task(current);
	if (!memcg || memcg == root_mem_cgroup)
		goto out;

	cgrp = memcg->css.cgroup;
	if (cgrp && cgroup_parent(cgrp)) {
		enum sli_lat_count cidx;
		u64 duration;

		duration = local_clock() - start;
		cidx = get_lat_count_idx(duration);

		duration = duration >> 10;
		this_cpu_inc(cgrp->sli_memlat_stat_percpu->item[sidx][cidx]);
		this_cpu_add(cgrp->sli_memlat_stat_percpu->latency_max[sidx], duration);

		if (static_branch_unlikely(&sli_monitor_enabled)) {
			struct sli_event_monitor *event_monitor = cgrp->cgrp_event_monitor;

			if (duration < event_monitor->memlat_threshold[sidx])
				goto out;

			if (event_monitor->mbuf_enable) {
				char *lat_name;

				lat_name = get_memlat_name(sidx);
				store_task_stack(current, lat_name, duration, 0);
			}
		}
	}

out:
	rcu_read_unlock();
}

void sli_schedlat_stat(struct task_struct *task, enum sli_schedlat_stat_item sidx, u64 delta)
{
	struct cgroup *cgrp = NULL;

	if (!static_branch_likely(&sli_enabled) || !task)
		return;

	rcu_read_lock();
	cgrp = get_cgroup_from_task(task);
	if (cgrp && cgroup_parent(cgrp)) {
		enum sli_lat_count cidx = get_lat_count_idx(delta);

		delta = delta >> 10;
		this_cpu_inc(cgrp->sli_schedlat_stat_percpu->item[sidx][cidx]);
		this_cpu_add(cgrp->sli_schedlat_stat_percpu->latency_max[sidx], delta);

		if (static_branch_unlikely(&sli_monitor_enabled)) {
			struct sli_event_monitor *event_monitor = cgrp->cgrp_event_monitor;

			if (delta < event_monitor->schedlat_threshold[sidx])
				goto out;

			if (event_monitor->mbuf_enable) {
				char *lat_name;

				lat_name = get_schedlat_name(sidx);
				store_task_stack(task, lat_name, delta, 0);
			}
		}
	}

out:
	rcu_read_unlock();
}

void sli_schedlat_rundelay(struct task_struct *task, struct task_struct *prev, u64 delta)
{
	enum sli_schedlat_stat_item sidx = SCHEDLAT_RUNDELAY;
	struct cgroup *cgrp = NULL;

	if (!static_branch_likely(&sli_enabled) || !task || !prev)
		return;

	rcu_read_lock();
	cgrp = get_cgroup_from_task(task);
	if (cgrp && cgroup_parent(cgrp)) {
		enum sli_lat_count cidx = get_lat_count_idx(delta);

		delta = delta >> 10;
		this_cpu_inc(cgrp->sli_schedlat_stat_percpu->item[sidx][cidx]);
		this_cpu_add(cgrp->sli_schedlat_stat_percpu->latency_max[sidx], delta);

		if (static_branch_unlikely(&sli_monitor_enabled)) {
			struct sli_event_monitor *event_monitor = cgrp->cgrp_event_monitor;

			if (delta < event_monitor->schedlat_threshold[sidx])
				goto out;

			if (event_monitor->mbuf_enable) {
				int i;
				unsigned long *entries;
				unsigned int nr_entries = 0;
				unsigned long flags;

				entries = kmalloc_array(MAX_STACK_TRACE_DEPTH, sizeof(*entries),
							GFP_ATOMIC);
				if (!entries)
					goto out;

				nr_entries = stack_trace_save_tsk(prev, entries,
								  MAX_STACK_TRACE_DEPTH, 0);

				spin_lock_irqsave(&cgrp->cgrp_mbuf_lock, flags);

				mbuf_print(cgrp, "record reason:schedlat_rundelay next_comm:%s "
					   "next_pid:%d prev_comm:%s prev_pid:%d duration=%lld\n",
					   task->comm, task->pid, prev->comm, prev->pid, delta);

				for (i = 0; i < nr_entries; i++)
					mbuf_print(cgrp, "[<0>] %pB\n", (void *)entries[i]);

				spin_unlock_irqrestore(&cgrp->cgrp_mbuf_lock, flags);
				kfree(entries);
			}
		}
	}

out:
	rcu_read_unlock();
}

#ifdef CONFIG_SCHED_INFO
void sli_check_longsys(struct task_struct *tsk)
{
	long delta;

	if (!static_branch_likely(&sli_enabled))
		return;

	if (!tsk || tsk->sched_class != &fair_sched_class)
		return ;

	/* Longsys is performed only when TIF_RESCHED is set */
	if (!test_tsk_need_resched(tsk))
		return;

	/* Kthread is not belong to any cgroup */
	if (tsk->flags & PF_KTHREAD)
		return;

	if (!tsk->sched_info.kernel_exec_start ||
	    tsk->sched_info.task_switch != (tsk->nvcsw + tsk->nivcsw) ||
	    tsk->utime != tsk->sched_info.utime) {
		tsk->sched_info.utime = tsk->utime;
		tsk->sched_info.kernel_exec_start = rq_clock(this_rq());
		tsk->sched_info.task_switch = tsk->nvcsw + tsk->nivcsw;
		return;
	}

	delta = rq_clock(this_rq()) - tsk->sched_info.kernel_exec_start;
	sli_schedlat_stat(tsk, SCHEDLAT_LONGSYS, delta);
}

#endif

static u64 sli_schedlat_stat_gather(struct cgroup *cgrp,
				 enum sli_schedlat_stat_item sidx,
				 enum sli_lat_count cidx)
{
	u64 sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu_ptr(cgrp->sli_schedlat_stat_percpu, cpu)->item[sidx][cidx];

	return sum;
}

int sli_schedlat_max_show(struct seq_file *m, struct cgroup *cgrp)
{
	enum sli_schedlat_stat_item sidx;

	if (!static_branch_likely(&sli_enabled)) {
		seq_printf(m, "sli is not enabled, please echo 1 > /proc/sli/sli_enabled\n");
		return 0;
	}

	if (!cgrp->sli_schedlat_stat_percpu)
		return 0;

	for (sidx = SCHEDLAT_WAIT; sidx < SCHEDLAT_STAT_NR; sidx++) {
		int cpu;
		unsigned long latency_sum = 0;

		for_each_possible_cpu(cpu)
			latency_sum += per_cpu_ptr(cgrp->sli_schedlat_stat_percpu, cpu)->latency_max[sidx];

		seq_printf(m, "%s: %lu\n", get_schedlat_name(sidx), latency_sum);
	}

	return 0;
}

int sli_schedlat_stat_show(struct seq_file *m, struct cgroup *cgrp)
{
	enum sli_schedlat_stat_item sidx;

	if (!static_branch_likely(&sli_enabled)) {
		seq_printf(m, "sli is not enabled, please echo 1 > /proc/sli/sli_enabled\n");
		return 0;
	}

	if (!cgrp->sli_schedlat_stat_percpu)
		return 0;

	for (sidx = SCHEDLAT_WAIT;sidx < SCHEDLAT_STAT_NR;sidx++) {
		seq_printf(m, "%s:\n", get_schedlat_name(sidx));
		seq_printf(m, "0-1ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_0_1));
		seq_printf(m, "1-4ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_1_4));
		seq_printf(m, "4-8ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_4_8));
		seq_printf(m, "8-16ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_8_16));
		seq_printf(m, "16-32ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_16_32));
		seq_printf(m, "32-64ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_32_64));
		seq_printf(m, "64-128ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_64_128));
		seq_printf(m, ">=128ms: %llu\n", sli_schedlat_stat_gather(cgrp, sidx, LAT_128_INF));
	}

	return 0;
}

static unsigned long sli_get_longterm_statistics(struct cgroup *cgrp,
						 enum sli_longterm_event event_id)
{
	int cpu, index;
	unsigned long latency_sum = 0;

	index = SCHEDLAT_RUNDELAY + event_id;
	for_each_possible_cpu(cpu)
		latency_sum += per_cpu_ptr(cgrp->sli_schedlat_stat_percpu, cpu)->latency_max[index];

	return latency_sum;
}

static inline int sli_parse_threshold(char *buf, struct sli_event_control *sec)
{
	char *str;
	int i, len, ret;
	u64 value;

	/* Replace the delimiter with '\0' */
	len = strlen(buf);
	for (i = 0; i < len; i++) {
		if (buf[i] == ',' || buf[i] == ' ') {
			buf[i] = '\0';
			break;
		}
	}

	if (i == len)
		return -EINVAL;

	/* Parse the value for theshold */
	ret = kstrtou64(buf, 0, &value);
	if (ret)
		return ret;

	sec->threshold = sli_convert_value(value, false);

	/* Move the pointer to the positon which after the delimiter */
	buf += (i + 1);
	len -= (i + 1);

	/* Parse the value for count(if it exist) */
	str = strnstr(buf, "count=", len);
	if (!str)
		return -EINVAL;

	str += strlen("count=");
	ret = kstrtou64(str, 0, &value);
	if (ret)
		return ret;

	sec->count = sli_convert_value(value, false);

	return 0;
}

static int sli_parse_parameter(char *buf, int len, struct sli_event_control *sec, int index)
{
	int i, min_len, ret = 0;
	u64 value;

	switch (index) {
	case 0:
		for (i = 0; i < ARRAY_SIZE(schedlat_theshold_name); i++) {
			min_len = min(len, (int)strlen(schedlat_theshold_name[i]));
			if (!strncmp(schedlat_theshold_name[i], buf, min_len))
				break;
		}

		if (i == ARRAY_SIZE(schedlat_theshold_name))
			return -EINVAL;

		buf += min_len;
		ret = sli_parse_threshold(buf, sec);
		if (ret)
			return ret;

		sec->event_type = SLI_SCHED_EVENT;
		sec->event_id = i;
		break;
	case 1:
		for (i = 0; i < ARRAY_SIZE(memlat_threshold_name); i++) {
			min_len = min(len, (int)strlen((const char *)memlat_threshold_name[i]));
			if (!strncmp(memlat_threshold_name[i], buf, min_len))
				break;
		}

		if (i == ARRAY_SIZE(memlat_threshold_name))
			return -EINVAL;

		buf += min_len;
		ret = sli_parse_threshold(buf, sec);
		if (ret)
			return ret;

		sec->event_type = SLI_MEM_EVENT;
		sec->event_id = i;
		break;
	case 2:
		for (i = 0; i < ARRAY_SIZE(longterm_threshold_name); i++) {
			min_len = min(len, (int)strlen((const char *)longterm_threshold_name[i]));
			if (!strncmp(longterm_threshold_name[i], buf, min_len))
				break;
		}

		if (i == ARRAY_SIZE(longterm_threshold_name))
			return -EINVAL;

		buf += min_len;
		ret = sli_parse_threshold(buf, sec);
		if (ret)
			return ret;

		sec->event_type = SLI_LONGTERM_EVENT;
		sec->event_id = i;
		break;
	case 3:
		buf += strlen("period=");
		ret = kstrtou64(buf, 0, &value);
		if (ret)
			return ret;

		sec->period = usecs_to_jiffies(value);
		break;
	case 4:
		buf += strlen("mbuf_enable=");
		ret = kstrtou64(buf, 0, &value);
		if (ret)
			return ret;

		sec->mbuf_enable = !!value;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int sli_sanity_check(char *buf, struct sli_event_control *sec)
{
	int i, len, min_len;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	len = strlen(buf);
	for (i = 0; i < ARRAY_SIZE(sanity_check_abbr); i++) {
		min_len = min(len, (int)strlen(sanity_check_abbr[i]));

		if (!strncmp(sanity_check_abbr[i], buf, min_len))
			break;
	}

	/* The input string is not match with entries in the list */
	if (i == ARRAY_SIZE(sanity_check_abbr))
		return -EINVAL;

	return sli_parse_parameter(buf, len, sec, i);
}

static int sli_event_update(struct sli_event_monitor *event_monitor,
			    struct sli_event_control *sec, u64 last_threshold)
{
	struct sli_event *event;

	/* Add the sli event */
	if (last_threshold == ULLONG_MAX && sec->threshold != ULLONG_MAX) {
		event = kmalloc(sizeof(struct sli_event), GFP_KERNEL);
		if (!event)
			return -ENOMEM;

		event->event_type = sec->event_type;
		event->event_id = sec->event_id;
		/* event_type and event_id assignment should be done before add entry to list */
		smp_wmb();
		list_add_rcu(&event->event_node, &event_monitor->event_head);
	} else if (last_threshold != ULLONG_MAX && sec->threshold == ULLONG_MAX) {
		list_for_each_entry(event, &event_monitor->event_head, event_node) {
			if (event->event_type != sec->event_type)
				continue;

			if (event->event_id != sec->event_id)
				continue;

			list_del_rcu(&event->event_node);
			kfree_rcu(event, rcu);
			break;
		}
	}

	return 0;
}

ssize_t cgroup_sli_control_write(struct kernfs_open_file *of, char *buf,
				 size_t nbytes, loff_t off)
{
	int ret;
	struct cgroup *cgrp;
	struct sli_event_monitor *event_monitor;
	struct sli_event_control sec = {.event_type = -1, .period = -1, .mbuf_enable = -1,};

	cgrp = of_css(of)->cgroup;
	if (cgroup_parent(cgrp))
		event_monitor = cgrp->cgrp_event_monitor;
	else
		event_monitor = &default_sli_event_monitor;

	inode_lock(file_inode(of->file));
	ret = sli_sanity_check(buf, &sec);
	if (ret)
		goto out;

	if (sec.period != -1) {
		if (!!event_monitor->period == !!sec.period) {
			event_monitor->period = sec.period;
			goto out;
		}

		event_monitor->period = sec.period;

		if (cgroup_parent(cgrp) || event_monitor->mbuf_enable)
			goto out;

		if (sec.period)
			static_branch_enable(&sli_monitor_enabled);
		else
			static_branch_disable(&sli_monitor_enabled);

		goto out;
	}

	if (sec.mbuf_enable != -1) {
		if (sec.mbuf_enable == event_monitor->mbuf_enable)
			goto out;

		event_monitor->mbuf_enable = sec.mbuf_enable;

		if (cgroup_parent(cgrp) || event_monitor->period)
			goto out;

		if (sec.mbuf_enable)
			static_branch_enable(&sli_monitor_enabled);
		else
			static_branch_disable(&sli_monitor_enabled);

		goto out;
	}

	if (sec.event_type != -1) {
		unsigned long long last_threshold;

		switch (sec.event_type) {
		case SLI_SCHED_EVENT:
			last_threshold = event_monitor->schedlat_threshold[sec.event_id];
			event_monitor->schedlat_threshold[sec.event_id] = sec.threshold;
			event_monitor->schedlat_count[sec.event_id] = sec.count;
			smp_wmb();
			atomic_long_set(&event_monitor->schedlat_statistics[sec.event_id], 0);
			ret = sli_event_update(event_monitor, &sec, last_threshold);
			break;
		case SLI_MEM_EVENT:
			last_threshold = event_monitor->memlat_threshold[sec.event_id];
			event_monitor->memlat_threshold[sec.event_id] = sec.threshold;
			event_monitor->memlat_count[sec.event_id] = sec.count;
			smp_wmb();
			atomic_long_set(&event_monitor->memlat_statistics[sec.event_id], 0);
			ret = sli_event_update(event_monitor, &sec, last_threshold);
			break;
		case SLI_LONGTERM_EVENT:
			last_threshold = event_monitor->longterm_threshold[sec.event_id];
			event_monitor->longterm_threshold[sec.event_id] = sec.threshold;
			smp_wmb();
			if (cgroup_parent(cgrp))
				atomic_long_set(&event_monitor->longterm_statistics[sec.event_id],
						sli_get_longterm_statistics(cgrp, sec.event_id));
			ret = sli_event_update(event_monitor, &sec, last_threshold);
			break;
		default:
			break;
		}

	}

out:
	if (!ret)
		ret = nbytes;
	inode_unlock(file_inode(of->file));
	return ret;
}

int cgroup_sli_control_show(struct seq_file *sf, void *v)
{
	int i;
	unsigned long long threshold, count;
	struct cgroup *cgrp;
	struct sli_event_monitor *event_monitor;

	cgrp = seq_css(sf)->cgroup;
	if (cgroup_parent(cgrp))
		event_monitor = cgrp->cgrp_event_monitor;
	else
		event_monitor = &default_sli_event_monitor;

	seq_printf(sf, "period: %d\n", event_monitor->period);
	seq_printf(sf, "mbuf_enable: %d\n", event_monitor->mbuf_enable);

	for (i = 0; i < SCHEDLAT_STAT_NR; i++) {
		threshold = sli_convert_value(event_monitor->schedlat_threshold[i], true);
		count = sli_convert_value(event_monitor->schedlat_count[i], true);

		seq_printf(sf, "%s: threshold: %llu, count: %llu\n", get_schedlat_name(i),
			   threshold, count);
	}

	for (i = 0; i < MEM_LAT_STAT_NR; i++) {
		threshold = sli_convert_value(event_monitor->memlat_threshold[i], true);
		count = sli_convert_value(event_monitor->memlat_count[i], true);

		seq_printf(sf, "%s: threshold: %llu, count: %llu\n", get_memlat_name(i),
			   threshold, count);
	}

	for (i = 0; i < SLI_LONGTERM_NR; i++) {
		threshold = sli_convert_value(event_monitor->longterm_threshold[i], true);

		seq_printf(sf, "%s: threshold: %llu\n", get_longterm_name(i), threshold);
	}

	return 0;
}

static int sli_enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", static_key_enabled(&sli_enabled));
	return 0;
}

static int sli_enabled_open(struct inode *inode, struct file *file)
{
	return single_open(file, sli_enabled_show, NULL);
}

static ssize_t sli_enabled_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char val = -1;
	int ret = count;

	if (count < 1 || *ppos) {
		ret = -EINVAL;
		goto out;
	}

	if (copy_from_user(&val, ubuf, 1)) {
		ret = -EFAULT;
		goto out;
	}

	switch (val) {
	case '0':
		if (static_key_enabled(&sli_enabled))
			static_branch_disable(&sli_enabled);
		break;
	case '1':
		if (!static_key_enabled(&sli_enabled))
			static_branch_enable(&sli_enabled);
		break;
	default:
		ret = -EINVAL;
	}

out:
	return ret;
}

static const struct file_operations sli_enabled_fops = {
	.open       = sli_enabled_open,
	.read       = seq_read,
	.write      = sli_enabled_write,
	.llseek     = seq_lseek,
	.release    = single_release,
};

int sli_cgroup_alloc(struct cgroup *cgroup)
{
	if (!cgroup_need_sli(cgroup))
		return 0;

	spin_lock_init(&cgroup->cgrp_mbuf_lock);
	cgroup->sli_memlat_stat_percpu = alloc_percpu(struct sli_memlat_stat);
	if (!cgroup->sli_memlat_stat_percpu)
		goto out;

	cgroup->sli_schedlat_stat_percpu = alloc_percpu(struct sli_schedlat_stat);
	if (!cgroup->sli_schedlat_stat_percpu)
		goto free_memlat_percpu;

	cgroup->cgrp_event_monitor = kzalloc(sizeof(struct sli_event_monitor), GFP_KERNEL);
	if (!cgroup->cgrp_event_monitor)
		goto free_schelat_percpu;

	sli_event_monitor_init(cgroup->cgrp_event_monitor, cgroup);
	if (sli_event_inherit(cgroup))
		goto free_cgrp_event;

	return 0;

free_cgrp_event:
	kfree(cgroup->cgrp_event_monitor);
free_schelat_percpu:
	free_percpu(cgroup->sli_schedlat_stat_percpu);
free_memlat_percpu:
	free_percpu(cgroup->sli_memlat_stat_percpu);
out:
	return -ENOMEM;
}

void sli_cgroup_free(struct cgroup *cgroup)
{
	struct sli_event *event, *event_tmp;

	/*
	 * Cgroup's subsys would be cleared before sli_cgroup_free() had been called.
	 * So we use !cgroup->cgrp_event_monitor instead of cgroup_need_sli to check
	 * whether the cgroup'smemory should be freed here.
	 */
	if (!cgroup->cgrp_event_monitor)
		return;

	free_percpu(cgroup->sli_memlat_stat_percpu);
	free_percpu(cgroup->sli_schedlat_stat_percpu);
	/* Free memory from the event list */
	list_for_each_entry_safe(event, event_tmp,
				 &cgroup->cgrp_event_monitor->event_head, event_node) {
		list_del(&event->event_node);
		kfree(event);
	}
	kfree(cgroup->cgrp_event_monitor);
}

static int __init sli_proc_init(void)
{
	sli_event_monitor_init(&default_sli_event_monitor, NULL);
	sli_workqueue = alloc_workqueue("events_unbound", WQ_UNBOUND, WQ_UNBOUND_MAX_ACTIVE);
	if (!sli_workqueue) {
		printk(KERN_ERR "Create sli workqueue failed!\n");
		return -1;
	}
	proc_mkdir("sli", NULL);
	proc_create("sli/sli_enabled", 0, NULL, &sli_enabled_fops);
	return 0;
}

late_initcall(sli_proc_init);

