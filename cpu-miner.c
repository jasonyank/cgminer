
/*
 * Copyright 2010 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.  See COPYING for more details.
 */

#include "cpuminer-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#ifndef WIN32
#include <sys/resource.h>
#endif
#include <pthread.h>
#include <getopt.h>
#include <jansson.h>
#include "compat.h"
#include "miner.h"

#define PROGRAM_NAME		"minerd"
#define DEF_RPC_URL		"http://127.0.0.1:8332/"
#define DEF_RPC_USERPASS	"rpcuser:rpcpass"

enum {
	STAT_SLEEP_INTERVAL		= 100,
	STAT_CTR_INTERVAL		= 10000000,
};

enum sha256_algos {
	ALGO_C,			/* plain C */
	ALGO_4WAY,		/* parallel SSE2 */
	ALGO_VIA,		/* VIA padlock */
	ALGO_CRYPTOPP,		/* Crypto++ */
};

static const char *algo_names[] = {
	[ALGO_C]		= "c",
#ifdef WANT_SSE2_4WAY
	[ALGO_4WAY]		= "4way",
#endif
#ifdef WANT_VIA_PADLOCK
	[ALGO_VIA]		= "via",
#endif
	[ALGO_CRYPTOPP]		= "cryptopp",
};

bool opt_debug = false;
bool opt_protocol = false;
static bool program_running = true;
static const bool opt_time = true;
static enum sha256_algos opt_algo = ALGO_C;
static int opt_n_threads = 1;
static char *rpc_url = DEF_RPC_URL;
static char *userpass = DEF_RPC_USERPASS;


struct option_help {
	const char	*name;
	const char	*helptext;
};

static struct option_help options_help[] = {
	{ "help",
	  "(-h) Display this help text" },

	{ "algo XXX",
	  "(-a XXX) Specify sha256 implementation:\n"
	  "\tc\t\tLinux kernel sha256, implemented in C (default)"
#ifdef WANT_SSE2_4WAY
	  "\n\t4way\t\ttcatm's 4-way SSE2 implementation"
#endif
#ifdef WANT_VIA_PADLOCK
	  "\n\tvia\t\tVIA padlock implementation (EXPERIMENTAL)"
#endif
	  "\n\tcryptopp\tCrypto++ library implementation (EXPERIMENTAL)"
	  },

	{ "debug",
	  "(-D) Enable debug output (default: off)" },

	{ "protocol-dump",
	  "(-P) Verbose dump of protocol-level activities (default: off)" },

	{ "threads N",
	  "(-t N) Number of miner threads (default: 1)" },

	{ "url URL",
	  "URL for bitcoin JSON-RPC server "
	  "(default: " DEF_RPC_URL ")" },

	{ "userpass USERNAME:PASSWORD",
	  "Username:Password pair for bitcoin JSON-RPC server "
	  "(default: " DEF_RPC_USERPASS ")" },
};

static struct option options[] = {
	{ "help", 0, NULL, 'h' },
	{ "algo", 1, NULL, 'a' },
	{ "debug", 0, NULL, 'D' },
	{ "protocol-dump", 0, NULL, 'P' },
	{ "threads", 1, NULL, 't' },
	{ "url", 1, NULL, 1001 },
	{ "userpass", 1, NULL, 1002 },
	{ }
};

struct work {
	unsigned char	data[128];
	unsigned char	hash1[64];
	unsigned char	midstate[32];
	unsigned char	target[32];

	unsigned char	hash[32];
};

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (!tmp) {
		fprintf(stderr, "JSON key '%s' not found\n", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (!hexstr) {
		fprintf(stderr, "JSON key '%s' is not a string\n", key);
		return false;
	}
	if (!hex2bin(buf, hexstr, buflen))
		return false;

	return true;
}

static bool work_decode(const json_t *val, struct work *work)
{
	if (!jobj_binary(val, "midstate",
			 work->midstate, sizeof(work->midstate))) {
		fprintf(stderr, "JSON inval midstate\n");
		goto err_out;
	}

	if (!jobj_binary(val, "data", work->data, sizeof(work->data))) {
		fprintf(stderr, "JSON inval data\n");
		goto err_out;
	}

	if (!jobj_binary(val, "hash1", work->hash1, sizeof(work->hash1))) {
		fprintf(stderr, "JSON inval hash1\n");
		goto err_out;
	}

	if (!jobj_binary(val, "target", work->target, sizeof(work->target))) {
		fprintf(stderr, "JSON inval target\n");
		goto err_out;
	}

	memset(work->hash, 0, sizeof(work->hash));

	return true;

err_out:
	return false;
}

static void submit_work(struct work *work)
{
	char *hexstr = NULL;
	json_t *val, *res;
	char s[345];

	printf("PROOF OF WORK FOUND?  submitting...\n");

	/* build hex string */
	hexstr = bin2hex(work->data, sizeof(work->data));
	if (!hexstr)
		goto out;

	/* build JSON-RPC request */
	sprintf(s,
	      "{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
		hexstr);

	if (opt_debug)
		fprintf(stderr, "DBG: sending RPC call:\n%s", s);

	/* issue JSON-RPC request */
	val = json_rpc_call(rpc_url, userpass, s);
	if (!val) {
		fprintf(stderr, "submit_work json_rpc_call failed\n");
		goto out;
	}

	res = json_object_get(val, "result");

	printf("PROOF OF WORK RESULT: %s\n",
		json_is_true(res) ? "true (yay!!!)" : "false (booooo)");

	json_decref(val);

out:
	free(hexstr);
}

static void hashmeter(int thr_id, struct timeval *tv_start,
		      unsigned long hashes_done)
{
	struct timeval tv_end, diff;
	double khashes, secs;

	gettimeofday(&tv_end, NULL);

	timeval_subtract(&diff, &tv_end, tv_start);

	khashes = hashes_done / 1000.0;
	secs = (double)diff.tv_sec + ((double)diff.tv_usec / 1000000.0);

	printf("HashMeter(%d): %lu hashes, %.2f khash/sec\n",
	       thr_id, hashes_done,
	       khashes / secs);
}

static void *miner_thread(void *thr_id_int)
{
	int thr_id = (unsigned long) thr_id_int;
	static const char *rpc_req =
		"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

	while (1) {
		struct work work __attribute__((aligned(128)));
		unsigned long hashes_done;
		struct timeval tv_start;
		json_t *val;
		bool rc;

		/* obtain new work from bitcoin */
		val = json_rpc_call(rpc_url, userpass, rpc_req);
		if (!val) {
			fprintf(stderr, "json_rpc_call failed\n");
			return NULL;
		}

		/* decode result into work state struct */
		rc = work_decode(json_object_get(val, "result"), &work);
		if (!rc) {
			fprintf(stderr, "work decode failed\n");
			return NULL;
		}

		json_decref(val);

		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		/* scan nonces for a proof-of-work hash */
		switch (opt_algo) {
		case ALGO_C:
			rc = scanhash_c(work.midstate, work.data + 64,
				        work.hash1, work.hash, &hashes_done);
			break;

#ifdef WANT_SSE2_4WAY
		case ALGO_4WAY: {
			unsigned int rc4 =
				ScanHash_4WaySSE2(work.midstate, work.data + 64,
						  work.hash1, work.hash,
						  &hashes_done);
			rc = (rc4 == -1) ? false : true;
			}
			break;
#endif

#ifdef WANT_VIA_PADLOCK
		case ALGO_VIA:
			rc = scanhash_via(work.midstate, work.data + 64,
					  work.hash1, work.hash,
					  &hashes_done);
			break;
#endif
		case ALGO_CRYPTOPP:
			rc = scanhash_cryptopp(work.midstate, work.data + 64,
				        work.hash1, work.hash, &hashes_done);
			break;

		}

		hashmeter(thr_id, &tv_start, hashes_done);

		/* if nonce found, submit work */
		if (rc)
			submit_work(&work);
	}

	return NULL;
}

static void show_usage(void)
{
	int i;

	printf("Usage:\tminerd [options]\n\nSupported options:\n");
	for (i = 0; i < ARRAY_SIZE(options_help); i++) {
		struct option_help *h;

		h = &options_help[i];
		printf("--%s\n%s\n\n", h->name, h->helptext);
	}

	exit(1);
}

static void parse_arg (int key, char *arg)
{
	int v, i;

	switch(key) {
	case 'a':
		for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
			if (algo_names[i] &&
			    !strcmp(arg, algo_names[i])) {
				opt_algo = i;
				break;
			}
		}
		if (i == ARRAY_SIZE(algo_names))
			show_usage();
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 't':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage();

		opt_n_threads = v;
		break;
	case 1001:			/* --url */
		if (strncmp(arg, "http://", 7) &&
		    strncmp(arg, "https://", 8))
			show_usage();

		rpc_url = arg;
		break;
	case 1002:			/* --userpass */
		if (!strchr(arg, ':'))
			show_usage();

		userpass = arg;
		break;
	default:
		show_usage();
	}
}

static void parse_cmdline(int argc, char *argv[])
{
	int key;

	while (1) {
		key = getopt_long(argc, argv, "a:DPt:h?", options, NULL);
		if (key < 0)
			break;

		parse_arg(key, optarg);
	}
}

int main (int argc, char *argv[])
{
	int i;

	/* parse command line */
	parse_cmdline(argc, argv);

	/* set our priority to the highest (aka "nicest, least intrusive") */
	if (setpriority(PRIO_PROCESS, 0, 19))
		perror("setpriority");

	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		pthread_t t;

		if (pthread_create(&t, NULL, miner_thread,
				   (void *)(unsigned long) i)) {
			fprintf(stderr, "thread %d create failed\n", i);
			return 1;
		}

		sleep(1);	/* don't pound RPC server all at once */
	}

	fprintf(stderr, "%d miner threads started, "
		"using SHA256 '%s' algorithm.\n",
		opt_n_threads,
		algo_names[opt_algo]);

	/* main loop */
	while (program_running) {
		sleep(STAT_SLEEP_INTERVAL);
		/* do nothing */
	}

	return 0;
}

