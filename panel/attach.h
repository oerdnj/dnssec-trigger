/*
 * attach.h - dnssec-trigger acttachment from panel to daemon.
 *
 * Copyright (c) 2011, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the code that attaches the panel to the daemon.
 */

#ifndef ATTACH_H
#define ATTACH_H
struct feed;
struct cfg;
struct strlist;

/** attachment structure for the results read thread */
extern struct feed* feed;

/**
 * Alert arguments
 */
struct alert_arg {
	int last_insecure;
	int now_insecure;
	int now_dark;
	int now_cache;
	int now_auth;
	int now_disconn;
	int now_tcp;
	int now_ssl;
	int now_forced_insecure;
	int now_http_insecure;
};

/** structure for reading from the daemon */
struct feed {
	/* routine that locks a mutex for this structure */
	void (*lock)(void);
	/* routine that unlocks the mutex for this structure */
	void (*unlock)(void);
	/* quit the program, when stop is sent by triggerd */
	void (*quit)(void);
	/* alert function, new status information */
	void (*alert)(struct alert_arg*);
	/* update_alert function, new software update info, must free
	 * the passed (malloced) version string. */ 
	void (*update_alert)(char*);

	/* if connection with the daemon has been established. */
	int connected;
	/* non connection reason */
	char connect_reason[512];
	
	/* list of lines, last has status */
	struct strlist* results, *results_last;
	/* if we are in insecure mode - here to see if it has changed */
	int insecure_mode;

	/* list of lines for update status */
	struct strlist* update, *update_last;

	/* config */
	struct cfg* cfg;
	/* ssl context with keys */
	SSL_CTX* ctx;
	/* ssl to read results from */
	SSL* ssl_read;
	/* ssl to write results to */
	SSL* ssl_write;
};

/** create the feed structure and inits it
 * setups the global feed pointer.
 * Then you the caller must fill the function pointers in the struct
 * with proper callbacks.  Then call attach_start from a fresh thread.
 */
void attach_create(void);

/** delete feed structure.
 */
void attach_delete(void);

/** start the connection thread */
void attach_start(struct cfg* cfg);

/** stop attach */
void attach_stop(void);

/** send insecure choice to the daemon */
void attach_send_insecure(int val);
void attach_send_reprobe(void);
void attach_send_hotspot_signon(void);
void attach_send_skip_http(void);
void attach_send_update_cancel(void);
void attach_send_update_ok(void);

/** get tooltip text from alert state (fixed string) */
const char* state_tooltip(struct alert_arg* a);
/** 
 * process state for new alert (at GUI side)
 * @param a: the alert state info.
 * @param unsafe_asked: 1 if user chose something in the unsafe dialog.
 * @param noweb_asked: 1 if user chose something in the noweb dialog.
 * @param danger: routine to show danger icon
 * @param safe: routine to show safe icon.
 * @param dialog: routine to show the insecure-question dialog.
 * @param noweb: routine to whot the noweb-login dialog.
 */
void process_state(struct alert_arg* a, int* unsafe_asked, int* noweb_asked,
	void (*danger)(void), void(*safe)(void), void(*dialog)(void),
	void (*noweb)(void));

/**
 * Fetch proberesults text.
 * @param buf: buffer for string.
 * @param len: length of buffer.
 * @param lf: line ending (string), e.g. "\n"
 */
void fetch_proberesults(char* buf, size_t len, const char* lf);

/**
 * forkexec login command given cfg, where forkexec exists (unix)
 * Takes cfg item from feed global. logs errors to syslog.
 */
void run_login(void);

#endif /* ATTACH_H */
