/*
 * Copyright (c) 2009 Atheme Development Group
 * Copyright (c) 2009 Rizon Development Team <http://redmine.rizon.net>
 * Rights to this code are documented in doc/LICENSE.
 *
 * This file contains functionality which implements
 * the OperServ SGLINE command.
 */

#include "atheme.h"

static void os_sgline_newuser(hook_user_nick_t *data);

static void os_cmd_sgline(sourceinfo_t *si, int parc, char *parv[]);
static void os_cmd_sgline_add(sourceinfo_t *si, int parc, char *parv[]);
static void os_cmd_sgline_del(sourceinfo_t *si, int parc, char *parv[]);
static void os_cmd_sgline_list(sourceinfo_t *si, int parc, char *parv[]);
static void os_cmd_sgline_sync(sourceinfo_t *si, int parc, char *parv[]);

command_t os_sgline = { "SGLINE", N_("Manages network realname bans."), PRIV_MASS_AKILL, 3, os_cmd_sgline, { .path = "oservice/sgline" } };

command_t os_sgline_add = { "ADD", N_("Adds a network realname ban"), AC_NONE, 2, os_cmd_sgline_add, { .path = "" } };
command_t os_sgline_del = { "DEL", N_("Deletes a network realname ban"), AC_NONE, 1, os_cmd_sgline_del, { .path = "" } };
command_t os_sgline_list = { "LIST", N_("Lists all network realname bans"), AC_NONE, 1, os_cmd_sgline_list, { .path = "" } };
command_t os_sgline_sync = { "SYNC", N_("Synchronises network realname bans to servers"), AC_NONE, 0, os_cmd_sgline_sync, { .path = "" } };

mowgli_patricia_t *os_sgline_cmds;

static void
mod_init(module_t *const restrict m)
{
	if (ircd != NULL && xline_sts == generic_xline_sts)
	{
		slog(LG_INFO, "Module %s requires xline support, refusing to load.",
				m->name);
		m->mflags = MODTYPE_FAIL;
		return;
	}

	service_named_bind_command("operserv", &os_sgline);

	os_sgline_cmds = mowgli_patricia_create(strcasecanon);

	/* Add sub-commands */
	command_add(&os_sgline_add, os_sgline_cmds);
	command_add(&os_sgline_del, os_sgline_cmds);
	command_add(&os_sgline_list, os_sgline_cmds);
	command_add(&os_sgline_sync, os_sgline_cmds);

	hook_add_event("user_add");
	hook_add_user_add(os_sgline_newuser);
}

static void
mod_deinit(const module_unload_intent_t intent)
{
	service_named_unbind_command("operserv", &os_sgline);

	/* Delete sub-commands */
	command_delete(&os_sgline_add, os_sgline_cmds);
	command_delete(&os_sgline_del, os_sgline_cmds);
	command_delete(&os_sgline_list, os_sgline_cmds);
	command_delete(&os_sgline_sync, os_sgline_cmds);

	hook_del_user_add(os_sgline_newuser);

	mowgli_patricia_destroy(os_sgline_cmds, NULL, NULL);
}

static void os_sgline_newuser(hook_user_nick_t *data)
{
	user_t *u = data->u;
	struct xline *x;

	/* If the user has been killed, don't do anything. */
	if (!u)
		return;

	if (is_internal_client(u))
		return;
	x = xline_find_user(u);
	if (x != NULL)
	{
		/* Server didn't have that xline, send it again.
		 * To ensure xline exempt works on sglines too, do
		 * not send a KILL. -- jilles */
		xline_sts("*", x->realname, x->duration ? x->expires - CURRTIME : 0, x->reason);
	}
}

static void os_cmd_sgline(sourceinfo_t *si, int parc, char *parv[])
{
	/* Grab args */
	char *cmd = parv[0];
	command_t *c;

	/* Bad/missing arg */
	if (!cmd)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SGLINE");
		command_fail(si, fault_needmoreparams, _("Syntax: SGLINE ADD|DEL|LIST"));
		return;
	}

	c = command_find(os_sgline_cmds, cmd);
	if(c == NULL)
	{
		command_fail(si, fault_badparams, _("Invalid command. Use \2/%s%s help\2 for a command listing."), (ircd->uses_rcommand == false) ? "msg " : "", si->service->disp);
		return;
	}

	command_exec(si->service, si, c, parc - 1, parv + 1);
}

static void os_cmd_sgline_add(sourceinfo_t *si, int parc, char *parv[])
{
	char *target = parv[0];
	char *token = strtok(parv[1], " ");
	char *treason, reason[BUFSIZE];
	long duration;
	char *s;
	struct xline *x;

	if (!target || !token)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SGLINE ADD");
		command_fail(si, fault_needmoreparams, _("Syntax: SGLINE ADD <gecos> [!P|!T <minutes>] <reason>"));
		return;
	}

	if (!strcasecmp(token, "!P"))
	{
		duration = 0;
		treason = strtok(NULL, "");

		if (treason)
			mowgli_strlcpy(reason, treason, BUFSIZE);
		else
			mowgli_strlcpy(reason, "No reason given", BUFSIZE);
	}
	else if (!strcasecmp(token, "!T"))
	{
		s = strtok(NULL, " ");
		treason = strtok(NULL, "");
		if (treason)
			mowgli_strlcpy(reason, treason, BUFSIZE);
		else
			mowgli_strlcpy(reason, "No reason given", BUFSIZE);
		if (s)
		{
			duration = (atol(s) * 60);
			while (isdigit((unsigned char)*s))
				s++;
			if (*s == 'h' || *s == 'H')
				duration *= 60;
			else if (*s == 'd' || *s == 'D')
				duration *= 1440;
			else if (*s == 'w' || *s == 'W')
				duration *= 10080;
			else if (*s == '\0')
				;
			else
				duration = 0;
			if (duration == 0)
			{
				command_fail(si, fault_badparams, _("Invalid duration given."));
				command_fail(si, fault_badparams, _("Syntax: SGLINE ADD <gecos> [!P|!T <minutes>] <reason>"));
				return;
			}
		}
		else {
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SGLINE ADD");
			command_fail(si, fault_needmoreparams, _("Syntax: SGLINE ADD <gecos> [!P|!T <minutes>] <reason>"));
			return;
		}

	}
	else
	{
		duration = config_options.kline_time;
		mowgli_strlcpy(reason, token, BUFSIZE);
		treason = strtok(NULL, "");

		if (treason)
		{
			mowgli_strlcat(reason, " ", BUFSIZE);
			mowgli_strlcat(reason, treason, BUFSIZE);
		}
	}

	char *p;
	int i = 0;

	/* make sure there's at least 3 non-wildcards */
	/* except if there are no wildcards at all */
	for (p = target; *p; p++)
	{
		if (*p != '*' && *p != '?')
			i++;
	}

	if (i < 3 && (strchr(target, '*') || strchr(target, '?')) && !has_priv(si, PRIV_AKILL_ANYMASK))
	{
		command_fail(si, fault_badparams, _("Invalid gecos: \2%s\2. At least three non-wildcard characters are required."), target);
		return;
	}

	if (strlen(target) > (GECOSLEN + 1) * 2)
	{
		command_fail(si, fault_badparams, _("The mask provided is too long."));
		return;
	}

	if (xline_find(target))
	{
		command_fail(si, fault_nochange, _("SGLINE \2%s\2 is already matched in the database."), target);
		return;
	}

	x = xline_add(target, reason, duration, get_storage_oper_name(si));

	if (duration)
		command_success_nodata(si, _("Timed SGLINE on \2%s\2 was successfully added and will expire in %s."), x->realname, timediff(duration));
	else
		command_success_nodata(si, _("SGLINE on \2%s\2 was successfully added."), x->realname);

	verbose_wallops("\2%s\2 is \2adding\2 an \2SGLINE\2 for \2%s\2 -- reason: \2%s\2", get_oper_name(si), x->realname,
		x->reason);
	logcommand(si, CMDLOG_ADMIN, "SGLINE:ADD: \2%s\2 (reason: \2%s\2)", x->realname, x->reason);
}

static void os_cmd_sgline_del(sourceinfo_t *si, int parc, char *parv[])
{
	char *target = parv[0];
	struct xline *x;
	unsigned int number;
	char *s;

	if (!target)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SGLINE DEL");
		command_fail(si, fault_needmoreparams, _("Syntax: SGLINE DEL <gecos>"));
		return;
	}

	if (strchr(target, ','))
	{
		unsigned int start = 0, end = 0, i;
		char t[16];

		s = strtok(target, ",");

		do
		{
			if (strchr(s, ':'))
			{
				for (i = 0; *s != ':'; s++, i++)
					t[i] = *s;

				t[++i] = '\0';
				start = atoi(t);

				s++;	/* skip past the : */

				for (i = 0; *s != '\0'; s++, i++)
					t[i] = *s;

				t[++i] = '\0';
				end = atoi(t);

				for (i = start; i <= end; i++)
				{
					if (!(x = xline_find_num(i)))
					{
						command_fail(si, fault_nosuch_target, _("No such SGLINE with number \2%d\2."), i);
						continue;
					}

					command_success_nodata(si, _("SGLINE on \2%s\2 has been successfully removed."), x->realname);
					verbose_wallops("\2%s\2 is \2removing\2 an \2SGLINE\2 for \2%s\2 -- reason: \2%s\2",
						get_oper_name(si), x->realname, x->reason);

					logcommand(si, CMDLOG_ADMIN, "SGLINE:DEL: \2%s\2", x->realname);
					xline_delete(x->realname);
				}

				continue;
			}

			number = atoi(s);

			if (!(x = xline_find_num(number)))
			{
				command_fail(si, fault_nosuch_target, _("No such SGLINE with number \2%d\2."), number);
				return;
			}

			command_success_nodata(si, _("SGLINE on \2%s\2 has been successfully removed."), x->realname);
			verbose_wallops("\2%s\2 is \2removing\2 an \2SGLINE\2 for \2%s\2 -- reason: \2%s\2",
				get_oper_name(si), x->realname, x->reason);

			logcommand(si, CMDLOG_ADMIN, "SGLINE:DEL: \2%s\2", x->realname);
			xline_delete(x->realname);
		} while ((s = strtok(NULL, ",")));

		return;
	}

	if (!(x = xline_find(target)))
	{
		command_fail(si, fault_nosuch_target, _("No such SGLINE: \2%s\2."), target);
		return;
	}

	command_success_nodata(si, _("SGLINE on \2%s\2 has been successfully removed."), target);

	verbose_wallops("\2%s\2 is \2removing\2 an \2SGLINE\2 for \2%s\2 -- reason: \2%s\2",
		get_oper_name(si), x->realname, x->reason);

	logcommand(si, CMDLOG_ADMIN, "SGLINE:DEL: \2%s\2", target);
	xline_delete(target);
}

static void os_cmd_sgline_list(sourceinfo_t *si, int parc, char *parv[])
{
	char *param = parv[0];
	bool full = false;
	mowgli_node_t *n;
	struct xline *x;

	if (param != NULL && !strcasecmp(param, "FULL"))
		full = true;

	if (full)
		command_success_nodata(si, _("SGLINE list (with reasons):"));
	else
		command_success_nodata(si, _("SGLINE list:"));

	MOWGLI_ITER_FOREACH(n, xlnlist.head)
	{
		x = (struct xline *)n->data;

		if (x->duration && full)
			command_success_nodata(si, _("%d: %s - by \2%s\2 - expires in \2%s\2 - (%s)"), x->number, x->realname, x->setby, timediff(x->expires > CURRTIME ? x->expires - CURRTIME : 0), x->reason);
		else if (x->duration && !full)
			command_success_nodata(si, _("%d: %s - by \2%s\2 - expires in \2%s\2"), x->number, x->realname, x->setby, timediff(x->expires > CURRTIME ? x->expires - CURRTIME : 0));
		else if (!x->duration && full)
			command_success_nodata(si, _("%d: %s - by \2%s\2 - \2permanent\2 - (%s)"), x->number, x->realname, x->setby, x->reason);
		else
			command_success_nodata(si, _("%d: %s - by \2%s\2 - \2permanent\2"), x->number, x->realname, x->setby);
	}

	command_success_nodata(si, _("Total of \2%zu\2 %s in SGLINE list."), xlnlist.count, (xlnlist.count == 1) ? "entry" : "entries");
	logcommand(si, CMDLOG_GET, "SGLINE:LIST: \2%s\2", full ? " FULL" : "");
}

static void os_cmd_sgline_sync(sourceinfo_t *si, int parc, char *parv[])
{
	mowgli_node_t *n;
	struct xline *x;

	logcommand(si, CMDLOG_DO, "SGLINE:SYNC");

	MOWGLI_ITER_FOREACH(n, xlnlist.head)
	{
		x = (struct xline *)n->data;

		if (x->duration == 0)
			xline_sts("*", x->realname, 0, x->reason);
		else if (x->expires > CURRTIME)
			xline_sts("*", x->realname, x->expires - CURRTIME, x->reason);
	}

	command_success_nodata(si, _("SGLINE list synchronized to servers."));
}

SIMPLE_DECLARE_MODULE_V1("operserv/sgline", MODULE_UNLOAD_CAPABILITY_OK)
