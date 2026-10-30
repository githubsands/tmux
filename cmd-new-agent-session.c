/* $OpenBSD$ */

/*
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Create a new agent session with a head agent, or a new agent in an
 * existing agent session, or give an agent its task.
 */

static enum cmd_retval	cmd_new_agent_session_exec(struct cmd *,
			    struct cmdq_item *);
static enum cmd_retval	cmd_new_agent_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_set_agent_task_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_new_agent_session_entry = {
	.name = "new-agent-session",
	.alias = "newas",

	.args = { "a:Ac:dDe:EF:f:ln:N:Ps:t:x:Xy:", 0, -1, NULL },
	.usage = "[-AdDElPX] [-a agent] [-c start-directory] [-e environment] "
		 "[-F format] [-f flags] [-n window-name] [-N agents] "
		 "[-s session-name] "
		 CMD_TARGET_SESSION_USAGE " [-x width] [-y height] "
		 "[shell-command]",

	.target = { 't', CMD_FIND_SESSION, CMD_FIND_CANFAIL },

	.flags = CMD_STARTSERVER,
	.exec = cmd_new_agent_session_exec
};

const struct cmd_entry cmd_new_agent_entry = {
	.name = "new-agent",
	.alias = "newa",

	.args = { "abc:de:F:kn:PSt:", 0, -1, NULL },
	.usage = "[-abdkPS] [-c start-directory] [-e environment] [-F format] "
		 "[-n agent-name] " CMD_TARGET_WINDOW_USAGE " [shell-command]",

	.target = { 't', CMD_FIND_WINDOW, CMD_FIND_WINDOW_INDEX },

	.flags = 0,
	.exec = cmd_new_agent_exec
};

const struct cmd_entry cmd_set_agent_task_entry = {
	.name = "set-agent-task",
	.alias = "setat",

	.args = { "pt:u", 0, 1, NULL },
	.usage = "[-pu] " CMD_TARGET_WINDOW_USAGE " [task]",

	.target = { 't', CMD_FIND_WINDOW, 0 },

	.flags = 0,
	.exec = cmd_set_agent_task_exec
};

static enum cmd_retval
cmd_new_agent_session_exec(struct cmd *self, struct cmdq_item *item)
{
	return (cmd_new_session_run(self, item, 1));
}

static enum cmd_retval
cmd_new_agent_exec(struct cmd *self, struct cmdq_item *item)
{
	struct session	*s = cmdq_get_target(item)->s;

	if (~s->flags & SESSION_AGENTS) {
		cmdq_error(item, "%s is not an agent session", s->name);
		return (CMD_RETURN_ERROR);
	}
	return (cmd_new_window_run(self, item));
}

static enum cmd_retval
cmd_set_agent_task_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*w = target->wl->window;
	struct window_pane	*wp = w->active;
	const char		*task;

	if (w->agent != WINDOW_AGENT_HEAD && w->agent != WINDOW_AGENT_WORKER) {
		cmdq_error(item, "%s is not an agent", w->name);
		return (CMD_RETURN_ERROR);
	}

	free(w->agent_task);
	w->agent_task = NULL;
	if (args_has(args, 'u'))
		return (CMD_RETURN_NORMAL);
	if (args_count(args) != 1) {
		cmdq_error(item, "no task given");
		return (CMD_RETURN_ERROR);
	}
	task = args_string(args, 0);
	if (*task != '\0')
		w->agent_task = xstrdup(task);

	if (args_has(args, 'p') && *task != '\0' && wp != NULL &&
	    wp->fd != -1 && (~wp->flags & PANE_INPUTOFF)) {
		bufferevent_write(wp->event, task, strlen(task));
		bufferevent_write(wp->event, "\r", 1);
	}
	server_status_window(w);
	return (CMD_RETURN_NORMAL);
}
