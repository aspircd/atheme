/*
 * SPDX-License-Identifier: ISC
 * SPDX-URL: https://spdx.org/licenses/ISC.html
 *
 * Copyright (C) 2006-2015 Atheme Project (http://atheme.org/)
 * Copyright (C) 2017 Atheme Development Group (https://atheme.github.io/)
 *
 * This file contains the main() routine.
 */

#include "atheme.h"

#ifndef MINIMUM
#  define MINIMUM(a, b) (((a) < (b)) ? (a) : (b))
#endif

static mowgli_list_t sessions;
static mowgli_list_t mechanisms;
static char mechlist_string[SASL_S2S_MAXLEN_ATONCE_B64];
static bool hide_server_names;

static struct service *saslsvs = NULL;
static mowgli_eventloop_timer_t *delete_stale_timer = NULL;

static const char *
sasl_format_sourceinfo(struct sourceinfo *const restrict si, const bool full)
{
	static char result[BUFSIZE];

	const struct sasl_sourceinfo *const ssi = (const struct sasl_sourceinfo *) si;

	if (full)
		(void) snprintf(result, sizeof result, "SASL/%s:%s[%s]:%s",
		                ssi->sess->uid ? ssi->sess->uid : "?",
		                ssi->sess->host ? ssi->sess->host : "?",
		                ssi->sess->ip ? ssi->sess->ip : "?",
		                ssi->sess->server ? ssi->sess->server->name : "?");
	else
		(void) snprintf(result, sizeof result, "SASL(%s)",
		                ssi->sess->host ? ssi->sess->host : "?");

	return result;
}

static const char *
sasl_get_source_name(struct sourceinfo *const restrict si)
{
	static char result[HOSTLEN + 1 + NICKLEN + 11];
	char description[BUFSIZE];

	const struct sasl_sourceinfo *const ssi = (const struct sasl_sourceinfo *) si;

	if (ssi->sess->server && ! hide_server_names)
		(void) snprintf(description, sizeof description, "Unknown user on %s (via SASL)",
		                                                 ssi->sess->server->name);
	else
		(void) mowgli_strlcpy(description, "Unknown user (via SASL)", sizeof description);

	// we can reasonably assume that si->v is non-null as this is part of the SASL vtable
	if (si->sourcedesc)
		(void) snprintf(result, sizeof result, "<%s:%s>%s", description, si->sourcedesc,
		                si->smu ? entity(si->smu)->name : "");
	else
		(void) snprintf(result, sizeof result, "<%s>%s", description, si->smu ? entity(si->smu)->name : "");

	return result;
}

static void
sasl_sourceinfo_recreate(struct sasl_session *const restrict p)
{
	static struct sourceinfo_vtable sasl_vtable = {

		.description        = "SASL",
		.format             = sasl_format_sourceinfo,
		.get_source_name    = sasl_get_source_name,
		.get_source_mask    = sasl_get_source_name,
	};

	if (p->si)
		(void) atheme_object_unref(p->si);

	struct sasl_sourceinfo *const ssi = smalloc(sizeof *ssi);

	(void) atheme_object_init(atheme_object(ssi), "<sasl sourceinfo>", &sfree);

	ssi->parent.s = p->server;
	ssi->parent.connection = curr_uplink->conn;

	if (p->host)
		ssi->parent.sourcedesc = p->host;

	ssi->parent.service = saslsvs;
	ssi->parent.v = &sasl_vtable;

	ssi->parent.force_language = language_find("en");
	ssi->sess = p;

	p->si = &ssi->parent;
}

static struct sasl_session *
find_session(const char *const restrict uid)
{
	if (! uid)
		return NULL;

	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, sessions.head)
	{
		struct sasl_session *const p = n->data;

		if (p->uid && strcmp(p->uid, uid) == 0)
			return p;
	}

	return NULL;
}

static struct sasl_session *
find_or_make_session(const char *const restrict uid, struct server *const restrict server)
{
	struct sasl_session *p;

	if (! (p = find_session(uid)))
	{
		p = smalloc(sizeof *p);

		p->server = server;

		(void) mowgli_strlcpy(p->uid, uid, sizeof p->uid);
		(void) mowgli_node_add(p, &p->node, &sessions);
	}

	return p;
}

static const struct sasl_mechanism *
find_mechanism(const char *const restrict name)
{
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, mechanisms.head)
	{
		const struct sasl_mechanism *const mptr = n->data;

		if (strcmp(mptr->name, name) == 0)
			return mptr;
	}

	(void) slog(LG_DEBUG, "%s: cannot find mechanism '%s'!", MOWGLI_FUNC_NAME, name);

	return NULL;
}

static void
sasl_server_eob(struct server ATHEME_VATTR_UNUSED *const restrict s)
{
	(void) sasl_mechlist_sts(mechlist_string);
}

static void
mechlist_build_string(void)
{
	char *buf = mechlist_string;
	size_t tmplen = 0;
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, mechanisms.head)
	{
		const struct sasl_mechanism *const mptr = n->data;
		const size_t namelen = strlen(mptr->name);

		if (tmplen + namelen >= sizeof mechlist_string)
			break;

		(void) memcpy(buf, mptr->name, namelen);

		buf += namelen;
		*buf++ = ',';
		tmplen += namelen + 1;
	}

	if (tmplen)
		buf--;

	*buf = 0x00;
}

static void
mechlist_do_rebuild(void)
{
	(void) mechlist_build_string();

	if (me.connected)
		(void) sasl_mechlist_sts(mechlist_string);
}

static bool
may_impersonate(struct myuser *const source_mu, struct myuser *const target_mu)
{
	// Allow same (although this function won't get called in that case anyway)
	if (source_mu == target_mu)
		return true;

	char priv[BUFSIZE] = PRIV_IMPERSONATE_ANY;

	// Check for wildcard priv
	if (has_priv_myuser(source_mu, priv))
		return true;

	// Check for target-operclass specific priv
	const char *const classname = (target_mu->soper && target_mu->soper->classname) ?
	                                  target_mu->soper->classname : "user";
	(void) snprintf(priv, sizeof priv, PRIV_IMPERSONATE_CLASS_FMT, classname);
	if (has_priv_myuser(source_mu, priv))
		return true;

	// Check for target-entity specific priv
	(void) snprintf(priv, sizeof priv, PRIV_IMPERSONATE_ENTITY_FMT, entity(target_mu)->name);
	if (has_priv_myuser(source_mu, priv))
		return true;

	// Allow modules to check too
	hook_sasl_may_impersonate_t req = {

		.source_mu = source_mu,
		.target_mu = target_mu,
		.allowed = false,
	};

	(void) hook_call_sasl_may_impersonate(&req);

	return req.allowed;
}

static struct myuser *
login_user(struct sasl_session *const restrict p)
{
	// source_mu is the user whose credentials we verified ("authentication id" / authcid)
	// target_mu is the user who will be ultimately logged in ("authorization id" / authzid)
	struct myuser *source_mu;
	struct myuser *target_mu;

	if (! *p->authceid || ! (source_mu = myuser_find_uid(p->authceid)))
		return NULL;

	if (! *p->authzeid)
	{
		target_mu = source_mu;

		(void) mowgli_strlcpy(p->authzid, p->authcid, sizeof p->authzid);
		(void) mowgli_strlcpy(p->authzeid, p->authceid, sizeof p->authzeid);
	}
	else if (! (target_mu = myuser_find_uid(p->authzeid)))
		return NULL;

	if (metadata_find(source_mu, "private:freeze:freezer"))
	{
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (frozen)", entity(source_mu)->name);
		return NULL;
	}

	if (target_mu != source_mu)
	{
		if (! may_impersonate(source_mu, target_mu))
		{
			(void) logcommand(p->si, CMDLOG_LOGIN, "denied IMPERSONATE by \2%s\2 to \2%s\2",
			                                       entity(source_mu)->name, entity(target_mu)->name);
			return NULL;
		}

		if (metadata_find(target_mu, "private:freeze:freezer"))
		{
			(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (frozen)",
			                                       entity(target_mu)->name);
			return NULL;
		}
	}

	if (MOWGLI_LIST_LENGTH(&target_mu->logins) >= me.maxlogins)
	{
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (too many logins)",
		                                       entity(target_mu)->name);
		return NULL;
	}

	// Log it with the full n!u@h later
	p->flags |= ASASL_NEED_LOG;

	/* We just did SASL authentication for a user.  With IRCds which do not
	 * have unique UIDs for users, we will likely be expecting the login
	 * data to be bursted.  As a result, we should give the core a heads'
	 * up that this is going to happen so that hooks will be properly
	 * fired...
	 */
	if (ircd->flags & IRCD_SASL_USE_PUID)
	{
		target_mu->flags &= ~MU_NOBURSTLOGIN;
		target_mu->flags |= MU_PENDINGLOGIN;
	}

	if (target_mu != source_mu)
		(void) logcommand(p->si, CMDLOG_LOGIN, "allowed IMPERSONATE by \2%s\2 to \2%s\2",
		                                       entity(source_mu)->name, entity(target_mu)->name);

	return target_mu;
}

/* given an entire sasl message, advance session by passing data to mechanism
 * and feeding returned data back to client.
 */
static bool ATHEME_FATTR_WUR
sasl_packet(struct sasl_session *const restrict p, char *const restrict buf, const size_t len)
{
	unsigned int rc;

	struct sasl_output_buf outbuf = {
		.buf    = NULL,
		.len    = 0,
		.flags  = ASASL_OUTFLAG_NONE,
	};

	bool have_written = false;

	// First piece of data in a session is the name of the SASL mechanism that will be used
	if (! p->mechptr && ! len)
	{
		(void) sasl_sourceinfo_recreate(p);

		if (! (p->mechptr = find_mechanism(buf)))
		{
			(void) sasl_sts(p->uid, 'M', mechlist_string);
			return false;
		}

		if (p->mechptr->mech_start)
			rc = p->mechptr->mech_start(p, &outbuf);
		else
			rc = ASASL_MORE;
	}
	else if (! p->mechptr)
	{
		(void) slog(LG_ERROR, "%s: session has no mechanism (BUG!)", MOWGLI_FUNC_NAME);
		return false;
	}
	else
	{
		if (*buf == '+' && len == 1)
		{
			rc = p->mechptr->mech_step(p, NULL, &outbuf);
		}
		else
		{
			unsigned char decbuf[SASL_S2S_MAXLEN_TOTAL_RAW + 1];
			const size_t declen = base64_decode(buf, decbuf, SASL_S2S_MAXLEN_TOTAL_RAW);

			if (declen != (size_t) -1)
			{
				// Ensure input is NULL-terminated for modules that want to process the data as a string
				decbuf[declen] = 0x00;

				unsigned int inflags = ASASL_INFLAG_NONE;
				const struct sasl_input_buf inbuf = {
					.buf    = decbuf,
					.len    = declen,
					.flags  = &inflags,
				};

				rc = p->mechptr->mech_step(p, &inbuf, &outbuf);

				// The mechanism instructed us to wipe the input data now that it has been processed
				if (inflags & ASASL_INFLAG_WIPE_BUF)
				{
					/* If we got here, the bufferred base64-encoded input data is either in a
					 * dedicated buffer (buf == p->buf && len == p->len) or directly from a
					 * parv[] inside struct sasl_message. Either way buf is mutable.    -- amdj
					 */
					(void) smemzero(buf, len);          // Erase the base64-encoded input data
					(void) smemzero(decbuf, declen);    // Erase the base64-decoded input data
				}
			}
			else
			{
				(void) slog(LG_DEBUG, "%s: base64_decode() failed", MOWGLI_FUNC_NAME);

				rc = ASASL_ERROR;
			}
		}
	}

	// Some progress has been made, reset timeout.
	p->flags &= ~ASASL_MARKED_FOR_DELETION;

	if (outbuf.buf && outbuf.len)
	{
		char encbuf[SASL_S2S_MAXLEN_TOTAL_B64 + 1];
		const size_t enclen = base64_encode(outbuf.buf, outbuf.len, encbuf, sizeof encbuf);

		// The mechanism instructed us to wipe the output data now that it has been encoded
		if (outbuf.flags & ASASL_OUTFLAG_WIPE_BUF)
			(void) smemzero(outbuf.buf, outbuf.len);

		// The mechanism did not indicate to us that the buffer is not dynamic -- free it now
		if (! (outbuf.flags & ASASL_OUTFLAG_DONT_FREE_BUF))
			(void) sfree(outbuf.buf);

		outbuf.buf = NULL;
		outbuf.len = 0;

		if (enclen == (size_t) -1)
		{
			(void) slog(LG_ERROR, "%s: base64_encode() failed", MOWGLI_FUNC_NAME);
			return false;
		}

		const char *encbufptr = encbuf;
		size_t encbuflast = SASL_S2S_MAXLEN_ATONCE_B64;

		for (size_t encbufrem = enclen; encbufrem != 0; /* No action */)
		{
			char encbufpart[SASL_S2S_MAXLEN_ATONCE_B64 + 1];
			const size_t encbufptrlen = MINIMUM(SASL_S2S_MAXLEN_ATONCE_B64, encbufrem);

			(void) memset(encbufpart, 0x00, sizeof encbufpart);
			(void) memcpy(encbufpart, encbufptr, encbufptrlen);
			(void) sasl_sts(p->uid, 'C', encbufpart);

			// The mechanism instructed us to wipe the output data now that it has been transmitted
			if (outbuf.flags & ASASL_OUTFLAG_WIPE_BUF)
				(void) smemzero(encbufpart, encbufptrlen);

			encbufptr += encbufptrlen;
			encbufrem -= encbufptrlen;
			encbuflast = encbufptrlen;
		}

		/* The end of a packet is indicated by a string not of the maximum length. If the last string
		 * was the maximum length, send another, empty string, to advance the session.    -- amdj
		 */
		if (encbuflast == SASL_S2S_MAXLEN_ATONCE_B64)
			(void) sasl_sts(p->uid, 'C', "+");

		// The mechanism instructed us to wipe the output data now that it has been transmitted
		if (outbuf.flags & ASASL_OUTFLAG_WIPE_BUF)
			(void) smemzero(encbuf, enclen);

		have_written = true;
	}

	if (rc == ASASL_MORE)
	{
		if (! have_written)
			/* We want more data from the client, but we haven't sent any of our own.
			 * Send an empty string to advance the session.    -- amdj
			 */
			(void) sasl_sts(p->uid, 'C', "+");

		return true;
	}

	if (rc == ASASL_DONE)
	{
		struct myuser *const mu = login_user(p);

		if (! mu)
			return false;

		char *cloak = "*";
		struct metadata *md;

		if ((md = metadata_find(mu, "private:usercloak")))
			cloak = md->value;

		if (! (mu->flags & MU_WAITAUTH))
			(void) svslogin_sts(p->uid, "*", "*", cloak, mu);

		(void) sasl_sts(p->uid, 'D', "S");

		// Will destroy session on introduction of user to net.
		return true;
	}

	if (rc == ASASL_FAIL && *p->authceid)
	{
		/* If we reach this, they failed SASL auth, so if they were trying
		 * to identify as a specific user, bad_password them.
		 */
		struct myuser *const mu = myuser_find_uid(p->authceid);

		if (! mu)
			return false;

		/* We might have more information to construct a more accurate sourceinfo now?
		 * TODO: Investigate whether this is necessary
		 */
		(void) sasl_sourceinfo_recreate(p);

		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN (%s) to \2%s\2 (bad password)",
		                                       p->mechptr->name, entity(mu)->name);
		(void) bad_password(p->si, mu);

		return false;
	}

	return false;
}

static bool ATHEME_FATTR_WUR
sasl_buf_process(struct sasl_session *const restrict p)
{
	// Ensure the buffer is NULL-terminated so that base64_decode() doesn't overrun it
	p->buf[p->len] = 0x00;

	if (! sasl_packet(p, p->buf, p->len))
		return false;

	(void) sfree(p->buf);

	p->buf = NULL;
	p->len = 0;

	return true;
}

static void
sasl_input_hostinfo(const struct sasl_message *const restrict smsg, struct sasl_session *const restrict p)
{
	p->host = sstrdup(smsg->parv[0]);
	p->ip   = sstrdup(smsg->parv[1]);

	if (smsg->parc >= 3 && strcmp(smsg->parv[2], "P") != 0)
		p->tls = true;
}

static bool ATHEME_FATTR_WUR
sasl_input_startauth(const struct sasl_message *const restrict smsg, struct sasl_session *const restrict p)
{
	if (strcmp(smsg->parv[0], "EXTERNAL") == 0)
	{
		if (smsg->parc < 2)
		{
			(void) slog(LG_DEBUG, "%s: client %s starting EXTERNAL authentication without a "
			                      "fingerprint", MOWGLI_FUNC_NAME, smsg->uid);
			return false;
		}

		(void) sfree(p->certfp);

		p->certfp = sstrdup(smsg->parv[1]);
		p->tls = true;
	}

	return sasl_packet(p, smsg->parv[0], 0);
}

static bool ATHEME_FATTR_WUR
sasl_input_clientdata(const struct sasl_message *const restrict smsg, struct sasl_session *const restrict p)
{
	/* This is complicated.
	 *
	 * Clients are restricted to sending us 300 bytes (400 Base-64 characters), but the mechanism
	 * that they have chosen could require them to send more than this amount, so they have to send
	 * it 400 Base-64 characters at a time in stages. When we receive data less than 400 characters,
	 * we know we don't need to buffer any more data, and can finally process it.
	 *
	 * However, if the client wants to send us a multiple of 400 characters and no more, we would be
	 * waiting forever for them to send 'the rest', even though there isn't any. This is solved by
	 * having them send a single '+' character to indicate that they have no more data to send.
	 *
	 * This is also what clients send us when they do not want to send us any data at all, and in
	 * either event, this is *NOT* *DATA* we are receiving, and we should not buffer it.
	 *
	 * Also, if the data is a single '*' character, the client is aborting authentication. Servers
	 * should send us a 'D' packet instead of a 'C *' packet in this case, but this is for if they
	 * don't. Note that this will usually result in the client getting a 904 numeric instead of 906,
	 * but the alternative is not treating '*' specially and then going on to fail to decode it in
	 * sasl_packet() above, which will result in ... an aborted session and a 904 numeric. So this
	 * just saves time.
	 */

	const size_t len = strlen(smsg->parv[0]);

	// Abort?
	if (len == 1 && smsg->parv[0][0] == '*')
		return false;

	// End of data?
	if (len == 1 && smsg->parv[0][0] == '+')
	{
		if (p->buf)
			return sasl_buf_process(p);

		// This function already deals with the special case of 1 '+' character
		return sasl_packet(p, smsg->parv[0], len);
	}

	/* Optimisation: If there is no buffer yet and this data is less than 400 characters, we don't
	 * need to buffer it at all, and can process it immediately.
	 */
	if (! p->buf && len < SASL_S2S_MAXLEN_ATONCE_B64)
		return sasl_packet(p, smsg->parv[0], len);

	/* We need to buffer the data now, but first check if the client hasn't sent us an excessive
	 * amount already.
	 */
	if ((p->len + len) > SASL_S2S_MAXLEN_TOTAL_B64)
	{
		(void) slog(LG_DEBUG, "%s: client %s has exceeded allowed data length", MOWGLI_FUNC_NAME, smsg->uid);
		return false;
	}

	// (Re)allocate a buffer, append the received data to it, and update its recorded length.
	p->buf = srealloc(p->buf, p->len + len + 1);
	(void) memcpy(p->buf + p->len, smsg->parv[0], len);
	p->len += len;

	// Messages not exactly 400 characters are the end of data.
	if (len < SASL_S2S_MAXLEN_ATONCE_B64)
		return sasl_buf_process(p);

	return true;
}

static void
destroy_session(struct sasl_session *const restrict p)
{
	if (p->flags & ASASL_NEED_LOG && *p->authceid)
	{
		struct myuser *const mu = myuser_find_uid(p->authceid);

		if (mu && ! (ircd->flags & IRCD_SASL_USE_PUID))
			(void) logcommand(p->si, CMDLOG_LOGIN, "LOGIN (session timed out)");
	}

	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, sessions.head)
		if (n == &p->node && n->data == p)
			(void) mowgli_node_delete(n, &sessions);

	if (p->mechptr && p->mechptr->mech_finish)
		(void) p->mechptr->mech_finish(p);

	if (p->si)
		(void) atheme_object_unref(p->si);

	(void) sfree(p->certfp);
	(void) sfree(p->host);
	(void) sfree(p->buf);
	(void) sfree(p->ip);
	(void) sfree(p);
}

static inline void
sasl_session_abort(struct sasl_session *const restrict p)
{
	(void) sasl_sts(p->uid, 'D', "F");
	(void) destroy_session(p);
}

static void
sasl_input(struct sasl_message *const restrict smsg)
{
	struct sasl_session *const p = find_or_make_session(smsg->uid, smsg->server);

	switch(smsg->mode)
	{
	case 'H':
		// (H)ost information
		(void) sasl_input_hostinfo(smsg, p);
		return;

	case 'S':
		// (S)tart authentication
		if (! sasl_input_startauth(smsg, p))
			(void) sasl_session_abort(p);

		return;

	case 'C':
		// (C)lient data
		if (! sasl_input_clientdata(smsg, p))
			(void) sasl_session_abort(p);

		return;

	case 'D':
		// (D)one -- when we receive it, means client abort
		(void) destroy_session(p);
		return;
	}
}

static void
sasl_newuser(hook_user_nick_t *const restrict data)
{
	// If the user has been killed, don't do anything.
	struct user *const u = data->u;
	if (! u)
		return;

	// Not concerned unless it's a SASL login.
	struct sasl_session *const p = find_session(u->uid);
	if (! p)
		return;

	// We will log it ourselves, if needed
	p->flags &= ~ASASL_NEED_LOG;

	// Find the account
	struct myuser *const mu = *p->authzeid ? myuser_find_uid(p->authzeid) : NULL;
	if (! mu)
	{
		(void) notice(saslsvs->nick, u->nick, "Account %s dropped, login cancelled",
		                                      *p->authzid ? p->authzid : "???");
		(void) destroy_session(p);

		// We'll remove their ircd login in handle_burstlogin()
		return;
	}

	const struct sasl_mechanism *const mptr = p->mechptr;

	(void) destroy_session(p);
	(void) myuser_login(saslsvs, u, mu, false);
	(void) logcommand_user(saslsvs, u, CMDLOG_LOGIN, "LOGIN (%s)", mptr->name);
}

static void
delete_stale(void ATHEME_VATTR_UNUSED *const restrict vptr)
{
	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, sessions.head)
	{
		struct sasl_session *const p = n->data;

		if (p->flags & ASASL_MARKED_FOR_DELETION)
			(void) destroy_session(p);
		else
			p->flags |= ASASL_MARKED_FOR_DELETION;
	}
}

static void
sasl_mech_register(const struct sasl_mechanism *const restrict mech)
{
	(void) slog(LG_DEBUG, "%s: registering %s", MOWGLI_FUNC_NAME, mech->name);

	mowgli_node_t *const node = mowgli_node_create();

	if (! node)
	{
		(void) slog(LG_ERROR, "%s: mowgli_node_create() failed; out of memory?", MOWGLI_FUNC_NAME);
		return;
	}

	/* Here we cast it to (void *) because mowgli_node_add() expects that; it cannot be made const because then
	 * it would have to return a (const void *) too which would cause multiple warnings any time it is actually
	 * storing, and thus gets assigned to, a pointer to a mutable object.
	 *
	 * To avoid the cast generating a diagnostic due to dropping a const qualifier, we first cast to uintptr_t.
	 * This is not unprecedented in this codebase; libathemecore/crypto.c & libathemecore/strshare.c do the
	 * same thing.
	 */
	(void) mowgli_node_add((void *)((uintptr_t) mech), node, &mechanisms);

	(void) mechlist_do_rebuild();
}

static void
sasl_mech_unregister(const struct sasl_mechanism *const restrict mech)
{
	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, sessions.head)
	{
		struct sasl_session *const session = n->data;

		if (session->mechptr == mech)
		{
			(void) slog(LG_DEBUG, "%s: destroying session %s", MOWGLI_FUNC_NAME, session->uid);
			(void) destroy_session(session);
		}
	}
	MOWGLI_ITER_FOREACH_SAFE(n, tn, mechanisms.head)
	{
		if (n->data == mech)
		{
			(void) slog(LG_DEBUG, "%s: unregistering %s", MOWGLI_FUNC_NAME, mech->name);
			(void) mowgli_node_delete(n, &mechanisms);
			(void) mowgli_node_free(n);
			(void) mechlist_do_rebuild();

			break;
		}
	}
}

static inline bool ATHEME_FATTR_WUR
sasl_authxid_can_login(struct sasl_session *const restrict p, const char *const restrict authxid,
                       struct myuser **const restrict muo, char *const restrict val_name,
                       char *const restrict val_eid, const char *const restrict other_val_eid)
{
	struct myuser *const mu = myuser_find_by_nick(authxid);

	if (! mu)
	{
		(void) slog(LG_DEBUG, "%s: myuser_find_by_nick: does not exist", MOWGLI_FUNC_NAME);
		return false;
	}

	if (muo)
		*muo = mu;

	(void) mowgli_strlcpy(val_name, entity(mu)->name, NICKLEN + 1);
	(void) mowgli_strlcpy(val_eid, entity(mu)->id, IDLEN + 1);

	if (strcmp(val_eid, other_val_eid) == 0)
		// We have already executed the user_can_login hook for this user
		return true;

	hook_user_login_check_t req = {

		.si         = p->si,
		.mu         = mu,
		.allowed    = true,
	};

	(void) hook_call_user_can_login(&req);

	if (! req.allowed)
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (denied by hook)", entity(mu)->name);

	return req.allowed;
}

static bool ATHEME_FATTR_WUR
sasl_authcid_can_login(struct sasl_session *const restrict p, const char *const restrict authcid,
                       struct myuser **const restrict muo)
{
	return sasl_authxid_can_login(p, authcid, muo, p->authcid, p->authceid, p->authzeid);
}

static bool ATHEME_FATTR_WUR
sasl_authzid_can_login(struct sasl_session *const restrict p, const char *const restrict authzid,
                       struct myuser **const restrict muo)
{
	return sasl_authxid_can_login(p, authzid, muo, p->authzid, p->authzeid, p->authceid);
}

const struct sasl_core_functions sasl_core_functions = {

	.mech_register      = &sasl_mech_register,
	.mech_unregister    = &sasl_mech_unregister,
	.authcid_can_login  = &sasl_authcid_can_login,
	.authzid_can_login  = &sasl_authzid_can_login,
};

static void
saslserv(struct sourceinfo *const restrict si, const int parc, char **const restrict parv)
{
	// this should never happen
	if (parv[0][0] == '&')
	{
		(void) slog(LG_ERROR, "%s: got parv with local channel: %s", MOWGLI_FUNC_NAME, parv[0]);
		return;
	}

	// make a copy of the original for debugging
	char orig[BUFSIZE];
	(void) mowgli_strlcpy(orig, parv[parc - 1], sizeof orig);

	// lets go through this to get the command
	char *const cmd = strtok(parv[parc - 1], " ");
	char *const text = strtok(NULL, "");

	if (! cmd)
		return;

	if (*orig == '\001')
	{
		(void) handle_ctcp_common(si, cmd, text);
		return;
	}

	(void) command_fail(si, fault_noprivs, "This service exists to identify connecting clients to the network. "
	                                       "It has no public interface.");
}

static void
mod_init(struct module *const restrict m)
{
	if (! (saslsvs = service_add("saslserv", &saslserv)))
	{
		(void) slog(LG_ERROR, "%s: service_add() failed", m->name);
		m->mflags |= MODFLAG_FAIL;
		return;
	}

	(void) hook_add_event("sasl_input");
	(void) hook_add_sasl_input(&sasl_input);
	(void) hook_add_event("user_add");
	(void) hook_add_user_add(&sasl_newuser);
	(void) hook_add_event("server_eob");
	(void) hook_add_server_eob(&sasl_server_eob);
	(void) hook_add_event("sasl_may_impersonate");
	(void) hook_add_event("user_can_login");

	delete_stale_timer = mowgli_timer_add(base_eventloop, "sasl_delete_stale", &delete_stale, NULL, 30);
	authservice_loaded++;

	(void) add_bool_conf_item("HIDE_SERVER_NAMES", &saslsvs->conf_table, 0, &hide_server_names, false);
}

static void
mod_deinit(const enum module_unload_intent ATHEME_VATTR_UNUSED intent)
{
	(void) hook_del_sasl_input(&sasl_input);
	(void) hook_del_user_add(&sasl_newuser);
	(void) hook_del_server_eob(&sasl_server_eob);

	(void) mowgli_timer_destroy(base_eventloop, delete_stale_timer);

	(void) del_conf_item("HIDE_SERVER_NAMES", &saslsvs->conf_table);
	(void) service_delete(saslsvs);

	authservice_loaded--;

	if (sessions.head)
		(void) slog(LG_ERROR, "saslserv/main: shutting down with a non-empty session list; "
		                      "a mechanism did not unregister itself! (BUG)");
}

SIMPLE_DECLARE_MODULE_V1("saslserv/main", MODULE_UNLOAD_CAPABILITY_OK)
