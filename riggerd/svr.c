/*
 * svr.c - dnssec-trigger server implementation
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
 * This file contains the server implementation.
 */
#include "config.h"
#include "svr.h"
#include "cfg.h"
#include "log.h"
#include "http.h"
#include "probe.h"
#include "netevent.h"
#include "net_help.h"
#include "reshook.h"
#include "update.h"
#ifdef USE_WINSOCK
#include "winsock_event.h"
#endif

struct svr* global_svr = NULL;

static int setup_ssl_ctx(struct svr* svr);
static int setup_listen(struct svr* svr);
static void sslconn_delete(struct sslconn* sc);
static int sslconn_readline(struct sslconn* sc);
static int sslconn_write(struct sslconn* sc);
static int sslconn_checkclose(struct sslconn* sc);
static void sslconn_shutdown(struct sslconn* sc);
static void sslconn_command(struct sslconn* sc);
static void sslconn_persist_command(struct sslconn* sc);
static void send_results_to_con(struct svr* svr, struct sslconn* s);

struct svr* svr_create(struct cfg* cfg)
{
	struct svr* svr = (struct svr*)calloc(1, sizeof(*svr));
	if(!svr) return NULL;
	global_svr = svr;
	svr->max_active = 32;
	svr->cfg = cfg;
	svr->base = comm_base_create(0);
	ldns_init_random(NULL, 0);
	if(!svr->base) {
		log_err("cannot create base");
		svr_delete(svr);
		return NULL;
	}
	svr->udp_buffer = ldns_buffer_new(65553);
	if(!svr->udp_buffer) {
		log_err("out of memory");
		svr_delete(svr);
		return NULL;
	}
	svr->retry_timer = comm_timer_create(svr->base, &svr_retry_callback,
		svr);
	svr->tcp_timer = comm_timer_create(svr->base, &svr_tcp_callback, svr);
	if(!svr->retry_timer || !svr->tcp_timer) {
		log_err("out of memory");
		svr_delete(svr);
		return NULL;
	}
	if(cfg->check_updates) {
		svr->update = selfupdate_create(svr, cfg);
		if(!svr->update) {
			log_err("out of memory");
			svr_delete(svr);
			return NULL;
		}
	}

	/* setup SSL_CTX */
	if(!setup_ssl_ctx(svr)) {
		log_err("cannot setup SSL context");
		svr_delete(svr);
		return NULL;
	}
	/* create listening */
	if(!setup_listen(svr)) {
		log_err("cannot setup listening socket");
		svr_delete(svr);
		return NULL;
	}

	return svr;
}

void svr_delete(struct svr* svr)
{
	struct listen_list* ll, *nll;
	if(!svr) return;
	/* delete busy */
	while(svr->busy_list) {
		(void)SSL_shutdown(svr->busy_list->ssl);
		sslconn_delete(svr->busy_list);
	}

	/* delete listening */
	ll = svr->listen;
	while(ll) {
		nll = ll->next;
		comm_point_delete(ll->c);
		free(ll);
		ll=nll;
	}

	/* delete probes */
	probe_list_delete(svr->probes);

	if(svr->ctx) {
		SSL_CTX_free(svr->ctx);
	}
	selfupdate_delete(svr->update);
	ldns_buffer_free(svr->udp_buffer);
	comm_timer_delete(svr->retry_timer);
	comm_timer_delete(svr->tcp_timer);
	http_general_delete(svr->http);
	comm_base_delete(svr->base);
	free(svr);
}

static int setup_ssl_ctx(struct svr* s)
{
	char* s_cert;
	char* s_key;
	s->ctx = SSL_CTX_new(SSLv23_server_method());
	if(!s->ctx) {
		log_crypto_err("could not SSL_CTX_new");
		return 0;
	}
	/* no SSLv2 because has defects */
#if OPENSSL_VERSION_NUMBER < 0x10100000
	if(!(SSL_CTX_set_options(s->ctx, SSL_OP_NO_SSLv2) & SSL_OP_NO_SSLv2)){
		log_crypto_err("could not set SSL_OP_NO_SSLv2");
		return 0;
	}
#endif
	s_cert = s->cfg->server_cert_file;
	s_key = s->cfg->server_key_file;
	verbose(VERB_ALGO, "setup SSL certificates");
	if(!SSL_CTX_use_certificate_file(s->ctx,s_cert,SSL_FILETYPE_PEM)) {
		log_err("Error for server-cert-file: %s", s_cert);
		log_crypto_err("Error in SSL_CTX use_certificate_file");
		return 0;
	}
	if(!SSL_CTX_use_PrivateKey_file(s->ctx,s_key,SSL_FILETYPE_PEM)) {
		log_err("Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX use_PrivateKey_file");
		return 0;
	}
	if(!SSL_CTX_check_private_key(s->ctx)) {
		log_err("Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX check_private_key");
		return 0;
	}
	if(!SSL_CTX_load_verify_locations(s->ctx, s_cert, NULL)) {
		log_crypto_err("Error setting up SSL_CTX verify locations");
		return 0;
	}
	SSL_CTX_set_client_CA_list(s->ctx, SSL_load_client_CA_file(s_cert));
	SSL_CTX_set_verify(s->ctx, SSL_VERIFY_PEER, NULL);
	return 1;
}

static int setup_listen(struct svr* svr)
{
	const char* str="127.0.0.1";
	struct sockaddr_storage addr;
	socklen_t len;
	int s;
	int fam;
	struct listen_list* e;
#if defined(SO_REUSEADDR) || defined(IPV6_V6ONLY)
	int on = 1;
#endif
	if(!ipstrtoaddr(str, svr->cfg->control_port, &addr, &len)) {
		log_err("cannot parse ifname %s", str);
		return 0;
	}
	if(strchr(str, ':')) fam = AF_INET6;
	else fam = AF_INET;
	s = socket(fam, SOCK_STREAM, 0);
	if(s == -1) {
		log_err("socket %s: %s", str, strerror(errno));
		return 0;
	}
#ifdef SO_REUSEADDR
	if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&on,
		(socklen_t)sizeof(on)) < 0) {
		log_err("setsockopt(.. SO_REUSEADDR ..) failed: %s",
			strerror(errno));
	}
#endif
#if defined(IPV6_V6ONLY)
	if(fam == AF_INET6) {
		if(setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			log_err("setsockopt(..., IPV6_V6ONLY, ...) failed: %s",
				strerror(errno));
		}
	}
#endif
	if(bind(s, (struct sockaddr*)&addr, len) != 0) {
		fatal_exit("can't bind tcp socket %s: %s", str, strerror(errno));
	}
	fd_set_nonblock(s);
	if(listen(s, 15) == -1) {
		log_err("can't listen: %s", strerror(errno));
	}
	/* add entry */
	e = (struct listen_list*)calloc(1, sizeof(*e));
	if(!e) {
		fatal_exit("out of memory");
	}
	e->c = comm_point_create_raw(svr->base, s, 0, handle_ssl_accept, NULL);
	e->c->do_not_close = 0;
	e->next = svr->listen;
	svr->listen = e;
	return 1;
}

void svr_service(struct svr* svr)
{
	comm_base_dispatch(svr->base);
}

static void sslconn_delete(struct sslconn* sc)
{
	struct sslconn** pp;
	if(!sc) return;
	/* delete and remove from busylist */
	for(pp = &global_svr->busy_list; *pp; pp = &((*pp)->next)) {
		if((*pp) == sc) {
			(*pp) = sc->next;
			break;
		}
	}
	global_svr->active--;
	if(sc->buffer)
		ldns_buffer_free(sc->buffer);
	comm_point_delete(sc->c);
	if(sc->ssl)
		SSL_free(sc->ssl);
	free(sc);
}

int handle_ssl_accept(struct comm_point* c, void* ATTR_UNUSED(arg), int err,
	struct comm_reply* ATTR_UNUSED(reply_info))
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int s;
	struct svr* svr = global_svr;
	struct sslconn* sc;
        if(err != NETEVENT_NOERROR) {
                log_err("error %d on remote_accept_callback", err);
                return 0;
        }
        /* perform the accept */
        s = comm_point_perform_accept(c, &addr, &addrlen);
        if(s == -1)
                return 0;
        /* create new commpoint unless we are servicing already */
        if(svr->active >= svr->max_active) {
                log_warn("drop incoming remote control: too many connections");
        close_exit:
#ifndef USE_WINSOCK
                close(s);
#else
                closesocket(s);
#endif
                return 0;
        }

	/* setup commpoint to service the remote control command */
        sc = (struct sslconn*)calloc(1, sizeof(*sc));
        if(!sc) {
                log_err("out of memory");
                goto close_exit;
        }

	/* start in reading state */
        sc->c = comm_point_create_raw(svr->base, s, 0, &control_callback, sc);
        if(!sc->c) {
                log_err("out of memory");
                free(sc);
                goto close_exit;
        }
	log_addr(VERB_QUERY, "new control connection from", &addr, addrlen);

	sc->c->do_not_close = 0;
	/* no timeout on the connection: the panel stays connected for long */
	memcpy(&sc->c->repinfo.addr, &addr, addrlen);
        sc->c->repinfo.addrlen = addrlen;
        sc->shake_state = rc_hs_read;
        sc->ssl = SSL_new(svr->ctx);
        if(!sc->ssl) {
                log_crypto_err("could not SSL_new");
		comm_point_delete(sc->c);
                free(sc);
		return 0;
        }
        SSL_set_accept_state(sc->ssl);
        (void)SSL_set_mode(sc->ssl, SSL_MODE_AUTO_RETRY);
        if(!SSL_set_fd(sc->ssl, s)) {
                log_crypto_err("could not SSL_set_fd");
                SSL_free(sc->ssl);
                comm_point_delete(sc->c);
                free(sc);
		return 0;
        }
#ifdef USE_WINSOCK
	comm_point_tcp_win_bio_cb(sc->c, sc->ssl);
#endif
	sc->buffer = ldns_buffer_new(65536);
	if(!sc->buffer) {
		log_err("out of memory");
                SSL_free(sc->ssl);
                comm_point_delete(sc->c);
                free(sc);
		return 0;
	}
        sc->next = svr->busy_list;
        svr->busy_list = sc;
        svr->active ++;

        /* perform the first nonblocking read already, for windows, 
         * so it can return wouldblock. could be faster too. */
        (void)control_callback(sc->c, sc, NETEVENT_NOERROR, NULL);
	return 0;
}

int control_callback(struct comm_point* c, void* arg, int err,
	struct comm_reply* ATTR_UNUSED(reply_info))
{
        struct sslconn* s = (struct sslconn*)arg;
        int r;
        if(err != NETEVENT_NOERROR) {
                if(err==NETEVENT_TIMEOUT)
                        log_err("remote control timed out");
		sslconn_delete(s);
                return 0;
        }
        /* (continue to) setup the SSL connection */
	if(s->shake_state == rc_hs_read || s->shake_state == rc_hs_write) {
		ERR_clear_error();
		r = SSL_do_handshake(s->ssl);
		if(r != 1) {
			int r2 = SSL_get_error(s->ssl, r);
			if(r2 == SSL_ERROR_WANT_READ) {
				if(s->shake_state == rc_hs_read) {
					/* try again later */
					return 0;
				}
				s->shake_state = rc_hs_read;
				comm_point_listen_for_rw(c, 1, 0);
				return 0;
			} else if(r2 == SSL_ERROR_WANT_WRITE) {
				if(s->shake_state == rc_hs_write) {
					/* try again later */
					return 0;
				}
				s->shake_state = rc_hs_write;
				comm_point_listen_for_rw(c, 0, 1);
				return 0;
			} else if(r2 == SSL_ERROR_SYSCALL) {
				if(ERR_peek_error()) {
					char errbuf[128];
					ERR_error_string_n(ERR_get_error(),
						errbuf, sizeof(errbuf));
					log_err("ssl_handshake: %s", errbuf);
				} else if(r == 0) {
					log_err("ssl_handshake EOF violation");
				} else if(r == -1) {
#ifdef USE_WINSOCK
					log_err("ssl_handshake syscall: "
						"%s, wsa: %s", strerror(errno),
						wsa_strerror(WSAGetLastError()));
#else
					log_err("ssl_handshake syscall: %s",
						strerror(errno));
#endif
				} else	log_err("ssl_handshake syscall ret %d",
						r);
				sslconn_delete(s);
				return 0;
			} else {
				if(r == 0)
					log_err("remote control connection closed prematurely");
				log_addr(1, "failed connection from",
					&s->c->repinfo.addr, s->c->repinfo.addrlen);
				log_crypto_err("remote control failed ssl");
				sslconn_delete(s);
				return 0;
			}
		}
		/* once handshake has completed, check authentication */
		if(SSL_get_verify_result(s->ssl) == X509_V_OK) {
			X509* x = SSL_get_peer_certificate(s->ssl);
			if(!x) {
				verbose(VERB_DETAIL, "remote control connection "
					"provided no client certificate");
				sslconn_delete(s);
				return 0;
			}
			verbose(VERB_ALGO, "remote control connection authenticated");
			X509_free(x);
		} else {
			verbose(VERB_DETAIL, "remote control connection failed to "
				"authenticate with client certificate");
			sslconn_delete(s);
			return 0;
		}
		/* set to read state */
		s->line_state = command_read;
		if(s->shake_state == rc_hs_write)
			comm_point_listen_for_rw(c, 1, 0);
		s->shake_state = rc_hs_none;
		ldns_buffer_clear(s->buffer);
	} else if(s->shake_state == rc_hs_want_write) {
		/* we have satisfied the condition that the socket is
		 * writable, remove the handshake state, and continue */
		comm_point_listen_for_rw(c, 1, 0); /* back to reading */
		s->shake_state = rc_hs_none;
	} else if(s->shake_state == rc_hs_want_read) {
		/* we have satisfied the condition that the socket is
		 * readable, remove the handshake state, and continue */
		comm_point_listen_for_rw(c, 1, 1); /* back to writing */
		s->shake_state = rc_hs_none;
	} else if(s->shake_state == rc_hs_shutdown) {
		sslconn_shutdown(s);
	}

	if(s->line_state == command_read) {
		if(!sslconn_readline(s))
			return 0;
		/* we are done handle it */
		sslconn_command(s);
	} else if(s->line_state == persist_read) {
		do {
			if(!sslconn_readline(s))
				return 0;
			/* we are done handle it */
			sslconn_persist_command(s);
			/* there may be more to read in the same SSL packet */
		} while(SSL_pending(s->ssl) != 0);
	} else if(s->line_state == persist_write) {
		if(sslconn_checkclose(s))
			return 0;
		if(!sslconn_write(s))
			return 0;
		if(s->fetch_another_update) {
			s->fetch_another_update = 0;
			send_results_to_con(global_svr, s);
			return 0;
		}
		/* nothing more to write */
		if(s->close_me) {
			sslconn_shutdown(s);
			return 0;
		}
		comm_point_listen_for_rw(c, 1, 0);
		s->line_state = persist_write_checkclose;
	} else if(s->line_state == persist_write_checkclose) {
		(void)sslconn_checkclose(s);
	}
	return 0;
}

static int sslconn_readline(struct sslconn* sc)
{
        int r;
	while(ldns_buffer_available(sc->buffer, 1)) {
		ERR_clear_error();
		if((r=SSL_read(sc->ssl, ldns_buffer_current(sc->buffer), 1))
			<= 0) {
			int want = SSL_get_error(sc->ssl, r);
			if(want == SSL_ERROR_ZERO_RETURN) {
				sslconn_shutdown(sc);
				return 0;
			} else if(want == SSL_ERROR_WANT_READ) {
				return 0;
			} else if(want == SSL_ERROR_WANT_WRITE) {
				sc->shake_state = rc_hs_want_write;
				comm_point_listen_for_rw(sc->c, 0, 1);
				return 0;
			} else if(want == SSL_ERROR_SYSCALL) {
				if(ERR_peek_error()) {
					char errbuf[128];
					ERR_error_string_n(ERR_get_error(),
						errbuf, sizeof(errbuf));
					log_err("ssl_read: %s", errbuf);
				} else if(r == 0) {
					log_err("ssl_read EOF violation");
				} else if(r == -1) {
#ifdef USE_WINSOCK
					int wsar = WSAGetLastError();
					/* conn reset common at restarts */
					if(wsar == WSAECONNRESET)
						verbose(VERB_ALGO,
							"ssl_read syscall: %s",
							wsa_strerror(wsar));
					else log_err("ssl_read syscall: "
						"%s, wsa: %s", strerror(errno),
						wsa_strerror(wsar));
#else
					log_err("ssl_read syscall: %s",
						strerror(errno));
#endif
				} else	log_err("ssl_read syscall ret %d", r);
				sslconn_delete(sc);
				return 0;
			}
			log_crypto_err("could not SSL_read");
			sslconn_delete(sc);
			return 0;
		}
		if(ldns_buffer_current(sc->buffer)[0] == '\n') {
			/* return string without \n */
			ldns_buffer_write_u8(sc->buffer, 0);
			ldns_buffer_flip(sc->buffer);
			return 1;
		}
		ldns_buffer_skip(sc->buffer, 1);
	}
	log_err("ssl readline too long");
	sslconn_delete(sc);
	return 0;
}

static int sslconn_write(struct sslconn* sc)
{
        int r;
	/* ignore return, if fails we may simply block */
	(void)SSL_set_mode(sc->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
	while(ldns_buffer_remaining(sc->buffer)>0) {
		ERR_clear_error();
		if((r=SSL_write(sc->ssl, ldns_buffer_current(sc->buffer), 
			(int)ldns_buffer_remaining(sc->buffer)))
			<= 0) {
			int want = SSL_get_error(sc->ssl, r);
			if(want == SSL_ERROR_ZERO_RETURN) {
				/* the other side has closed the channel */
				verbose(VERB_ALGO, "result write closed");
				sslconn_delete(sc);
				return 0;
			} else if(want == SSL_ERROR_WANT_READ) {
				sc->shake_state = rc_hs_want_read;
				comm_point_listen_for_rw(sc->c, 1, 0);
				return 0;
			} else if(want == SSL_ERROR_WANT_WRITE) {
				return 0;
			} else if(want == SSL_ERROR_SYSCALL) {
				if(ERR_peek_error()) {
					char errbuf[128];
					ERR_error_string_n(ERR_get_error(),
						errbuf, sizeof(errbuf));
					log_err("ssl_write: %s", errbuf);
				} else if(r == 0) {
					log_err("ssl_write EOF violation");
				} else if(r == -1) {
#ifdef USE_WINSOCK
					log_err("ssl_write syscall: "
						"%s, wsa: %s", strerror(errno),
						wsa_strerror(WSAGetLastError()));
#else
					log_err("ssl_write syscall: %s",
						strerror(errno));
#endif
				} else	log_err("ssl_write syscall ret %d", r);
				sslconn_delete(sc);
				return 0;
			}
			log_crypto_err("could not SSL_write");
			/* the other side has closed the channel */
			sslconn_delete(sc);
			return 0;
		}
		ldns_buffer_skip(sc->buffer, (ssize_t)r);
	}
	/* done writing the buffer. */
	return 1;
}

static void sslconn_shutdown(struct sslconn* sc)
{
	int r = SSL_shutdown(sc->ssl);
	if(r > 0) {
		sslconn_delete(sc);
	} else if(r == 0) {
		/* we do not need to get notify from the peer, since we
		 * close the fd */
		sslconn_delete(sc);
	} else {
		int want = SSL_get_error(sc->ssl, r);
		sc->shake_state = rc_hs_shutdown;
		if(want == SSL_ERROR_ZERO_RETURN) {
			sslconn_delete(sc);
		} else if(want == SSL_ERROR_WANT_READ) {
			comm_point_listen_for_rw(sc->c, 1, 0);
		} else if(want == SSL_ERROR_WANT_WRITE) {
			comm_point_listen_for_rw(sc->c, 0, 1);
		} else {
			log_crypto_err("could not SSL_shutdown");
			sslconn_delete(sc);
		}
	}

}

static int sslconn_checkclose(struct sslconn* sc)
{
	int r;
	ERR_clear_error();
	if((r=SSL_read(sc->ssl, NULL, 0)) <= 0) {
		int want = SSL_get_error(sc->ssl, r);
		if(want == SSL_ERROR_ZERO_RETURN) {
			verbose(VERB_ALGO, "checked channel closed otherside");
			sslconn_shutdown(sc);
			return 1;
		} else if(want == SSL_ERROR_WANT_READ) {
			return 0;
		} else if(want == SSL_ERROR_WANT_WRITE) {
			sc->shake_state = rc_hs_want_write;
			comm_point_listen_for_rw(sc->c, 0, 1);
			return 0;
		} else if(want == SSL_ERROR_SYSCALL) {
			if(ERR_peek_error()) {
				char errbuf[128];
				ERR_error_string_n(ERR_get_error(),
					errbuf, sizeof(errbuf));
				log_err("checkclose ssl_read: %s", errbuf);
			} else if(r == 0) {
				log_err("checkclose ssl_read EOF violation");
			} else if(r == -1) {
#ifdef USE_WINSOCK
				int wsar = WSAGetLastError();
				/* connreset common at restart of system */
				if(wsar == WSAECONNRESET)
				    verbose(VERB_ALGO,
					"checkclose ssl_read syscall: %s",
					wsa_strerror(wsar));
				else log_err("checkclose ssl_read syscall: "
					"%s, wsa: %s", strerror(errno),
					wsa_strerror(WSAGetLastError()));
#else
				log_err("checkclose ssl_read syscall: %s",
					strerror(errno));
#endif
			} else	log_err("checkclose ssl_read syscall ret %d",
					r);
			sslconn_delete(sc);
			return 0;
		}
		log_crypto_err("checkclose could not SSL_read");
		sslconn_delete(sc);
		return 1;
	}
	if(SSL_get_shutdown(sc->ssl)) {
		verbose(VERB_ALGO, "checked channel closed");
		sslconn_delete(sc);
		return 1;
	}
	return 0;
}

static void persist_cmd_insecure(int val)
{
	struct svr* svr = global_svr;
	int was_insecure = svr->insecure_state;
	svr->insecure_state = val;
	/* see if we need to change unbound's settings */
	if(svr->res_state == res_dark) {
		if(!was_insecure && val) {
			/* set resolv.conf to the DHCP IP list */
			hook_resolv_iplist(svr->cfg, svr->probes);
		} else if(was_insecure && !val) {
			/* set resolv.conf to 127.0.0.1 */
			hook_resolv_localhost(svr->cfg);
		}
	} else {
		/* no need for insecure; robustness, in case some delayed
		 * command arrives when we have reprobed again */
		if(!svr->forced_insecure)
			svr->insecure_state = 0;
	}
	svr_send_results(svr);
}

void cmd_reprobe(void)
{
	char buf[10240];
	char* now = buf;
	size_t left = sizeof(buf);
	struct probe_ip* p;
	buf[0]=0; /* safe, robust */
	for(p = global_svr->probes; p; p = p->next) {
		if(probe_is_cache(p)) {
			size_t len;
			if(left < strlen(p->name)+3)
				break; /* no space for more */
			snprintf(now, left, "%s%s",
				(now==buf)?"":" ", p->name);
			len = strlen(now);
			left -= len;
			now += len;
		}
	}
	probe_start(buf);
}

static void handle_hotspot_signon_cmd(struct svr* svr)
{
	verbose(VERB_OPS, "state dark forced_insecure");
	probe_setup_hotspot_signon(svr);
	svr_send_results(svr);
}

static void handle_skip_http_cmd(void)
{
	verbose(VERB_OPS, "state skip_http and reprobe");
	global_svr->skip_http = 1;
	cmd_reprobe();
}

static void sslconn_persist_command(struct sslconn* sc)
{
	char* str = (char*)ldns_buffer_begin(sc->buffer);
	while(*str == ' ')
		str++;
	verbose(VERB_ALGO, "persist-channel command: %s", str);
	if(*str == 0) {
		/* ignore empty lines */
	} else if(strcmp(str, "insecure yes") == 0) {
		persist_cmd_insecure(1);
	} else if(strcmp(str, "insecure no") == 0) {
		persist_cmd_insecure(0);
	} else if(strcmp(str, "reprobe") == 0) {
		global_svr->forced_insecure = 0;
		global_svr->http_insecure = 0;
		cmd_reprobe();
	} else if(strcmp(str, "skip_http") == 0) {
		handle_skip_http_cmd();
	} else if(strcmp(str, "hotspot_signon") == 0) {
		handle_hotspot_signon_cmd(global_svr);
	} else if(strcmp(str, "update_cancel") == 0) {
		selfupdate_userokay(global_svr->update, 0);
	} else if(strcmp(str, "update_ok") == 0) {
		selfupdate_userokay(global_svr->update, 1);
	} else {
		log_err("unknown command from panel: %s", str);
	}
	/* and ready to read the next command */
	ldns_buffer_clear(sc->buffer);
}

static void handle_submit(char* ips)
{
	/* start probing the servers */
	probe_start(ips);
}

/** append update signal to buffer to send */
static void
append_update_to_con(struct sslconn* s, char* version_available)
{
	ldns_buffer_printf(s->buffer, "update %s\n%s\n\n", PACKAGE_VERSION,
		version_available);
}

static void
send_results_to_con(struct svr* svr, struct sslconn* s)
{
	struct probe_ip* p;
	char at[32];
	int numcache = 0, unfinished = 0;
	ldns_buffer_clear(s->buffer);
	if(svr->probetime == 0)
		ldns_buffer_printf(s->buffer, "at (no probe performed)\n");
	else if(strftime(at, sizeof(at), "%Y-%m-%d %H:%M:%S",
		localtime(&svr->probetime)))
		ldns_buffer_printf(s->buffer, "at %s\n", at);
	for(p=svr->probes; p; p=p->next) {
		if(probe_is_cache(p))
			numcache++;
		if(!p->finished) {
			unfinished++;
			continue;
		}
		if(p->to_http) {
			if(p->host_c) {
		    	ldns_buffer_printf(s->buffer, "%s %s %s from %s: %s %s\n",
		    		"addr", p->host_c->qname,
				p->http_ip6?"AAAA":"A", p->name,
				p->works?"OK":"error", p->reason?p->reason:"");
			} else
		    	    ldns_buffer_printf(s->buffer, "%s %s (%s): %s %s\n",
		    		"http", p->http_desc, p->name,
				p->works?"OK":"error", p->reason?p->reason:"");
		} else if(p->dnstcp)
		    ldns_buffer_printf(s->buffer, "%s%d %s: %s %s\n",
		        p->ssldns?"ssl":"tcp", p->port, p->name,
			p->works?"OK":"error", p->reason?p->reason:"");
		else
		    ldns_buffer_printf(s->buffer, "%s %s: %s %s\n",
			p->to_auth?"authority":"cache", p->name,
			p->works?"OK":"error", p->reason?p->reason:"");
	}
	if(unfinished)
		ldns_buffer_printf(s->buffer, "probe is in progress\n");
	else if(!numcache)
		ldns_buffer_printf(s->buffer, "no cache: no DNS servers have been supplied via DHCP\n");

	ldns_buffer_printf(s->buffer, "state: %s %s%s%s\n",
		svr->res_state==res_cache?"cache":(
		svr->res_state==res_tcp?"tcp":(
		svr->res_state==res_ssl?"ssl":(
		svr->res_state==res_auth?"auth":(
		svr->res_state==res_disconn?"disconnected":"nodnssec")))),
		svr->insecure_state?"insecure_mode":"secure",
		svr->forced_insecure?" forced_insecure":"",
		svr->http_insecure?" http_insecure":""
		);
	ldns_buffer_printf(s->buffer, "\n");
	if(svr->update && svr->update->update_available &&
		!svr->update->user_replied) {
		log_info("append_update signal");
		append_update_to_con(s, svr->update->version_available);
	}
	ldns_buffer_flip(s->buffer);
	comm_point_listen_for_rw(s->c, 1, 1);
	s->line_state = persist_write;
}

void svr_signal_update(struct svr* svr, char* version_available)
{
	/* write stop to all connected panels */
	struct sslconn* s;
	for(s=svr->busy_list; s; s=s->next) {
		if(s->line_state == persist_write) {
			/* busy with last results, fetch update later */
			s->fetch_another_update=1;
		}
		if(s->line_state == persist_write_checkclose) {
			ldns_buffer_clear(s->buffer);
			append_update_to_con(s, version_available);
			ldns_buffer_flip(s->buffer);
			comm_point_listen_for_rw(s->c, 1, 1);
			s->line_state = persist_write;
		}
	}
}

static void handle_results_cmd(struct sslconn* sc)
{
	/* turn into persist write with results. */
	ldns_buffer_clear(sc->buffer);
	ldns_buffer_flip(sc->buffer);
	/* must listen for close of connection: reading */
	comm_point_listen_for_rw(sc->c, 1, 0);
	sc->line_state = persist_write_checkclose;
	/* feed it the first results (if any) */
	send_results_to_con(global_svr, sc);
}

static void handle_status_cmd(struct sslconn* sc)
{
	sc->close_me = 1;
	handle_results_cmd(sc);
}

static void handle_printclose(struct sslconn* sc, char* str)
{
	/* write and then close */
	sc->close_me = 1;
	comm_point_listen_for_rw(sc->c, 1, 1);
	sc->line_state = persist_write;
	/* enter contents */
	ldns_buffer_clear(sc->buffer);
	ldns_buffer_printf(sc->buffer, "%s\n", str);
	ldns_buffer_flip(sc->buffer);
}

static void handle_cmdtray_cmd(struct sslconn* sc)
{
#ifdef HOOKS_OSX
	/* OSX has messed up resolv.conf after relogin */
	restore_resolv_osx(global_svr->cfg);
#endif
	/* turn into persist read */
	ldns_buffer_clear(sc->buffer);
	comm_point_listen_for_rw(sc->c, 1, 0);
	sc->line_state = persist_read;
}

static void handle_unsafe_cmd(struct sslconn* sc)
{
	probe_unsafe_test();
	sslconn_shutdown(sc);
}

static void handle_test_tcp_cmd(struct sslconn* sc)
{
	probe_tcp_test();
	sslconn_shutdown(sc);
}

static void handle_test_ssl_cmd(struct sslconn* sc)
{
	probe_ssl_test();
	sslconn_shutdown(sc);
}

static void handle_test_http_cmd(struct sslconn* sc)
{
	probe_http_test();
	sslconn_shutdown(sc);
}

static void handle_test_update_cmd(struct sslconn* sc)
{
	global_svr->update->test_flag = 1;
	global_svr->update_desired = 1;
	sslconn_shutdown(sc);
}

static void handle_stoppanels_cmd(struct sslconn* sc)
{
	/* write stop to all connected panels */
	const char* stopcmd = "stop\n";
	struct sslconn* s;
	for(s=global_svr->busy_list; s; s=s->next) {
		/* skip non persistent-write connections */
		if(s->line_state != persist_write_checkclose &&
			s->line_state != persist_write)
			continue;
		(void)SSL_set_mode(s->ssl, SSL_MODE_AUTO_RETRY);
		if(SSL_get_fd(s->ssl) != -1) {
#ifdef USE_WINSOCK
			/* to be able to set it back to blocking mode
			 * we have to remove the EventSelect on it */
			if(WSAEventSelect(SSL_get_fd(s->ssl), NULL, 0)!=0)
				log_err("WSAEventSelect disable: %s",
					wsa_strerror(WSAGetLastError()));
#endif
			fd_set_block(SSL_get_fd(s->ssl));
		}
		if(s->line_state == persist_write) {
			/* busy with last results,  blocking write them */
			if(SSL_write(s->ssl, ldns_buffer_current(s->buffer), 
				(int)ldns_buffer_remaining(s->buffer)) < 0)
				log_crypto_err("cannot SSL_write remainder");
		}
		/* blocking write the stop command */
		if(SSL_write(s->ssl, stopcmd, (int)strlen(stopcmd)) < 0)
			log_crypto_err("cannot SSL_write panel stop");

		if(!SSL_get_shutdown(s->ssl)) {
			if(SSL_shutdown(s->ssl) == 0)
				SSL_shutdown(s->ssl); /* again to wait */
		}

		/* it will be closed now */
		if(SSL_get_fd(s->ssl) != -1)
			fd_set_nonblock(SSL_get_fd(s->ssl));
		comm_point_listen_for_rw(s->c, 1, 0);
		s->line_state = persist_write_checkclose;
	}
	/* wait until they all stopped, then stop commanding connection */
	sslconn_shutdown(sc);
}

static void sslconn_command(struct sslconn* sc)
{
	char header[10];
	char* str = (char*)ldns_buffer_begin(sc->buffer);
	snprintf(header, sizeof(header), "DNSTRIG%d ", CONTROL_VERSION);
	if(strncmp(str, header, strlen(header)) != 0) {
		log_err("bad version in control connection");
		sslconn_delete(sc);
		return;
	}
	str += strlen(header);
	while(*str == ' ')
		str++;
	verbose(VERB_ALGO, "command: %s", str);
	if(strncmp(str, "submit", 6) == 0) {
		handle_submit(str+6);
		sslconn_shutdown(sc);
	} else if(strncmp(str, "reprobe", 7) == 0) {
		global_svr->forced_insecure = 0;
		global_svr->http_insecure = 0;
		cmd_reprobe();
		sslconn_shutdown(sc);
	} else if(strncmp(str, "skip_http", 9) == 0) {
		handle_skip_http_cmd();
		sslconn_shutdown(sc);
	} else if(strncmp(str, "hotspot_signon", 14) == 0) {
		handle_hotspot_signon_cmd(global_svr);
		sslconn_shutdown(sc);
	} else if(strncmp(str, "results", 7) == 0) {
		handle_results_cmd(sc);
	} else if(strncmp(str, "status", 7) == 0) {
		handle_status_cmd(sc);
	} else if(strncmp(str, "cmdtray", 7) == 0) {
		handle_cmdtray_cmd(sc);
	} else if(strncmp(str, "unsafe", 6) == 0) {
		handle_unsafe_cmd(sc);
	} else if(strncmp(str, "test_tcp", 8) == 0) {
		handle_test_tcp_cmd(sc);
	} else if(strncmp(str, "test_ssl", 8) == 0) {
		handle_test_ssl_cmd(sc);
	} else if(strncmp(str, "test_http", 8) == 0) {
		handle_test_http_cmd(sc);
	} else if(strncmp(str, "test_update", 11) == 0) {
		handle_test_update_cmd(sc);
	} else if(strncmp(str, "stoppanels", 10) == 0) {
		handle_stoppanels_cmd(sc);
	} else if(strncmp(str, "stop", 4) == 0) {
		comm_base_exit(global_svr->base);
		sslconn_shutdown(sc);
	} else {
		verbose(VERB_DETAIL, "unknown command: %s", str);
		handle_printclose(sc, "error unknown command");
	}
}

void svr_send_results(struct svr* svr)
{
	struct sslconn* s;
	for(s=svr->busy_list; s; s=s->next) {
		if(s->line_state == persist_write) {
			/* busy with last results, fetch update later */
			s->fetch_another_update=1;
		}
		if(s->line_state == persist_write_checkclose) {
			send_results_to_con(svr, s);
		}
	}
}

void svr_retry_callback(void* arg)
{
	struct svr* svr = (struct svr*)arg;
	if(!svr->retry_timer_enabled) {
		comm_timer_disable(svr->retry_timer);
		return;
	}
	verbose(VERB_ALGO, "retry timeout");
	cmd_reprobe();
}

static void svr_retry_setit(struct svr* svr)
{
	struct timeval tv;
	if(svr->retry_timer_count < RETRY_TIMER_COUNT_MAX)
		verbose(VERB_ALGO, "retry in %d seconds (try nr %d)", svr->retry_timer_timeout, svr->retry_timer_count);
	else	verbose(VERB_ALGO, "retry in %d seconds", svr->retry_timer_timeout);
	tv.tv_sec = svr->retry_timer_timeout;
	tv.tv_usec = 0;
	comm_timer_set(svr->retry_timer, &tv);
}

static void svr_retry_start(struct svr* svr, int http_mode)
{
	svr->retry_timer_timeout = RETRY_TIMER_START;
	if(http_mode)
		svr->retry_timer_count = 1;
	else	svr->retry_timer_count = RETRY_TIMER_COUNT_MAX;
	svr->retry_timer_enabled = 1;
	svr_retry_setit(svr);
}

void svr_retry_timer_next(int http_mode)
{
	struct svr* svr = global_svr;
	if(!svr->retry_timer_enabled) {
		svr_retry_start(svr, http_mode);
		return;
	}
	if(svr->retry_timer_count < RETRY_TIMER_COUNT_MAX) {
		svr->retry_timer_count++;
	} else {
		svr->retry_timer_timeout *= 2;
		if(svr->retry_timer_timeout > RETRY_TIMER_MAX)
			svr->retry_timer_timeout = RETRY_TIMER_MAX;
	}
	svr_retry_setit(svr);
}

void svr_retry_timer_stop(void)
{
	struct svr* svr = global_svr;
	if(!svr->retry_timer_enabled)
		return;
	svr->retry_timer_enabled = 0;
	comm_timer_disable(svr->retry_timer);
}

void svr_tcp_timer_stop(void)
{
	struct svr* svr = global_svr;
	comm_timer_disable(svr->retry_timer);
}

void svr_tcp_timer_enable(void)
{
	struct svr* svr = global_svr;
	struct timeval tv;
	if(svr->tcp_timer_used)
		return;
	verbose(VERB_ALGO, "retry dnstcp in %d seconds", SVR_TCP_RETRY);
	tv.tv_sec = SVR_TCP_RETRY;
	tv.tv_usec = 0;
	comm_timer_set(svr->tcp_timer, &tv);
}

void svr_tcp_callback(void* arg)
{
	/* we do this probe because some 20 seconds after login, more
	 * ports may open and this can alleviate traffic on the open
	 * recursors */
	struct svr* svr = (struct svr*)arg;
	verbose(VERB_ALGO, "retry dnstcp timeout");
	comm_timer_disable(svr->tcp_timer);
	if(svr->res_state == res_tcp || svr->res_state == res_ssl) {
		svr->tcp_timer_used = 1;
		cmd_reprobe();
	}
}

void svr_check_update(struct svr* svr)
{
	if(svr->update_desired && !svr->insecure_state && !svr->forced_insecure
		&& svr->res_state != res_dark && svr->res_state != res_disconn)
	{
		selfupdate_start(svr->update);
	}
}
