#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <semaphore.h>

typedef struct {
	int n, m, type, start, step;
	uint64_t *bits;
} worker_t;

#define slow_comp(i, n) ((uint64_t)(i) * (i) * (i) % (n))

#ifndef __APPLE__ // Mac OS X does not implement spinlock
static pthread_spinlock_t g_spin;
#endif
static pthread_mutex_t g_mutex;
static sem_t g_sem;
static uint8_t g_lock2;

void *worker(void *data)
{
	worker_t *w = (worker_t*)data;
	int64_t i, nm;
	if (w->type == 0) { // only works for a single thread
		for (i = w->start, nm = (int64_t)w->n * w->m; i < nm; i += w->step) {
			uint64_t x = slow_comp(i, w->n);
			w->bits[x>>6] ^= 1LLU << (x&0x3f);
		}
	} else if (w->type == 1) { // no locking
		for (i = w->start, nm = (int64_t)w->n * w->m; i < nm; i += w->step) {
			uint64_t x = slow_comp(i, w->n);
			uint64_t *p = &w->bits[x>>6];
			uint64_t y = 1LLU << (x & 0x3f);
			__sync_xor_and_fetch(p, y);
		}
	} else if (w->type == 2) { // pseudo spin lock
		for (i = w->start, nm = (int64_t)w->n * w->m; i < nm; i += w->step) {
			uint64_t x = slow_comp(i, w->n);
			uint64_t *p = &w->bits[x>>6];
			uint64_t y = 1LLU << (x & 0x3f);
			while (!__sync_bool_compare_and_swap(&g_lock2, 0, 1)); // busy waiting
			*p ^= y;
			__sync_bool_compare_and_swap(&g_lock2, 1, 0);
		}
	} else if (w->type == 3) { // pthread spin lock
#ifdef __APPLE__
		fprintf(stderr, "ERROR: the pthread spin lock is not implemented in Mac OS X.\n");
#else
		for (i = w->start, nm = (int64_t)w->n * w->m; i < nm; i += w->step) {
			uint64_t x = slow_comp(i, w->n);
			uint64_t *p = &w->bits[x>>6];
			uint64_t y = 1LLU << (x & 0x3f);
			pthread_spin_lock(&g_spin);
			*p ^= y;
			pthread_spin_unlock(&g_spin);
		}
#endif
	} else if (w->type == 4) { // pthread mutex lock
		for (i = w->start, nm = (int64_t)w->n * w->m; i < nm; i += w->step) {
			uint64_t x = slow_comp(i, w->n);
			uint64_t *p = &w->bits[x>>6];
			uint64_t y = 1LLU << (x & 0x3f);
			pthread_mutex_lock(&g_mutex);
			*p ^= y;
			pthread_mutex_unlock(&g_mutex);
		}
	} else if (w->type == 5) { // semaphore
		for (i = w->start, nm = (int64_t)w->n * w->m; i < nm; i += w->step) {
			uint64_t x = slow_comp(i, w->n);
			uint64_t *p = &w->bits[x>>6];
			uint64_t y = 1LLU << (x & 0x3f);
			sem_wait(&g_sem);
			*p ^= y;
			sem_post(&g_sem);
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int c, n_threads = 1, i, tmp;
	uint64_t z;
	worker_t *w, w0;
	pthread_t *tid;
	pthread_attr_t attr;

	w0.n = 1000000; w0.m = 100; w0.type = 1; w0.start = 0; w0.step = 1;
	while ((c = getopt(argc, argv, "t:n:s:m:l:")) >= 0) {
		switch (c) {
			case 't': n_threads = atoi(optarg); break;
			case 'n': w0.n = atoi(optarg); break;
			case 'm': w0.m = atoi(optarg); break;
			case 'l': w0.type = atoi(optarg); break;
		}
	}
	fprintf(stderr, "Usage: lock_test [-t nThreads=%d] [-n size=%d] [-m repeat=%d] [-l lockType=%d]\n",
			n_threads, w0.n, w0.m, w0.type);
	fprintf(stderr, "Lock type: 0 for single-thread; 1 for gcc builtin; 2 for spin lock; 3 for pthread spin; 4 for mutex; 5 for semaphore\n");

	w0.bits = (uint64_t*)calloc((w0.n + 63) / 64, 8);

#ifndef __APPLE__
	pthread_spin_init(&g_spin, 0);
#endif
	pthread_mutex_init(&g_mutex, NULL);
	sem_init(&g_sem, 0, 1);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	tid = alloca(sizeof(pthread_t) * n_threads);
	w = alloca(sizeof(worker_t) * n_threads);
	for (i = 0; i < n_threads; ++i) {
		w[i] = w0;
		w[i].start = i; w[i].step = n_threads;
	}
	for (i = 0; i < n_threads; ++i) pthread_create(&tid[i], &attr, worker, &w[i]);
	for (i = 0; i < n_threads; ++i) pthread_join(tid[i], 0);
	sem_destroy(&g_sem);
	pthread_mutex_destroy(&g_mutex);

	for (i = 0, z = 0, tmp = (w0.n + 63)/64; i < tmp; ++i) z ^= w0.bits[i];
	fprintf(stderr, "Hash: %llx (should be 0 if -m is even or aaaaaaaaaaaaaaa if -m is odd)\n", (unsigned long long)z);
	free(w0.bits);
	return 0;
}
/*
OSX: 2.53 GHz Intel Core 2 Duo; Mac OS X 10.6.8; gcc 4.2.1
Linux: 2.83 GHz Intel Xeon E5440; Debian 5.0; gcc 4.3.2; glibc 2.7
=========================================================================================================
 Type              OSX-1         OSX-2        Linux-1        Linux-2          Linux-4         Linux2-4
---------------------------------------------------------------------------------------------------------
 Single         1.4/1.4+0.0     (Wrong)     1.2/1.1+0.0      (Wrong)          (Wrong)          (Wrong)
 Atomic         3.8/3.8+0.0   2.2/4.2+0.0   3.6/3.6+0.0    3.7/7.3 +0.0     2.5/9.9 +0.0    6.6/25.5+0.0
 Pseudo spin    5.8/5.7+0.0   5.1/9.5+0.0   5.1/5.1+0.0   20.7/41.1+0.1    32.7/130 +0.3  ~49.6/180 +0.2
 Pthread spin      (N/A)         (N/A)      3.8/3.8+0.0   21.1/42.0+0.2    53.8/206 +0.5  ~40.5/203 +0.0
 Pthread mutex  7.5/7.5+0.0  ~182/66 +205   6.0/5.9+0.0   31.0/22.2+22.9  ~97.2/55.9+228  ~95.5/83.0+237
 Semaphore      ~85/55 +30    ~51/55 +41    5.5/5.4+0.0   46.0/30.4+38.4   ~133/76.4+342  ~98.8/57.2+248
=========================================================================================================
* numbers with "~" are measured with a smaller -m and then scaled to -m100.
* CPU time is taken from a couple of runs. Variance seems quite large, though.
 */
