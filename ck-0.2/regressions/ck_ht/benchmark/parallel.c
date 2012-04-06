/*
 * Copyright 2012 Samy Al Bahra.
 * All rights reserved.
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ck_ht.h>

#ifdef CK_F_HT

#include <assert.h>
#include <ck_epoch.h>
#include <ck_malloc.h>
#include <ck_pr.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../common.h"

static ck_ht_t ht CK_CC_CACHELINE;
static char **keys;
static size_t keys_length = 0;
static size_t keys_capacity = 128;
static ck_epoch_t epoch_ht;
static ck_epoch_record_t epoch_wr;
static int n_threads;
static bool next_stage;

enum state {
	HT_STATE_STOP = 0,
	HT_STATE_GET,
	HT_STATE_STRICT_REPLACEMENT,
	HT_STATE_DELETION,
	HT_STATE_REPLACEMENT,
	HT_STATE_COUNT
};

static struct affinity affinerator = AFFINITY_INITIALIZER;
static uint64_t accumulator[HT_STATE_COUNT];
static int barrier[HT_STATE_COUNT];
static int state;

struct ht_epoch {
	ck_epoch_entry_t epoch_entry;
};

static void
alarm_handler(int s)
{

	(void)s;
	next_stage = true;
	return;
}

static void
ht_destroy(ck_epoch_entry_t *e)
{

	free(e);
	return;
}

static void *
ht_malloc(size_t r)
{
	ck_epoch_entry_t *b;

	b = malloc(sizeof(*b) + r);
	return b + 1;
}

static void
ht_free(void *p, size_t b, bool r)
{
	struct ht_epoch *e = p;

	(void)b;

	if (r == true) {
		/* Destruction requires safe memory reclamation. */
		ck_epoch_free(&epoch_wr, &(--e)->epoch_entry, ht_destroy);
	} else {
		free(--e);
	}

	return;
}

static struct ck_malloc my_allocator = {
	.malloc = ht_malloc,
	.free = ht_free
};

static void
table_init(void)
{

	ck_epoch_init(&epoch_ht, 10);
	ck_epoch_register(&epoch_ht, &epoch_wr);
	srand48((long int)time(NULL));
	ck_ht_allocator_set(&my_allocator);
	if (ck_ht_init(&ht, CK_HT_MODE_BYTESTRING, 8, lrand48()) == false) {
		perror("ck_ht_init");
		exit(EXIT_FAILURE);
	}

	return;
}

static bool
table_remove(const char *value)
{
	ck_ht_entry_t entry;
	ck_ht_hash_t h;
	size_t l = strlen(value);

	ck_ht_hash(&h, &ht, value, l);
	ck_ht_entry_key_set(&entry, value, l);
	return ck_ht_remove_spmc(&ht, h, &entry);
}

static bool
table_replace(const char *value)
{
	ck_ht_entry_t entry;
	ck_ht_hash_t h;
	size_t l = strlen(value);

	ck_ht_hash(&h, &ht, value, l);
	ck_ht_entry_set(&entry, h, value, l, "REPLACED");
	return ck_ht_set_spmc(&ht, h, &entry);
}

static void *
table_get(const char *value)
{
	ck_ht_entry_t entry;
	ck_ht_hash_t h;
	size_t l = strlen(value);

	ck_ht_hash(&h, &ht, value, l);
	ck_ht_entry_key_set(&entry, value, l);
	if (ck_ht_get_spmc(&ht, h, &entry) == true)
		return ck_ht_entry_value(&entry);

	return NULL;
}

static bool
table_insert(const char *value)
{
	ck_ht_entry_t entry;
	ck_ht_hash_t h;
	size_t l = strlen(value);

	ck_ht_hash(&h, &ht, value, l);
	ck_ht_entry_set(&entry, h, value, l, value);
	return ck_ht_put_spmc(&ht, h, &entry);
}

static size_t
table_count(void)
{

	return ck_ht_count(&ht);
}

static bool
table_reset(void)
{

	return ck_ht_reset_spmc(&ht);
}

static void *
ht_reader(void *unused)
{
	size_t i;
	ck_epoch_record_t epoch_record;
	int state_previous = HT_STATE_STOP;
	int n_state;
	uint64_t s, j, a;

	(void)unused;
	if (aff_iterate(&affinerator) != 0)
		perror("WARNING: Failed to affine thread");

	s = j = a = 0;
	ck_epoch_register(&epoch_ht, &epoch_record);
	for (;;) {
		j++;
		ck_epoch_read_begin(&epoch_record);
		s = rdtsc();
		for (i = 0; i < keys_length; i++) {
			char *r;

			r = table_get(keys[i]);
			if (r == NULL)
				continue;

			if (strcmp(r, "REPLACED") == 0)
				continue;

			if (strcmp(r, keys[i]) == 0)
				continue;

			fprintf(stderr, "ERROR: Found invalid value: [%s]\n", r);
			exit(EXIT_FAILURE);
		}
		a += rdtsc() - s;
		ck_epoch_read_end(&epoch_record);

		n_state = ck_pr_load_int(&state);
		if (n_state != state_previous) {
			ck_pr_add_64(&accumulator[state_previous], a / (j * keys_length));
			ck_pr_inc_int(&barrier[state_previous]);
			while (ck_pr_load_int(&barrier[state_previous]) != n_threads + 1)
				ck_pr_stall();

			state_previous = n_state;
			s = j = a = 0;
		}
	}

	return NULL;
}

int
main(int argc, char *argv[])
{
	FILE *fp;
	char buffer[512];
	size_t i, j, r;
	unsigned int d = 0;
	uint64_t s, e, a, repeated;
	char **t;
	pthread_t *readers;
	double p_r, p_d;

	r = 20;
	s = 8;
	p_d = 0.5;
	p_r = 0.5;
	n_threads = CORES - 1;

	if (argc < 2) {
		fprintf(stderr, "Usage: ck_ht <dictionary> [<interval length> <initial size> <readers>\n"
		    " <probability of replacement> <probability of deletion> <epoch threshold>]\n");
		exit(EXIT_FAILURE);
	}

	if (argc >= 3)
		r = atoi(argv[2]);

	if (argc >= 4)	
		s = (uint64_t)atoi(argv[3]);

	if (argc >= 5) {
		n_threads = atoi(argv[4]);
		if (n_threads < 1) {
			fprintf(stderr, "ERROR: Number of readers must be >= 1.\n");
			exit(EXIT_FAILURE);
		}
	}

	if (argc >= 6) {
		p_r = atof(argv[5]) / 100.00;
		if (p_r < 0) {
			fprintf(stderr, "ERROR: Probability of replacement must be >= 0 and <= 100.\n");
			exit(EXIT_FAILURE);
		}
	}

	if (argc >= 7) {
		p_d = atof(argv[6]) / 100.00;
		if (p_d < 0) {
			fprintf(stderr, "ERROR: Probability of deletion must be >= 0 and <= 100.\n");
			exit(EXIT_FAILURE);
		}
	}

	affinerator.delta = 1;
	readers = malloc(sizeof(pthread_t) * n_threads);
	assert(readers != NULL);

	keys = malloc(sizeof(char *) * keys_capacity);
	assert(keys != NULL);

	fp = fopen(argv[1], "r");
	assert(fp != NULL);

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		buffer[strlen(buffer) - 1] = '\0';
		keys[keys_length++] = strdup(buffer);
		assert(keys[keys_length - 1] != NULL);

		if (keys_length == keys_capacity) {
			t = realloc(keys, sizeof(char *) * (keys_capacity *= 2));
			assert(t != NULL);
			keys = t;
		}
	}

	t = realloc(keys, sizeof(char *) * keys_length);
	assert(t != NULL);
	keys = t;

	table_init();

	for (i = 0; i < (size_t)n_threads; i++) {
		if (pthread_create(&readers[i], NULL, ht_reader, NULL) != 0) {
			fprintf(stderr, "ERROR: Failed to create thread %zu.\n", i);
			exit(EXIT_FAILURE);
		}
	}

	for (i = 0; i < keys_length; i++)
		d += table_insert(keys[i]) == false;

	fprintf(stderr, " [S] %zu entries stored and %u duplicates.\n\n",
	    table_count(), d);

	fprintf(stderr, " ,- BASIC TEST\n");
	fprintf(stderr, " | Executing SMR test...");
	a = 0;
	for (j = 0; j < r; j++) {
		ck_epoch_write_begin(&epoch_wr);
		if (table_reset() == false) {
			fprintf(stderr, "ERROR: Failed to reset hash table.\n");
			exit(EXIT_FAILURE);
		}

		s = rdtsc();
		for (i = 0; i < keys_length; i++)
			d += table_insert(keys[i]) == false;
		e = rdtsc();
		a += e - s;
		ck_epoch_write_end(&epoch_wr);
	}
	fprintf(stderr, "done (%" PRIu64 " ticks)\n", a / (r * keys_length));

	fprintf(stderr, " | Executing replacement test...");
	a = 0;
	for (j = 0; j < r; j++) {
		ck_epoch_write_begin(&epoch_wr);
		s = rdtsc();
		for (i = 0; i < keys_length; i++)
			table_replace(keys[i]);
		e = rdtsc();
		a += e - s;
		ck_epoch_write_end(&epoch_wr);
	}
	fprintf(stderr, "done (%" PRIu64 " ticks)\n", a / (r * keys_length));

	fprintf(stderr, " | Executing get test...");
	a = 0;
	for (j = 0; j < r; j++) {
		ck_epoch_read_begin(&epoch_wr);
		s = rdtsc();
		for (i = 0; i < keys_length; i++) {
			if (table_get(keys[i]) == NULL) {
				fprintf(stderr, "ERROR: Unexpected NULL value.\n");
				exit(EXIT_FAILURE);
			}
		}
		e = rdtsc();
		a += e - s;
		ck_epoch_read_end(&epoch_wr);
	}
	fprintf(stderr, "done (%" PRIu64 " ticks)\n", a / (r * keys_length));

	a = 0;
	fprintf(stderr, " | Executing removal test...");
	for (j = 0; j < r; j++) {
		ck_epoch_write_begin(&epoch_wr);
		s = rdtsc();
		for (i = 0; i < keys_length; i++)
			table_remove(keys[i]);
		e = rdtsc();
		a += e - s;

		for (i = 0; i < keys_length; i++)
			table_insert(keys[i]);
		ck_epoch_write_end(&epoch_wr);
	} 
	fprintf(stderr, "done (%" PRIu64 " ticks)\n", a / (r * keys_length));

	fprintf(stderr, " | Executing negative look-up test...");
	a = 0;
	for (j = 0; j < r; j++) {
		ck_epoch_read_begin(&epoch_wr);
		s = rdtsc();
		for (i = 0; i < keys_length; i++) {
			table_get("\x50\x03\x04\x05\x06\x10");
		}
		e = rdtsc();
		a += e - s;
		ck_epoch_read_end(&epoch_wr);
	}
	fprintf(stderr, "done (%" PRIu64 " ticks)\n", a / (r * keys_length));

	ck_epoch_record_t epoch_temporary = epoch_wr;
	ck_epoch_purge(&epoch_wr);

	fprintf(stderr, " '- Summary: %u pending, %u peak, %" PRIu64 " reclamations -> "
	    "%u pending, %u peak, %" PRIu64 " reclamations\n\n",
	    epoch_temporary.n_pending, epoch_temporary.n_peak, epoch_temporary.n_reclamations,
	    epoch_wr.n_pending, epoch_wr.n_peak, epoch_wr.n_reclamations);

	fprintf(stderr, " ,- READER CONCURRENCY\n");
	fprintf(stderr, " | Executing reader test...");

	ck_pr_store_int(&state, HT_STATE_GET);
	while (ck_pr_load_int(&barrier[HT_STATE_STOP]) != n_threads)
		ck_pr_stall();
	ck_pr_inc_int(&barrier[HT_STATE_STOP]);
	sleep(r);
	ck_pr_store_int(&state, HT_STATE_STRICT_REPLACEMENT);
	while (ck_pr_load_int(&barrier[HT_STATE_GET]) != n_threads)
		ck_pr_stall();
	fprintf(stderr, "done (reader = %" PRIu64 " ticks)\n",
	    accumulator[HT_STATE_GET] / n_threads);

	fprintf(stderr, " | Executing strict replacement test...");
	
	a = repeated = 0;
	signal(SIGALRM, alarm_handler);
	alarm(r);

	ck_pr_inc_int(&barrier[HT_STATE_GET]);
	for (;;) {
		repeated++;
		s = rdtsc();
		for (i = 0; i < keys_length; i++)
			table_replace(keys[i]);
		e = rdtsc();
		a += e - s;

		if (next_stage == true) {
			next_stage = false;
			break;
		}
	}

	ck_pr_store_int(&state, HT_STATE_DELETION);
	while (ck_pr_load_int(&barrier[HT_STATE_STRICT_REPLACEMENT]) != n_threads)
		ck_pr_stall();
	table_reset();
	ck_epoch_purge(&epoch_wr);
	fprintf(stderr, "done (writer = %" PRIu64 " ticks, reader = %" PRIu64 " ticks)\n",
	    a / (repeated * keys_length), accumulator[HT_STATE_STRICT_REPLACEMENT] / n_threads);

	signal(SIGALRM, alarm_handler);
	alarm(r);

	fprintf(stderr, " | Executing deletion test (%.2f)...", p_d * 100);
	a = repeated = 0;
	ck_pr_inc_int(&barrier[HT_STATE_STRICT_REPLACEMENT]);
	for (;;) {
		double delete;

		repeated++;
		s = rdtsc();
		for (i = 0; i < keys_length; i++) {
			table_insert(keys[i]);
			if (p_d != 0.0) {
				delete = drand48();
				if (delete <= p_d)
					table_remove(keys[i]);
			}
		}
		e = rdtsc();
		a += e - s;

		if (next_stage == true) {
			next_stage = false;
			break;
		}
	}
	ck_pr_store_int(&state, HT_STATE_REPLACEMENT);
	while (ck_pr_load_int(&barrier[HT_STATE_DELETION]) != n_threads)
		ck_pr_stall();

	table_reset();
	ck_epoch_purge(&epoch_wr);
	fprintf(stderr, "done (writer = %" PRIu64 " ticks, reader = %" PRIu64 " ticks)\n",
	    a / (repeated * keys_length), accumulator[HT_STATE_DELETION] / n_threads);

	signal(SIGALRM, alarm_handler);
	alarm(r);

	fprintf(stderr, " | Executing replacement test (%.2f)...", p_r * 100);
	a = repeated = 0;
	ck_pr_inc_int(&barrier[HT_STATE_DELETION]);
	for (;;) {
		double replace, delete;

		repeated++;
		s = rdtsc();
		for (i = 0; i < keys_length; i++) {
			table_insert(keys[i]);
			if (p_d != 0.0) {
				delete = drand48();
				if (delete <= p_d)
					table_remove(keys[i]);
			}
			if (p_r != 0.0) {
				replace = drand48();
				if (replace <= p_r)
					table_replace(keys[i]);
			}
		}
		e = rdtsc();
		a += e - s;

		if (next_stage == true) {
			next_stage = false;
			break;
		}
	}
	ck_pr_store_int(&state, HT_STATE_STOP);
	while (ck_pr_load_int(&barrier[HT_STATE_REPLACEMENT]) != n_threads)
		ck_pr_stall();
	table_reset();
	ck_epoch_purge(&epoch_wr);
	fprintf(stderr, "done (writer = %" PRIu64 " ticks, reader = %" PRIu64 " ticks)\n",
	    a / (repeated * keys_length), accumulator[HT_STATE_REPLACEMENT] / n_threads);

	ck_pr_inc_int(&barrier[HT_STATE_REPLACEMENT]);
	epoch_temporary = epoch_wr;
	ck_epoch_purge(&epoch_wr);

	fprintf(stderr, " '- Summary: %u pending, %u peak, %" PRIu64 " reclamations -> "
	    "%u pending, %u peak, %" PRIu64 " reclamations\n\n",
	    epoch_temporary.n_pending, epoch_temporary.n_peak, epoch_temporary.n_reclamations,
	    epoch_wr.n_pending, epoch_wr.n_peak, epoch_wr.n_reclamations);
	return 0;
}
#else
int
main(void)
{

	return 0;
}
#endif /* CK_F_HT */

