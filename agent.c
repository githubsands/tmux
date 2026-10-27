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
#include <sys/time.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Agent sessions. The first window is the operator, a shell for the user. The
 * second is the head agent, named control-agent; every other window is an
 * agent whose output is stripped of escape sequences, split into lines tagged
 * with the agent and multiplexed to the head agent, either through a feed file
 * (named by $TMUX_AGENT_FEED) or by writing it to the head agent's input.
 *
 * Each claude agent is started with its own conversation (--session-id) and
 * the conversation of every agent is recorded in a file named by
 * $TMUX_AGENT_RECORD. If a claude agent exits, it is restarted with --resume
 * so it carries on from its saved context.
 */

#define AGENT_STATE_TEXT 0
#define AGENT_STATE_ESC 1
#define AGENT_STATE_CSI 2
#define AGENT_STATE_STRING 3
#define AGENT_STATE_STRING_ESC 4

#define AGENT_LINE_MAX 1024

#define AGENT_FEED_OFF 0
#define AGENT_FEED_FILE 1
#define AGENT_FEED_INPUT 2
#define AGENT_FEED_BOTH 3

/* Set up the feed and record files for a new agent session. */
void
agent_session_init(struct session *s)
{
	char	*cp;

	s->flags |= SESSION_AGENTS;

	xasprintf(&s->agent_record, "%s-%s.agents", socket_path, s->name);
	for (cp = s->agent_record + strlen(socket_path) + 1; *cp != '\0'; cp++) {
		if (*cp == '/')
			*cp = '_';
	}
	environ_set(s->environ, "TMUX_AGENT_RECORD", 0, "%s", s->agent_record);

	xasprintf(&s->agent_feed, "%s-agents-%u.log", socket_path, s->id);
	s->agent_fd = open(s->agent_feed, O_WRONLY|O_CREAT|O_TRUNC|O_APPEND,
	    0600);
	if (s->agent_fd == -1)
		log_debug("%s: open %s failed", __func__, s->agent_feed);
	environ_set(s->environ, "TMUX_AGENT_FEED", 0, "%s", s->agent_feed);
}

/*
 * Close and remove the feed file. The record file is kept so the agents'
 * conversations can still be found after the session is gone.
 */
void
agent_session_free(struct session *s)
{
	free(s->agent_record);
	s->agent_record = NULL;

	if (s->agent_fd != -1) {
		close(s->agent_fd);
		s->agent_fd = -1;
	}
	if (s->agent_feed != NULL) {
		unlink(s->agent_feed);
		free(s->agent_feed);
		s->agent_feed = NULL;
	}
}

/* Find the head agent pane of a session. */
struct window_pane *
agent_head(struct session *s)
{
	struct winlink	*wl;

	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl->window->agent == WINDOW_AGENT_HEAD)
			return (wl->window->active);
	}
	return (NULL);
}

/* Command for the agent selected by the agent option of a session. */
const char *
agent_command(struct session *s)
{
	struct options_entry	*o = options_get(s->options, "agent");

	return (options_table_entry(o)->choices[options_get_number(s->options,
	    "agent")]);
}

/* Name of a window's agent role. */
const char *
agent_role(struct window *w)
{
	switch (w->agent) {
	case WINDOW_AGENT_OPERATOR:
		return ("operator");
	case WINDOW_AGENT_HEAD:
		return ("control");
	case WINDOW_AGENT_WORKER:
		return ("agent");
	}
	return ("");
}

/* Next default name for an agent in a session: agent-1, agent-2 and so on. */
char *
agent_name(struct session *s)
{
	char	*name;

	xasprintf(&name, "agent-%u", ++s->agent_next);
	return (name);
}

/*
 * Status of an agent: exited if its pane is dead, running if it has written
 * output in the last AGENT_IDLE_TIME seconds, otherwise idle. The operator is
 * not an agent and has no status.
 */
const char *
agent_status(struct window *w)
{
	struct window_pane	*wp = w->active;
	struct timeval		 tv;

	if (w->agent == WINDOW_AGENT_NONE || w->agent == WINDOW_AGENT_OPERATOR)
		return ("");
	if (wp == NULL || wp->fd == -1 || (wp->flags & PANE_EXITED))
		return ("exited");
	if (gettimeofday(&tv, NULL) == 0 &&
	    tv.tv_sec - w->activity_time.tv_sec < AGENT_IDLE_TIME)
		return ("running");
	return ("idle");
}

/* Deliver one line of agent output to each agent session it belongs to. */
static void
agent_emit(struct window_pane *wp, const char *line, size_t len)
{
	struct window		*w = wp->window;
	struct winlink		*wl;
	struct session		*s;
	struct window_pane	*head;
	char			*prefix;
	int			 feed, n;

	TAILQ_FOREACH(wl, &w->winlinks, wentry) {
		s = wl->session;
		if (~s->flags & SESSION_AGENTS)
			continue;
		feed = options_get_number(s->options, "agent-feed");
		if (feed == AGENT_FEED_OFF)
			continue;
		n = xasprintf(&prefix, "[%d:%s] ", wl->idx, w->name);

		if ((feed == AGENT_FEED_FILE || feed == AGENT_FEED_BOTH) &&
		    s->agent_fd != -1) {
			if (write(s->agent_fd, prefix, n) != -1 &&
			    write(s->agent_fd, line, len) != -1)
				write(s->agent_fd, "\n", 1);
		}

		head = agent_head(s);
		if ((feed == AGENT_FEED_INPUT || feed == AGENT_FEED_BOTH) &&
		    head != NULL &&
		    head != wp &&
		    head->fd != -1 &&
		    (~head->flags & PANE_INPUTOFF)) {
			bufferevent_write(head->event, prefix, n);
			bufferevent_write(head->event, line, len);
			bufferevent_write(head->event, "\r", 1);
		}

		free(prefix);
	}
}

static void
agent_flush(struct window_pane *wp)
{
	struct evbuffer	*evb = wp->agent_line;

	if (EVBUFFER_LENGTH(evb) != 0) {
		agent_emit(wp, (const char *)EVBUFFER_DATA(evb),
		    EVBUFFER_LENGTH(evb));
		evbuffer_drain(evb, EVBUFFER_LENGTH(evb));
	}
}

/* Consume output from an agent pane before it is parsed. */
void
agent_pane_output(struct window_pane *wp, const u_char *buf, size_t len)
{
	size_t	i;
	u_char	ch;

	if (wp->window->agent != WINDOW_AGENT_WORKER)
		return;
	if (wp->agent_line == NULL)
		wp->agent_line = evbuffer_new();

	for (i = 0; i < len; i++) {
		ch = buf[i];
		switch (wp->agent_state) {
		case AGENT_STATE_TEXT:
			if (ch == '\033')
				wp->agent_state = AGENT_STATE_ESC;
			else if (ch == '\n')
				agent_flush(wp);
			else if (ch == '\t' || (ch >= 0x20 && ch != 0x7f))
				evbuffer_add(wp->agent_line, &ch, 1);
			if (EVBUFFER_LENGTH(wp->agent_line) >= AGENT_LINE_MAX)
				agent_flush(wp);
			break;
		case AGENT_STATE_ESC:
			if (ch == '[')
				wp->agent_state = AGENT_STATE_CSI;
			else if (strchr("]P_^X", ch) != NULL)
				wp->agent_state = AGENT_STATE_STRING;
			else if (ch < 0x20 || ch > 0x2f)
				wp->agent_state = AGENT_STATE_TEXT;
			break;
		case AGENT_STATE_CSI:
			if (ch >= 0x40 && ch <= 0x7e)
				wp->agent_state = AGENT_STATE_TEXT;
			break;
		case AGENT_STATE_STRING:
			if (ch == '\007')
				wp->agent_state = AGENT_STATE_TEXT;
			else if (ch == '\033')
				wp->agent_state = AGENT_STATE_STRING_ESC;
			break;
		case AGENT_STATE_STRING_ESC:
			if (ch == '\\')
				wp->agent_state = AGENT_STATE_TEXT;
			else
				wp->agent_state = AGENT_STATE_STRING;
			break;
		}
	}
}

/* Whether a command runs claude, which can save and resume a conversation. */
static int
agent_is_claude(const char *cmd)
{
	const char	*end, *base;
	size_t		 len;

	cmd += strspn(cmd, " \t");
	end = cmd + strcspn(cmd, " \t");
	for (base = cmd; cmd != end; cmd++) {
		if (*cmd == '/')
			base = cmd + 1;
	}
	len = end - base;
	return (len == 6 && strncmp(base, "claude", 6) == 0);
}

/* Make a random (version 4) UUID for a claude conversation. */
static char *
agent_uuid(void)
{
	u_char	 b[16];
	char	*uuid;

	arc4random_buf(b, sizeof b);
	b[6] = (b[6] & 0x0f) | 0x40;
	b[8] = (b[8] & 0x3f) | 0x80;
	xasprintf(&uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
	    "%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5],
	    b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return (uuid);
}

/*
 * Command to start a claude agent with: give it a new conversation the first
 * time and resume that conversation after. Returns NULL for other commands.
 */
char *
agent_spawn_command(struct window *w, const char *cmd)
{
	char	*new;

	if (w->agent != WINDOW_AGENT_HEAD && w->agent != WINDOW_AGENT_WORKER)
		return (NULL);
	if (!agent_is_claude(cmd))
		return (NULL);

	if (w->agent_context == NULL) {
		w->agent_context = agent_uuid();
		xasprintf(&new, "%s --session-id %s", cmd, w->agent_context);
	} else
		xasprintf(&new, "%s --resume %s", cmd, w->agent_context);
	return (new);
}

/* Write the record of every agent in a session to the record file. */
void
agent_save(struct session *s)
{
	struct winlink		*wl;
	struct window		*w;
	struct window_pane	*wp;
	FILE			*f;
	char			*path;

	if (s->agent_record == NULL)
		return;
	xasprintf(&path, "%s.tmp", s->agent_record);
	if ((f = fopen(path, "w")) == NULL) {
		log_debug("%s: open %s failed", __func__, path);
		free(path);
		return;
	}
	fprintf(f, "# session %s: index name role conversation restores cwd\n",
	    s->name);
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (w->agent == WINDOW_AGENT_NONE)
			continue;
		wp = w->active;
		fprintf(f, "%d\t%s\t%s\t%s\t%u\t%s\n", wl->idx, w->name,
		    agent_role(w),
		    w->agent_context != NULL ? w->agent_context : "-",
		    w->agent_restores,
		    (wp != NULL && wp->cwd != NULL) ? wp->cwd : "-");
	}
	if (fclose(f) != 0 || rename(path, s->agent_record) != 0) {
		log_debug("%s: write %s failed", __func__, s->agent_record);
		unlink(path);
	}
	free(path);
}

/*
 * Restart a claude agent whose process has exited, resuming its conversation.
 * Returns 1 if the pane is being restarted and should be kept.
 */
int
agent_restore(struct window_pane *wp)
{
	struct window		 *w = wp->window;
	struct winlink		 *wl;
	struct session		 *s;
	struct cmd_parse_result	 *pr;
	struct args_value	 *values;
	char			 *argv[4], *target, *cmd;
	time_t			  now;
	u_int			  i;

	if (w->agent_context == NULL || wp->argc != 1)
		return (0);
	if (TAILQ_FIRST(&w->panes) != wp || TAILQ_NEXT(wp, entry) != NULL)
		return (0);
	if ((wl = TAILQ_FIRST(&w->winlinks)) == NULL)
		return (0);
	s = wl->session;
	if ((~s->flags & SESSION_AGENTS) ||
	    !options_get_number(s->options, "agent-restore"))
		return (0);
	if (wp->flags & PANE_STATUSDRAWN)
		return (1); /* already being restarted */

	/* Give up if the agent keeps exiting straight away. */
	now = time(NULL);
	if (now - w->agent_restore_time > AGENT_RESTORE_TIME)
		w->agent_restores = 0;
	if (w->agent_restores >= AGENT_RESTORE_MAX) {
		log_debug("%s: @%u exited %u times, not restoring", __func__,
		    w->id, w->agent_restores);
		return (0);
	}
	w->agent_restores++;
	w->agent_restore_time = now;
	wp->flags |= PANE_STATUSDRAWN;

	/* Replace --session-id or --resume and the conversation. */
	cmd = xstrdup(wp->argv[0]);
	for (i = 0; i < 2; i++) {
		const char	*flag = (i == 0) ? " --session-id " : " --resume ";
		char		*cp = strstr(cmd, flag);

		if (cp != NULL)
			*cp = '\0';
	}

	xasprintf(&target, "%%%u", wp->id);
	argv[0] = xstrdup("respawn-pane");
	argv[1] = xstrdup("-t");
	argv[2] = target;
	xasprintf(&argv[3], "%s --resume %s", cmd, w->agent_context);
	free(cmd);

	log_debug("%s: @%u restoring: %s", __func__, w->id, argv[3]);
	values = args_from_vector(4, argv);
	pr = cmd_parse_from_arguments(values, 4, NULL);
	if (pr->status == CMD_PARSE_SUCCESS) {
		cmdq_append(NULL, cmdq_get_command(pr->cmdlist, NULL));
		cmd_list_free(pr->cmdlist);
	} else {
		log_debug("%s: %s", __func__, pr->error);
		free(pr->error);
	}
	args_free_values(values, 4);
	free(values);
	for (i = 0; i < 4; i++)
		free(argv[i]);

	agent_save(s);
	return (1);
}
