/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * We maintain a number of worker thread pools, to spread lock contention.
 *
 * Pools can be added on the fly, as a means to mitigate lock contention,
 * but can only be removed again by a restart. (XXX: we could fix that)
 *
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include "cache.h"
#include "common/heritage.h"

#include "vtim.h"

#include <stdio.h>
#include "orbit.h"

FILE *obout = NULL;

#if 0
#define obprintf(fmt, ...) do { fprintf(obout, fmt, ##__VA_ARGS__); } while (0)
#else
#define obprintf(fmt, ...) do { } while (0)
#endif

VTAILQ_HEAD(taskhead, pool_task);

struct poolsock {
	unsigned			magic;
#define POOLSOCK_MAGIC			0x1b0a2d38
	struct listen_sock		*lsock;
	struct pool_task		task;
};

/* Number of work requests queued in excess of worker threads available */

struct pool {
	unsigned			magic;
#define POOL_MAGIC			0x606658fa
	VTAILQ_ENTRY(pool)		list;

	pthread_cond_t			herder_cond;
	pthread_t			herder_thr;
	struct orbit_module		*herder_ob;
	struct orbit_pool		*herder_ob_pool;
	struct orbit_allocator		*herder_ob_alloc;
	struct orbit_pool		*herder_ob_scratch_pool;

	struct lock			mtx;
	struct taskhead			idle_queue;
	struct taskhead			front_queue;
	struct taskhead			back_queue;
	unsigned			nthr;
	unsigned			dry;
	unsigned			lqueue;
	uintmax_t			ndropped;
	uintmax_t			nqueued;
	struct sesspool			*sesspool;
	struct dstat			*a_stat;
	struct dstat			*b_stat;
};

static struct lock		pool_mtx;
static pthread_t		thr_pool_herder;
static unsigned			pool_accepting = 0;

static struct lock		wstat_mtx;

struct orbit_allocator *Pool_ObAlloc(void *priv)
{
	struct pool *pp;
	CAST_OBJ_NOTNULL(pp, priv, POOL_MAGIC);
	return pp->herder_ob_alloc;
}

/*--------------------------------------------------------------------
 * Summing of stats into global stats counters
 */

static void
pool_sumstat(const struct dstat *src)
{

	Lck_AssertHeld(&wstat_mtx);
#define L0(n)
#define L1(n) (VSC_C_main->n += src->n)
#define VSC_F(n, t, l, f, v, d, e) L##l(n);
#include "tbl/vsc_f_main.h"
#undef VSC_F
#undef L0
#undef L1
}

void
Pool_Sumstat(struct worker *w)
{

	Lck_Lock(&wstat_mtx);
	pool_sumstat(&w->stats);
	Lck_Unlock(&wstat_mtx);
	memset(&w->stats, 0, sizeof w->stats);
}

static int
Pool_TrySumstat(struct worker *w)
{
	if (Lck_Trylock(&wstat_mtx))
		return (0);
	pool_sumstat(&w->stats);
	Lck_Unlock(&wstat_mtx);
	memset(&w->stats, 0, sizeof w->stats);
	return (1);
}

/*--------------------------------------------------------------------
 * Summing of stats into pool counters
 */

static void
pool_addstat(struct dstat *dst, struct dstat *src)
{

	dst->summs++;
#define L0(n)
#define L1(n) (dst->n += src->n)
#define VSC_F(n, t, l, f, v, d, e) L##l(n);
#include "tbl/vsc_f_main.h"
#undef VSC_F
#undef L0
#undef L1
	memset(src, 0, sizeof *src);
}

/*--------------------------------------------------------------------
 * Helper function to update stats for purges under lock
 */

void
Pool_PurgeStat(unsigned nobj)
{
	Lck_Lock(&wstat_mtx);
	VSC_C_main->n_purges++;
	VSC_C_main->n_obj_purged += nobj;
	Lck_Unlock(&wstat_mtx);
}


/*--------------------------------------------------------------------
 */

static struct worker *
pool_getidleworker(struct pool *pp)
{
	struct pool_task *pt;
	struct worker *wrk;

	CHECK_OBJ_NOTNULL(pp, POOL_MAGIC);
	Lck_AssertHeld(&pp->mtx);
	pt = VTAILQ_FIRST(&pp->idle_queue);
	if (pt == NULL) {
		if (pp->nthr < cache_param->wthread_max) {
			obprintf("orbit: not found idle workder, dry++\n");
			pp->dry++;
			AZ(pthread_cond_signal(&pp->herder_cond));
		}
		return (NULL);
	}
	AZ(pt->func);
	CAST_OBJ_NOTNULL(wrk, pt->priv, WORKER_MAGIC);
	obprintf("orbit: found idle workder\n");
	return (wrk);
}

/*--------------------------------------------------------------------
 * Nobody is accepting on this socket, so we do.
 *
 * As long as we can stick the accepted connection to another thread
 * we do so, otherwise we put the socket back on the "BACK" queue
 * and handle the new connection ourselves.
 *
 * We store data about the accept in reserved workspace on the reserved
 * worker workspace.  SES_pool_accept_task() knows about this.
 */

static void
pool_accept(struct worker *wrk, void *arg)
{
	struct worker *wrk2;
	struct wrk_accept *wa, *wa2;
	struct pool *pp;
	struct poolsock *ps;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	pp = wrk->pool;
	CHECK_OBJ_NOTNULL(pp, POOL_MAGIC);
	CAST_OBJ_NOTNULL(ps, arg, POOLSOCK_MAGIC);

	CHECK_OBJ_NOTNULL(ps->lsock, LISTEN_SOCK_MAGIC);
	assert(sizeof *wa == WS_Reserve(wrk->aws, sizeof *wa));
	wa = (void*)wrk->aws->f;

	/* Delay until we are ready (flag is set when all
	 * initialization has finished) */
	while (!pool_accepting)
		VTIM_sleep(.1);

	while (1) {
		memset(wa, 0, sizeof *wa);
		wa->magic = WRK_ACCEPT_MAGIC;

		if (ps->lsock->sock < 0) {
			/* Socket Shutdown */
			FREE_OBJ_ORBIT(pp->herder_ob_alloc, ps);
			WS_Release(wrk->aws, 0);
			return;
		}
		if (VCA_Accept(ps->lsock, wa) < 0) {
			wrk->stats.sess_fail++;
			/* We're going to pace in vca anyway... */
			(void)Pool_TrySumstat(wrk);
			continue;
		}

		Lck_Lock(&pp->mtx);
		wrk2 = pool_getidleworker(pp);
		if (wrk2 == NULL) {
			/* No idle threads, do it ourselves */
			Lck_Unlock(&pp->mtx);
			AZ(Pool_Task(pp, &ps->task, POOL_QUEUE_BACK));
			SES_pool_accept_task(wrk, pp->sesspool);
			return;
		}
		VTAILQ_REMOVE(&pp->idle_queue, &wrk2->task, list);
		AZ(wrk2->task.func);
		assert(sizeof *wa2 == WS_Reserve(wrk2->aws, sizeof *wa2));
		wa2 = (void*)wrk2->aws->f;
		memcpy(wa2, wa, sizeof *wa);
		wrk2->task.func = SES_pool_accept_task;
		wrk2->task.priv = pp->sesspool;
		Lck_Unlock(&pp->mtx);
		AZ(pthread_cond_signal(&wrk2->cond));

		/*
		 * We were able to hand off, so release this threads VCL
		 * reference (if any) so we don't hold on to discarded VCLs.
		 */
		if (wrk->vcl != NULL)
			VCL_Rel(&wrk->vcl);
	}
}

/*--------------------------------------------------------------------
 * Enter a new task to be done
 */

int
Pool_Task(struct pool *pp, struct pool_task *task, enum pool_how how)
{
	struct worker *wrk;
	int retval = 0;

	CHECK_OBJ_NOTNULL(pp, POOL_MAGIC);
	AN(task);
	AN(task->func);

	Lck_Lock(&pp->mtx);

	/*
	 * The common case first:  Take an idle thread, do it.
	 */

	wrk = pool_getidleworker(pp);
	if (wrk != NULL) {
		VTAILQ_REMOVE(&pp->idle_queue, &wrk->task, list);
		AZ(wrk->task.func);
		wrk->task.func = task->func;
		wrk->task.priv = task->priv;
		Lck_Unlock(&pp->mtx);
		AZ(pthread_cond_signal(&wrk->cond));
		return (0);
	}

	switch (how) {
	case POOL_NO_QUEUE:
		retval = -1;
		break;
	case POOL_QUEUE_FRONT:
		/* If we have too much in the queue already, refuse. */
		if (pp->lqueue > cache_param->wthread_queue_limit) {
			pp->ndropped++;
			retval = -1;
		} else {
			VTAILQ_INSERT_TAIL(&pp->front_queue, task, list);
			pp->nqueued++;
			pp->lqueue++;
		}
		break;
	case POOL_QUEUE_BACK:
		VTAILQ_INSERT_TAIL(&pp->back_queue, task, list);
		break;
	default:
		WRONG("Unknown enum pool_how");
	}
	Lck_Unlock(&pp->mtx);
	return (retval);
}

/*--------------------------------------------------------------------
 * Empty function used as a pointer value for the thread exit condition.
 */

static void
pool_kiss_of_death(struct worker *wrk, void *priv)
{
	(void)wrk;
	(void)priv;
}

/*--------------------------------------------------------------------
 * Special function to summ stats
 */

static void __match_proto__(pool_func_t)
pool_stat_summ(struct worker *wrk, void *priv)
{
	struct dstat *src;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->pool, POOL_MAGIC);
	AN(priv);
	src = priv;
	Lck_Lock(&wstat_mtx);
	pool_sumstat(src);
	Lck_Unlock(&wstat_mtx);
	memset(src, 0, sizeof *src);
	wrk->pool->b_stat = src;
}

/*--------------------------------------------------------------------
 * This is the work function for worker threads in the pool.
 */

void
Pool_Work_Thread(void *priv, struct worker *wrk)
{
	struct pool *pp;
	struct pool_task *tp;
	struct pool_task tps;
	int i;

	CAST_OBJ_NOTNULL(pp, priv, POOL_MAGIC);
	wrk->pool = pp;
	while (1) {
		Lck_Lock(&pp->mtx);

		CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);

		WS_Reset(wrk->aws, NULL);

		tp = VTAILQ_FIRST(&pp->front_queue);
		if (tp != NULL) {
			pp->lqueue--;
			VTAILQ_REMOVE(&pp->front_queue, tp, list);
		} else {
			tp = VTAILQ_FIRST(&pp->back_queue);
			if (tp != NULL)
				VTAILQ_REMOVE(&pp->back_queue, tp, list);
		}

		if ((tp == NULL && wrk->stats.summs > 0) ||
		    (wrk->stats.summs >= cache_param->wthread_stats_rate))
			pool_addstat(pp->a_stat, &wrk->stats);

		if (tp != NULL) {
			wrk->stats.summs++;
		} else if (pp->b_stat != NULL && pp->a_stat->summs) {
			/* Nothing to do, push pool stats into global pool */
			tps.func = pool_stat_summ;
			tps.priv = pp->a_stat;
			pp->a_stat = pp->b_stat;
			pp->b_stat = NULL;
			tp = &tps;
		} else {
			/* Nothing to do: To sleep, perchance to dream ... */
			if (isnan(wrk->lastused))
				wrk->lastused = VTIM_real();
			wrk->task.func = NULL;
			wrk->task.priv = wrk;
			VTAILQ_INSERT_HEAD(&pp->idle_queue, &wrk->task, list);
			do {
				i = Lck_CondWait(&wrk->cond, &pp->mtx,
				    wrk->vcl == NULL ?  0 : wrk->lastused+60.);
				if (i == ETIMEDOUT)
					VCL_Rel(&wrk->vcl);
			} while (wrk->task.func == NULL);
			tp = &wrk->task;
			wrk->stats.summs++;
		}
		Lck_Unlock(&pp->mtx);

		if (tp->func == pool_kiss_of_death)
			break;

		assert(wrk->pool == pp);
		tp->func(wrk, tp->priv);
	}
	wrk->pool = NULL;
}

/*--------------------------------------------------------------------
 * Create another thread.
 */

static void
pool_breed(struct pool *qp, const pthread_attr_t *tp_attr)
{
	pthread_t tp;

	if (pthread_create(&tp, tp_attr, WRK_thread, qp)) {
		VSL(SLT_Debug, 0, "Create worker thread failed %d %s",
		    errno, strerror(errno));
		Lck_Lock(&pool_mtx);
		VSC_C_main->threads_failed++;
		Lck_Unlock(&pool_mtx);
		VTIM_sleep(cache_param->wthread_fail_delay);
	} else {
		AZ(pthread_detach(tp));
		qp->dry = 0;
		qp->nthr++;
		Lck_Lock(&pool_mtx);
		VSC_C_main->threads++;
		VSC_C_main->threads_created++;
		Lck_Unlock(&pool_mtx);
		VTIM_sleep(cache_param->wthread_add_delay);
	}
}

// TODO: rewrite the wrapper with new API
unsigned long addition_helper(size_t argc, unsigned long argv[]) {
	*(uint64_t*)argv[0] += (int64_t)argv[1];
	return 0;
}
unsigned long lock_helper(size_t argc, unsigned long argv[]) {
	Lck_Lock((struct lock*)argv[0]);
	return 0;
}
unsigned long unlock_helper(size_t argc, unsigned long argv[]) {
	Lck_Unlock((struct lock*)argv[0]);
	return 0;
}
unsigned long signal_helper(size_t argc, unsigned long argv[]) {
	AZ(pthread_cond_signal((pthread_cond_t*)argv[0]));
	return 0;
}
unsigned long vtailq_remove_helper(size_t argc, unsigned long argv[]) {
	struct taskhead *head = (struct taskhead *)argv[0];
	struct pool_task *task = (struct pool_task *)argv[1];
	VTAILQ_REMOVE(head, task, list);
	return 0;
}
unsigned long sleep_helper(size_t argc, unsigned long argv[]) {
	VTIM_sleep(*(double*)argv);
	return 0;
}
unsigned long cond_wait_helper(size_t argc, unsigned long argv[]) {
	(void)Lck_CondWait((pthread_cond_t*)argv[0], (struct lock*)argv[1], VTIM_real() + 5);
	return 0;
}
unsigned long pool_breed_helper(size_t argc, unsigned long argv[]) {
	struct pool *qp = (struct pool *)argv[0];
	pthread_attr_t *tp_attr = (pthread_attr_t *)argv[1];
	pool_breed(qp, tp_attr);
	return 0;
}

struct pool_herder_args {
	struct pool *pp;
};

static unsigned long pool_herder_orbit(void *store, void *argbuf)
{
	struct pool_herder_args *args = (struct pool_herder_args*)argbuf;
	(void)store;

	struct pool *pp = args->pp;
	struct pool_task *pt;
	pthread_attr_t tp_attr;
	double t_idle;
	struct worker *wrk;

	struct orbit_scratch scratch;
	orbit_scratch_create(&scratch);

	AZ(pthread_attr_init(&tp_attr));

	/* Set the stacksize for worker threads we create */
	if (cache_param->wthread_stacksize != UINT_MAX)
		AZ(pthread_attr_setstacksize(&tp_attr,
		    cache_param->wthread_stacksize));
	else {
		AZ(pthread_attr_destroy(&tp_attr));
		AZ(pthread_attr_init(&tp_attr));
	}

	/* Make more threads if needed and allowed */
	if (pp->nthr < cache_param->wthread_min ||
	    (pp->dry && pp->nthr < cache_param->wthread_max)) {
		pthread_attr_t *tp_attr_s = (pthread_attr_t *)
		    ((struct orbit_repr*)((char*)scratch.ptr + scratch.cursor))->any.data;
		orbit_scratch_push_any(&scratch, &tp_attr, sizeof(tp_attr));
		/* pool_breed(pp, tp_attr_s); */
		unsigned long argv[] = { (unsigned long)pp, (unsigned long)tp_attr_s, };
		orbit_scratch_push_operation(&scratch, pool_breed_helper, 2, argv);
		obprintf("orbit: requested add thread\n");
		goto end;
	}

	if (pp->nthr > cache_param->wthread_min) {

		t_idle = VTIM_real() - cache_param->wthread_timeout;
		obprintf("orbit: t_idle is %f\n", t_idle);

		unsigned long argv[] = { (unsigned long)&pp->mtx, };
		orbit_scratch_push_operation(&scratch, lock_helper, 1, argv);
		/* Lck_Lock(&pp->mtx); */

		/* XXX: unsafe counters */
		unsigned long argv_num[2];

		argv_num[0] = (unsigned long)&VSC_C_main->sess_queued;
		argv_num[1] = pp->nqueued;
		/* VSC_C_main->sess_queued += pp->nqueued; */
		orbit_scratch_push_operation(&scratch, addition_helper, 2, argv_num);

		argv_num[0] = (unsigned long)&VSC_C_main->sess_dropped;
		argv_num[1] = pp->ndropped;
		/* VSC_C_main->sess_dropped += pp->ndropped; */
		orbit_scratch_push_operation(&scratch, addition_helper, 2, argv_num);

		pp->nqueued = pp->ndropped = 0;
		orbit_scratch_push_update(&scratch, &pp->nqueued, sizeof(pp->nqueued));
		orbit_scratch_push_update(&scratch, &pp->ndropped, sizeof(pp->ndropped));

		wrk = NULL;
		pt = VTAILQ_LAST(&pp->idle_queue, taskhead);
		if (pt != NULL) {
			obprintf("orbit info: we are in pt != NULL\n");
			AZ(pt->func);
			CAST_OBJ_NOTNULL(wrk, pt->priv, WORKER_MAGIC);
			obprintf("orbit: found one idle, lastused %f\n", wrk->lastused);

			if (wrk->lastused < t_idle ||
			    pp->nthr > cache_param->wthread_max) {
				/* Give it a kiss on the cheek... */
				unsigned long argv[] = { (unsigned long)&pp->idle_queue, (unsigned long)&wrk->task };
				orbit_scratch_push_operation(&scratch, vtailq_remove_helper, 2, argv);
				/* VTAILQ_REMOVE(&pp->idle_queue,
				    &wrk->task, list); */
				obprintf("orbit: requested kill idle thread\n");

				wrk->task.func = pool_kiss_of_death;
				orbit_scratch_push_update(&scratch,
				    &wrk->task.func, sizeof(wrk->task.func));

				argv[0] = (unsigned long)&wrk->cond;
				orbit_scratch_push_operation(&scratch, signal_helper, 1, argv);
				/* AZ(pthread_cond_signal(&wrk->cond)); */
			} else
				wrk = NULL;
		}
		/* Lck_Unlock(&pp->mtx); */
		orbit_scratch_push_operation(&scratch, unlock_helper, 1, argv);

		if (wrk != NULL) {
			unsigned long argv[2];

			argv[0] = (unsigned long)&pp->nthr;
			argv[1] = -1;
			orbit_scratch_push_operation(&scratch, addition_helper, 2, argv);
			/* pp->nthr--; */

			argv[0] = (unsigned long)&pool_mtx;
			orbit_scratch_push_operation(&scratch, lock_helper, 1, argv);
			/* Lck_Lock(&pool_mtx); */

			argv[0] = (unsigned long)&VSC_C_main->threads;
			argv[1] = -1;
			/* VSC_C_main is using shared memory? */
			/* VSC_C_main->threads--; */
			orbit_scratch_push_operation(&scratch, addition_helper, 2, argv);

			argv[0] = (unsigned long)&VSC_C_main->threads_destroyed;
			argv[1] = 1;
			/* VSC_C_main->threads_destroyed++; */
			orbit_scratch_push_operation(&scratch, addition_helper, 2, argv);

			/* Lck_Unlock(&pool_mtx); */
			argv[0] = (unsigned long)&pool_mtx;
			orbit_scratch_push_operation(&scratch, unlock_helper, 1, argv);

			/* VTIM_sleep(cache_param->wthread_destroy_delay); */
			void *tmp = (void*)&cache_param->wthread_destroy_delay;
			argv[0] = *(unsigned long*)tmp;
			orbit_scratch_push_operation(&scratch, sleep_helper, 1, argv);

			goto end;
		}
	}

	unsigned long argv[] = { (unsigned long)&pp->mtx, };
	orbit_scratch_push_operation(&scratch, lock_helper, 1, argv);
	/* Lck_Lock(&pp->mtx); */
	if (!pp->dry) {
		/* (void)Lck_CondWait(&pp->herder_cond, &pp->mtx,
			VTIM_real() + 5); */
		unsigned long argv[] = { (unsigned long)&pp->herder_cond, (unsigned long)&pp->mtx, };
		orbit_scratch_push_operation(&scratch, cond_wait_helper, 2, argv);
	} else {
		/* XXX: unsafe counters */
		unsigned long argv[] = { (unsigned long)&VSC_C_main->threads_limited, 1, };
		orbit_scratch_push_operation(&scratch, addition_helper, 2, argv);
		/* VSC_C_main->threads_limited++; */

		pp->dry = 0;
		orbit_scratch_push_update(&scratch, &pp->dry, sizeof(pp->dry));
	}
	/* Lck_Unlock(&pp->mtx); */
	orbit_scratch_push_operation(&scratch, unlock_helper, 1, argv);

end:
	orbit_sendv(&scratch);

	return 0;
}

/*--------------------------------------------------------------------
 * Herd a single pool
 *
 * This thread wakes up whenever a pool queues.
 *
 * The trick here is to not be too aggressive about creating threads.
 * We do this by only examining one pool at a time, and by sleeping
 * a short while whenever we create a thread and a little while longer
 * whenever we fail to, hopefully missing a lot of cond_signals in
 * the meantime.
 *
 * XXX: probably need a lot more work.
 *
 */

static void*
pool_herder(void *priv)
{
	struct pool *pp;

	CAST_OBJ_NOTNULL(pp, priv, POOL_MAGIC);

	pthread_setname_np(pthread_self(), "pool_herder");

	while (1) {
		struct pool_herder_args args = (struct pool_herder_args) {
			.pp = pp,
		};
		struct orbit_task task;
		union orbit_result result;
		int ret;

		// FIXME: make orbit call without holding a lock may snapshot
		// inconsistent view of data.
		long flags = (1<<8);
		// long flags = 0;
		ret = orbit_call_async(pp->herder_ob, flags, 1, &pp->herder_ob_pool,
				NULL, &args, sizeof(args), &task);
		AZ(ret);

		ret = orbit_recvv(&result, &task);
		int err = errno;
		fprintf(stderr, "orbit: ret = %d, err = %s\n", ret, strerror(err));
		// while (1) sleep(1);
		assert(ret == 1);
		// TODO: check return value of apply
		orbit_apply(&result.scratch, false);

		ret = orbit_recvv(&result, &task);
		assert(ret == 0);
		obprintf("orbit: call return result %ld\n", result.retval);
		obprintf("orbit: #thd %ld, created %ld destoryed %ld\n", VSC_C_main->threads,
			VSC_C_main->threads_created, VSC_C_main->threads_destroyed);
	}
	NEEDLESS_RETURN(NULL);
}

/*--------------------------------------------------------------------
 * Add a thread pool
 */

struct orbit_pool *global_obpool = NULL;
struct orbit_allocator *global_oballoc = NULL;

static struct pool *
pool_mkpool(unsigned pool_no)
{
	struct pool *pp;
	struct listen_sock *ls;
	struct poolsock *ps;

	struct orbit_pool *obpool;
	struct orbit_allocator *oballoc;

	if (obout == NULL) {
		obout = fopen("/tmp/obout.log", "w");
		setbuf(obout, NULL);
	}

	if (global_obpool == NULL)
		global_obpool = orbit_pool_create(NULL, 64 * 1024 * 1024);
	obpool = global_obpool;
	AN(obpool);
	if (global_oballoc == NULL)
		global_oballoc = orbit_allocator_from_pool(obpool, true);
	oballoc = global_oballoc;
	AN(oballoc);

	ALLOC_OBJ_ORBIT(oballoc, pp, POOL_MAGIC);
	if (pp == NULL)
		return (NULL);
	pp->a_stat = calloc(1, sizeof *pp->a_stat);
	AN(pp->a_stat);
	pp->b_stat = calloc(1, sizeof *pp->b_stat);
	AN(pp->b_stat);
	Lck_New(&pp->mtx, lck_wq);

        /* This needs to be done before herder_thr create */
	pp->herder_ob_pool = obpool;
	pp->herder_ob_alloc = oballoc;
	AN(pp->herder_ob_scratch_pool = orbit_pool_create(NULL, 64 * 1024 * 1024));
	AZ(orbit_scratch_set_pool(pp->herder_ob_scratch_pool));
	AN(pp->herder_ob = orbit_create("pool herder", pool_herder_orbit, NULL));

	VTAILQ_INIT(&pp->idle_queue);
	VTAILQ_INIT(&pp->front_queue);
	VTAILQ_INIT(&pp->back_queue);
	pp->sesspool = SES_NewPool(pp, pool_no);
	AN(pp->sesspool);
	AZ(pthread_cond_init(&pp->herder_cond, NULL));
	AZ(pthread_create(&pp->herder_thr, NULL, pool_herder, pp));

	VTAILQ_FOREACH(ls, &heritage.socks, list) {
		if (ls->sock < 0)
			continue;
		ALLOC_OBJ_ORBIT(oballoc, ps, POOLSOCK_MAGIC);
		XXXAN(ps);
		ps->lsock = ls;
		ps->task.func = pool_accept;
		ps->task.priv = ps;
		AZ(Pool_Task(pp, &ps->task, POOL_QUEUE_BACK));
	}

	return (pp);
}

/*--------------------------------------------------------------------
 * This thread adjusts the number of pools to match the parameter.
 *
 */

static void *
pool_poolherder(void *priv)
{
	unsigned nwq;
	VTAILQ_HEAD(,pool)	pools = VTAILQ_HEAD_INITIALIZER(pools);
	struct pool *pp;
	uint64_t u;

	THR_SetName("pool_herder");
	(void)priv;

	nwq = 0;
	while (1) {
		if (nwq < cache_param->wthread_pools) {
			pp = pool_mkpool(nwq);
			if (pp != NULL) {
				VTAILQ_INSERT_TAIL(&pools, pp, list);
				VSC_C_main->pools++;
				nwq++;
				continue;
			}
		}
		/* XXX: remove pools */
		if (0)
			SES_DeletePool(NULL);
		(void)sleep(1);
		u = 0;
		VTAILQ_FOREACH(pp, &pools, list)
			u += pp->lqueue;
		VSC_C_main->thread_queue_len = u;
	}
	NEEDLESS_RETURN(NULL);
}

/*--------------------------------------------------------------------*/

void
Pool_Accept(void)
{

	ASSERT_CLI();
	pool_accepting = 1;
}

void
Pool_Init(void)
{

	Lck_New(&wstat_mtx, lck_wstat);
	Lck_New(&pool_mtx, lck_wq);
	AZ(pthread_create(&thr_pool_herder, NULL, pool_poolherder, NULL));
}
