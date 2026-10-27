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

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Agent sessions. Each window in an agent session is an agent. The first
 * window is the head agent; the output of every other agent is stripped of
 * escape sequences, split into lines tagged with the agent and fed to the
 * head agent, either through a feed file (named by $TMUX_AGENT_FEED) or by
 * writing it to the head agent's input.
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

/* Set up the feed file for a new agent session. */
void
agent_session_init(struct session *s)
{
	s->flags |= SESSION_AGENTS;

	xasprintf(&s->agent_feed, "%s-agents-%u.log", socket_path, s->id);
	s->agent_fd = open(s->agent_feed, O_WRONLY|O_CREAT|O_TRUNC|O_APPEND,
	    0600);
	if (s->agent_fd == -1)
		log_debug("%s: open %s failed", __func__, s->agent_feed);
	environ_set(s->environ, "TMUX_AGENT_FEED", 0, "%s", s->agent_feed);
}

/* Close and remove the feed file. */
void
agent_session_free(struct session *s)
{
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
	case WINDOW_AGENT_HEAD:
		return ("head");
	case WINDOW_AGENT_WORKER:
		return ("agent");
	}
	return ("");
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
