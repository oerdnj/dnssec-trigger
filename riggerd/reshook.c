/*
 * reshook.c - dnssec-trigger resolv.conf hooks for adjusting name resolution 
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
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the unbound hooks for adjusting the name resolution
 * on the system (to 127.0.0.1).
 */
#include "config.h"
#include <sys/stat.h>
#include "reshook.h"
#include "log.h"
#include "cfg.h"
#include "probe.h"
#ifdef USE_WINSOCK
#include "winrc/win_svc.h"
#endif
#ifdef HAVE_CHFLAGS
#include <sys/stat.h>
#endif
static int set_to_localhost = 0;

#ifdef HOOKS_OSX
/** set the DNS the OSX way */
static void
set_dns_osx(struct cfg* cfg, char* iplist)
{
	char cmd[10240];
	char dm[1024];
	char* domains = "nothing.invalid";
	if(cfg->rescf_domain && cfg->rescf_domain[0])
		domains = cfg->rescf_domain;
	else if(cfg->rescf_search && cfg->rescf_search[0]) {
		snprintf(dm, sizeof(dm), "%s", cfg->rescf_search);
		domains = dm;
	}
	snprintf(cmd, sizeof(cmd), "%s/dnssec-trigger-setdns.sh mset %s -- %s",
		LIBEXEC_DIR, domains, iplist);
	verbose(VERB_QUERY, "%s", cmd);
	system(cmd);
}

/** restore resolv.conf on OSX if localhost is enabled */
void restore_resolv_osx(struct cfg* cfg)
{
	if(set_to_localhost)
		hook_resolv_localhost(cfg);
}

#endif /* HOOKS_OSX */

#ifndef USE_WINSOCK
static void prline(FILE* out, const char* line)
{
	int r = fprintf(out, "%s", line);
	if(r < 0) {
		log_err("cannot write resolvconf: %s", strerror(errno));
	} else if(r != (int)strlen(line)) {
		log_err("short write resolvconf: filesystem full?");
	}
}

#if defined(HAVE_CHFLAGS) && !defined(HOOKS_OSX)
static void r_mutable_bsd(const char* f)
{
	if(chflags(f, 0) < 0) {
		log_err("chflags(%s, nouchg) failed: %s", f, strerror(errno));
	}
}
static void r_immutable_bsd(const char* f)
{
	/* BSD method for immutable files */
	if(chflags(f, UF_IMMUTABLE|UF_NOUNLINK) < 0) {
		log_err("chflags(%s, uchg) failed: %s", f, strerror(errno));
	}
}
#elif !defined(HAVE_CHFLAGS) && !defined(HOOKS_OSX)
static void r_mutable_efs(const char* f)
{
	char buf[10240];
	snprintf(buf, sizeof(buf), "chattr -i %s", f);
	if(system(buf) < 0)
		log_err("could not %s: %s", buf, strerror(errno));
}
static void r_immutable_efs(const char* f)
{
	char buf[10240];
	/* this chattr only works on extX file systems */
	snprintf(buf, sizeof(buf), "chattr +i %s", f);
	if(system(buf) < 0)
		log_err("could not %s: %s", buf, strerror(errno));
}
#endif /* mutable/immutable on BSD and Linux */

static FILE* open_rescf(struct cfg* cfg)
{
	FILE* out;
	char line[1024];
#  if defined(HAVE_CHFLAGS) && !defined(HOOKS_OSX)
	r_mutable_bsd(cfg->resolvconf);
#  elif !defined(HAVE_CHFLAGS) && !defined(HOOKS_OSX)
	r_mutable_efs(cfg->resolvconf);
#  endif
	if(chmod(cfg->resolvconf, 0644)<0) {
		log_err("chmod(%s) failed: %s", cfg->resolvconf,
			strerror(errno));
	}
	/* make resolv.conf writable */
	out = fopen(cfg->resolvconf, "w");
	if(!out) {
		log_err("cannot open %s: %s", cfg->resolvconf, strerror(errno));
		return NULL;
	}
	prline(out, "# Generated by " PACKAGE_STRING "\n");
	if(cfg->rescf_domain) {
		snprintf(line, sizeof(line), "domain %s\n", cfg->rescf_domain);
		prline(out, line);
	}
	if(cfg->rescf_search) {
		snprintf(line, sizeof(line), "search %s\n", cfg->rescf_search);
		prline(out, line);
	}
	return out;
}

static void close_rescf(struct cfg* cfg, FILE* out)
{
	fclose(out);
	/* make resolv.conf readonly */
	if(chmod(cfg->resolvconf, 0444)<0) {
		log_err("chmod(%s) failed: %s", cfg->resolvconf,
			strerror(errno));
	}
#if defined(HAVE_CHFLAGS) && !defined(HOOKS_OSX)
	r_immutable_bsd(cfg->resolvconf);
#elif !defined(HAVE_CHFLAGS) && !defined(HOOKS_OSX)
	r_immutable_efs(cfg->resolvconf);
#endif
}
#endif /* !USE_WINSOCK */

#if !defined(USE_WINSOCK) && !defined(HOOKS_OSX)
/** check argument on line matches config option */
static int
check_line_arg(char* line, const char* opt)
{
	if(!opt) /* has opt in file but should not */
		return 0;
	if(strncmp(line, opt, strlen(opt)) != 0)
		/* file has wrong content */
		return 0;
	if(strcmp(line+strlen(opt), "\n") != 0)
		/* stuff after opt (too many domains) */
		return 0;
	return 1;
}

/** check if resolv.conf is set to 127.0.0.1 like we want */
static int
really_set_to_localhost(struct cfg* cfg) {
	FILE* in = fopen(cfg->resolvconf, "r");
	int saw_127 = 0, saw_search = 0, saw_domain = 0;
	char line[1024];
	if(!in) {
		verbose(VERB_DETAIL, "fopen %s: %s",
			cfg->resolvconf, strerror(errno));
		return 0;
	}
	if(!fgets(line, (int)sizeof(line), in)) {
		fclose(in); /* failed to read first line */
		return 0;
	}
	/* we want the first line to be 'Generated by me' */
	if(strcmp(line, "# Generated by " PACKAGE_STRING "\n") != 0) {
		fclose(in);
		return 0;
	}
	/* must contain 127.0.0.1 and nothing else */
	while(fgets(line, (int)sizeof(line), in)) {
		line[sizeof(line)-1] = 0; /* robust end of string */
		if(strcmp(line, "nameserver 127.0.0.1\n") == 0) {
			saw_127 = 1;
		} else if(strncmp(line, "nameserver", 10) == 0) {
			/* not 127.0.0.1 but in resolv.conf, bad! */
			fclose(in);
			return 0;
		} else if(strncmp(line, "search ", 7) == 0) {
			if(!check_line_arg(line+7, cfg->rescf_search)) {
				fclose(in);
				return 0;
			}
			saw_search = 1;
		} else if(strncmp(line, "domain ", 7) == 0) {
			if(!check_line_arg(line+7, cfg->rescf_domain)) {
				fclose(in);
				return 0;
			}
			saw_domain = 1;
		}
	}
	fclose(in);
	if(cfg->rescf_search && !saw_search)
		return 0;
	if(cfg->rescf_domain && !saw_domain)
		return 0;
	return saw_127;
}
#endif /* no USE_WINSOCK, no OSX */

void hook_resolv_localhost(struct cfg* cfg)
{
#ifndef USE_WINSOCK
	FILE* out;
#endif
	set_to_localhost = 1;
	if(cfg->noaction) {
		return;
	}
#ifdef HOOKS_OSX
	set_dns_osx(cfg, "127.0.0.1");
#endif
#ifdef USE_WINSOCK
	win_set_resolv("127.0.0.1");
#else /* not on windows */
#  ifndef HOOKS_OSX /* on Linux/BSD */
	if(really_set_to_localhost(cfg)) {
		/* already done, do not do it again, that would open
		 * a brief moment of mutable resolv.conf */
		verbose(VERB_ALGO, "resolv.conf localhost already set");
		return;
	}
	verbose(VERB_ALGO, "resolv.conf localhost write");
#  endif
	out = open_rescf(cfg);
	if(!out) return;
	/* write the nameserver records */
	prline(out, "nameserver 127.0.0.1\n");
	close_rescf(cfg, out);
#endif /* not on windows */
}

void hook_resolv_iplist(struct cfg* cfg, struct probe_ip* list)
{
#ifndef USE_WINSOCK
	char line[1024];
	FILE* out;
#endif
#if defined(HOOKS_OSX) || defined(USE_WINSOCK)
	char iplist[10240];
	iplist[0] = 0;
#endif
	set_to_localhost = 0;
	if(cfg->noaction)
		return;
#ifndef USE_WINSOCK
	out = open_rescf(cfg);
	if(!out) return;
#endif
	/* write the nameserver records */
	while(list) {
		if(probe_is_cache(list)) {
#ifndef USE_WINSOCK
			snprintf(line, sizeof(line), "nameserver %s\n",
				list->name);
			prline(out, line);
#endif
#if defined(HOOKS_OSX) || defined(USE_WINSOCK)
			snprintf(iplist+strlen(iplist),
				sizeof(iplist)-strlen(iplist), "%s%s",
				((iplist[0]==0)?"":" "), list->name);
#endif
		}
		list = list->next;
	}
#ifndef USE_WINSOCK
	close_rescf(cfg, out);
#endif
#ifdef HOOKS_OSX
	set_dns_osx(cfg, iplist);
#endif
#ifdef USE_WINSOCK
	win_set_resolv(iplist);
#endif
}

void hook_resolv_flush(struct cfg* cfg)
{
	/* attempt to flush OS specific caches, because we go from
	 * insecure to secure mode */
	(void)cfg;
#ifdef HOOKS_OSX
	/* dscacheutil on 10.5 an later, lookupd before that */
	system("dscacheutil -flushcache || lookupd -flushcache");
#elif defined(USE_WINSOCK)
	win_run_cmd("ipconfig /flushdns");
#else
	/* TODO */
#endif
}

#ifdef HOOKS_OSX
static void osx_uninit(void)
{
	char cmd[10240];
	snprintf(cmd, sizeof(cmd), "%s/dnssec-trigger-setdns.sh uninit",
		LIBEXEC_DIR);
	verbose(VERB_QUERY, "%s", cmd);
	system(cmd);
}
#endif /* HOOKS_OSX */

void hook_resolv_uninstall(struct cfg* cfg)
{
#ifdef HOOKS_OSX
	/* on OSX: do the OSX thing */
	(void)cfg;
	osx_uninit();
#elif defined(USE_WINSOCK)
	/* on Windows: edit registry */
	(void)cfg;
	win_clear_resolv();
#elif defined(HAVE_CHFLAGS)
	/* on BSD: make file mutable again */
	r_mutable_bsd(cfg->resolvconf);
#else
	/* other (unix) systems, make it via ext2fs mutable again */
	r_mutable_efs(cfg->resolvconf);
#endif
}
