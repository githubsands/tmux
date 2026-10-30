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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#endif

#include "tmux.h"

/*
 * Tree of every tmux server with a socket in the same directory as this one,
 * with their sessions and windows (or agents) underneath. The other servers
 * are asked for their windows by running their own binary, since stock tmux
 * does not speak the same protocol as this one. Choosing an item on another
 * server replaces the client with one attached to that server.
 */

static struct screen	*window_server_init(struct window_mode_entry *,
			     struct cmd_find_state *, struct args *);
static void		 window_server_free(struct window_mode_entry *);
static void		 window_server_resize(struct window_mode_entry *, u_int,
			     u_int);
static void		 window_server_update(struct window_mode_entry *);
static void		 window_server_key(struct window_mode_entry *,
			     struct client *, struct session *,
			     struct winlink *, key_code, struct mouse_event *);

#define WINDOW_SERVER_MAX 36
#define WINDOW_SERVER_TIMEOUT 500
#define WINDOW_SERVER_REFRESH 2

/* Fields asked of each server, one line per window, separated by tabs. */
#define WINDOW_SERVER_FORMAT \
	"#{session_name}\t#{session_attached}\t#{session_agents}\t" \
	"#{window_index}\t#{window_name}\t#{window_agent}\t" \
	"#{window_agent_status}\t#{window_active}\t#{window_agent_key}\t" \
	"#{window_agent_question}\t#{window_agent_task}"

static const struct menu_item window_server_menu_items[] = {
	{ "Select", '\r', NULL },
	{ "", KEYC_NONE, NULL },
	{ "Cancel", 'q', NULL },

	{ NULL, KEYC_NONE, NULL }
};

const struct window_mode window_server_mode = {
	.name = "server-mode",

	.init = window_server_init,
	.free = window_server_free,
	.resize = window_server_resize,
	.update = window_server_update,
	.key = window_server_key,
};

enum window_server_type {
	WINDOW_SERVER_SERVER,
	WINDOW_SERVER_SESSION,
	WINDOW_SERVER_WINDOW
};

struct window_server_server {
	char	*name;
	char	*path;
	char	*binary;
	int	 current;
	char	*output;
};

struct window_server_itemdata {
	enum window_server_type	 type;
	struct window_server_server *server;
	char			*session;
	int			 idx;
};

struct window_server_modedata {
	struct window_pane		 *wp;

	struct mode_tree_data		 *data;
	struct event			  timer;

	struct window_server_server	**server_list;
	u_int				  server_size;
	time_t				  server_time;
	uint64_t			  start_tag;

	struct window_server_itemdata	**item_list;
	u_int				  item_size;

	char				 *preview_target;
	char				 *preview;
	time_t				  preview_time;
};

/* Path of the binary running a process. */
static char *
window_server_binary(pid_t pid)
{
#if defined(__APPLE__)
	char	path[PROC_PIDPATHINFO_MAXSIZE];

	if (proc_pidpath(pid, path, sizeof path) <= 0)
		return (NULL);
	return (xstrdup(path));
#elif defined(__linux__)
	char	proc[64], path[PATH_MAX];
	ssize_t	n;

	xsnprintf(proc, sizeof proc, "/proc/%ld/exe", (long)pid);
	if ((n = readlink(proc, path, sizeof path - 1)) <= 0)
		return (NULL);
	path[n] = '\0';
	return (xstrdup(path));
#else
	(void)pid;
	return (NULL);
#endif
}

/* Path of the binary running the process at the other end of a socket. */
static char *
window_server_peer_binary(int fd)
{
#if defined(__APPLE__)
	pid_t		pid;
	socklen_t	len = sizeof pid;

	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) != 0)
		return (NULL);
	return (window_server_binary(pid));
#elif defined(__linux__)
	struct ucred	uc;
	socklen_t	len = sizeof uc;

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) != 0)
		return (NULL);
	return (window_server_binary(uc.pid));
#else
	(void)fd;
	return (NULL);
#endif
}

/* Check a server is listening on a socket and find its binary. */
static int
window_server_probe(const char *path, char **binary)
{
	struct sockaddr_un	sa;
	int			fd;

	*binary = NULL;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, path, sizeof sa.sun_path) >=
	    sizeof sa.sun_path)
		return (0);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return (0);
	setblocking(fd, 0);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1 &&
	    errno != EINPROGRESS) {
		close(fd);
		return (0);
	}
	*binary = window_server_peer_binary(fd);
	close(fd);
	if (*binary == NULL)
		*binary = xstrdup("tmux");
	return (1);
}

/*
 * Run a command on another server with its own binary and return what it
 * printed. Give up after WINDOW_SERVER_TIMEOUT milliseconds so a stuck server
 * cannot hang this one.
 */
static char *
window_server_run(struct window_server_server *ws, const char **args,
    u_int nargs)
{
	const char	**argv;
	struct pollfd	  pfd;
	struct evbuffer	 *evb;
	sigset_t	  set, oldset;
	pid_t		  pid;
	int		  fds[2], fd, status;
	u_int		  i;
	uint64_t	  deadline, now;
	char		  buf[BUFSIZ], *out;
	ssize_t		  n;

	argv = xcalloc(nargs + 4, sizeof *argv);
	argv[0] = ws->binary;
	argv[1] = "-S";
	argv[2] = ws->path;
	for (i = 0; i < nargs; i++)
		argv[3 + i] = args[i];

	if (pipe(fds) != 0) {
		free(argv);
		return (NULL);
	}

	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, &oldset);
	switch (pid = fork()) {
	case -1:
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		close(fds[0]);
		close(fds[1]);
		free(argv);
		return (NULL);
	case 0:
		proc_clear_signals(server_proc, 1);
		sigprocmask(SIG_SETMASK, &oldset, NULL);

		if (dup2(fds[1], STDOUT_FILENO) == -1)
			_exit(1);
		if ((fd = open(_PATH_DEVNULL, O_RDWR)) != -1) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDERR_FILENO);
		}
		closefrom(STDERR_FILENO + 1);
		execvp(argv[0], (char **)argv);
		_exit(1);
	}
	sigprocmask(SIG_SETMASK, &oldset, NULL);
	close(fds[1]);
	free(argv);

	evb = evbuffer_new();
	deadline = get_timer() + WINDOW_SERVER_TIMEOUT;
	pfd.fd = fds[0];
	pfd.events = POLLIN;
	for (;;) {
		now = get_timer();
		if (now >= deadline)
			break;
		if (poll(&pfd, 1, deadline - now) <= 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((n = read(fds[0], buf, sizeof buf)) <= 0)
			break;
		evbuffer_add(evb, buf, n);
	}
	close(fds[0]);

	if (waitpid(pid, &status, WNOHANG) == 0) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}

	evbuffer_add(evb, "", 1);
	out = xstrdup((const char *)EVBUFFER_DATA(evb));
	evbuffer_free(evb);
	return (out);
}

/* List the windows of this server in the same form as another server would. */
static char *
window_server_local(void)
{
	struct session	*s;
	struct winlink	*wl;
	struct evbuffer	*evb;
	char		*line, *out;

	evb = evbuffer_new();
	RB_FOREACH(s, sessions, &sessions) {
		RB_FOREACH(wl, winlinks, &s->windows) {
			line = format_single(NULL, WINDOW_SERVER_FORMAT, NULL,
			    s, wl, NULL);
			evbuffer_add_printf(evb, "%s\n", line);
			free(line);
		}
	}
	evbuffer_add(evb, "", 1);
	out = xstrdup((const char *)EVBUFFER_DATA(evb));
	evbuffer_free(evb);
	return (out);
}

static void
window_server_free_servers(struct window_server_modedata *data)
{
	struct window_server_server	*ws;
	u_int				 i;

	for (i = 0; i < data->server_size; i++) {
		ws = data->server_list[i];
		free(ws->name);
		free(ws->path);
		free(ws->binary);
		free(ws->output);
		free(ws);
	}
	free(data->server_list);
	data->server_list = NULL;
	data->server_size = 0;
}

static int
window_server_cmp(const void *a0, const void *b0)
{
	const struct window_server_server *const	*a = a0;
	const struct window_server_server *const	*b = b0;

	return (strcmp((*a)->name, (*b)->name));
}

/* Find the servers in the socket directory and ask each for its windows. */
static void
window_server_scan(struct window_server_modedata *data)
{
	struct window_server_server	*ws;
	DIR				*dir;
	struct dirent			*de;
	struct stat			 sb;
	char				*base, *path, *binary;
	const char			*args[] = {
		"list-windows", "-a", "-F", WINDOW_SERVER_FORMAT
	};
	u_int				 i;

	window_server_free_servers(data);
	data->server_time = time(NULL);

	base = xstrdup(socket_path);
	if (strrchr(base, '/') != NULL)
		*strrchr(base, '/') = '\0';
	if ((dir = opendir(base)) == NULL) {
		free(base);
		return;
	}
	while ((de = readdir(dir)) != NULL &&
	    data->server_size < WINDOW_SERVER_MAX) {
		if (*de->d_name == '.')
			continue;
		xasprintf(&path, "%s/%s", base, de->d_name);
		if (lstat(path, &sb) != 0 || !S_ISSOCK(sb.st_mode)) {
			free(path);
			continue;
		}

		binary = NULL;
		if (strcmp(path, socket_path) != 0 &&
		    !window_server_probe(path, &binary)) {
			free(path);
			continue;
		}

		ws = xcalloc(1, sizeof *ws);
		ws->name = xstrdup(de->d_name);
		ws->path = path;
		if (binary == NULL) {
			ws->current = 1;
			ws->binary = window_server_binary(getpid());
			if (ws->binary == NULL)
				ws->binary = xstrdup("tmux");
		} else
			ws->binary = binary;

		data->server_list = xreallocarray(data->server_list,
		    data->server_size + 1, sizeof *data->server_list);
		data->server_list[data->server_size++] = ws;
	}
	closedir(dir);
	free(base);

	qsort(data->server_list, data->server_size, sizeof *data->server_list,
	    window_server_cmp);
	for (i = 0; i < data->server_size; i++) {
		ws = data->server_list[i];
		if (ws->current)
			ws->output = window_server_local();
		else
			ws->output = window_server_run(ws, args, nitems(args));
	}
}

/* Tag for an item that stays the same when the tree is rebuilt. */
static uint64_t
window_server_tag(const char *path, const char *session, int idx)
{
	uint64_t	 hash = 14695981039346656037ULL;
	const char	*cp;

	for (cp = path; *cp != '\0'; cp++)
		hash = (hash ^ (u_char)*cp) * 1099511628211ULL;
	hash = (hash ^ '\n') * 1099511628211ULL;
	if (session != NULL) {
		for (cp = session; *cp != '\0'; cp++)
			hash = (hash ^ (u_char)*cp) * 1099511628211ULL;
		hash = (hash ^ '\n') * 1099511628211ULL;
		hash = (hash ^ (uint64_t)(idx + 1)) * 1099511628211ULL;
	}
	return (hash);
}

static struct window_server_itemdata *
window_server_add_item(struct window_server_modedata *data,
    enum window_server_type type, struct window_server_server *ws,
    const char *session, int idx)
{
	struct window_server_itemdata	*item;

	data->item_list = xreallocarray(data->item_list, data->item_size + 1,
	    sizeof *data->item_list);
	item = data->item_list[data->item_size++] = xcalloc(1, sizeof *item);
	item->type = type;
	item->server = ws;
	item->session = (session == NULL) ? NULL : xstrdup(session);
	item->idx = idx;
	return (item);
}

static void
window_server_free_items(struct window_server_modedata *data)
{
	u_int	i;

	for (i = 0; i < data->item_size; i++) {
		free(data->item_list[i]->session);
		free(data->item_list[i]);
	}
	free(data->item_list);
	data->item_list = NULL;
	data->item_size = 0;
}

/* Binary name shown with home replaced by ~. */
static char *
window_server_short(const char *binary)
{
	const char	*home = find_home();
	size_t		 len;
	char		*s;

	if (home != NULL && (len = strlen(home)) != 0 &&
	    strncmp(binary, home, len) == 0 && binary[len] == '/')
		xasprintf(&s, "~%s", binary + len);
	else
		s = xstrdup(binary);
	return (s);
}

/* Split one line of server output into its fields. */
static int
window_server_split(char *line, char **fields, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++) {
		fields[i] = strsep(&line, "\t");
		if (fields[i] == NULL)
			return (0);
	}
	return (1);
}

static void
window_server_build_server(struct window_server_modedata *data,
    struct window_server_server *ws)
{
	struct window_server_itemdata	*item;
	struct mode_tree_item		*mti, *smti = NULL;
	char				*copy, *next, *line, *f[11], *text;
	char				*last = NULL, *shown, *copy2, *next2;
	char				*line2, *g[11], *name, *key = NULL;
	char				*question = NULL, *task = NULL;
	char				 waiting[32];
	u_int				 nsessions = 0, count, nwaiting;
	int				 idx;

	/* Count sessions for the server line. */
	copy = next = xstrdup(ws->output != NULL ? ws->output : "");
	while ((line = strsep(&next, "\n")) != NULL) {
		if (!window_server_split(line, f, nitems(f)))
			continue;
		if (last == NULL || strcmp(last, f[0]) != 0)
			nsessions++;
		last = f[0];
	}
	free(copy);

	shown = window_server_short(ws->binary);
	xasprintf(&text, "%s - %u session%s%s", shown, nsessions,
	    nsessions == 1 ? "" : "s", ws->current ? " (current)" : "");
	free(shown);
	item = window_server_add_item(data, WINDOW_SERVER_SERVER, ws, NULL, -1);
	mti = mode_tree_add(data->data, NULL, item,
	    window_server_tag(ws->path, NULL, -1), ws->name, text, -1);
	free(text);

	last = NULL;
	copy = next = xstrdup(ws->output != NULL ? ws->output : "");
	while ((line = strsep(&next, "\n")) != NULL) {
		if (!window_server_split(line, f, nitems(f)))
			continue;

		if (last == NULL || strcmp(last, f[0]) != 0) {
			/*
			 * Count the windows in this session and the agents
			 * waiting on a question.
			 */
			copy2 = next2 = xstrdup(next != NULL ? next : "");
			count = 1;
			nwaiting = (*f[9] != '\0');
			while ((line2 = strsep(&next2, "\n")) != NULL) {
				if (!window_server_split(line2, g, nitems(g)))
					continue;
				if (strcmp(g[0], f[0]) != 0)
					break;
				count++;
				if (*g[9] != '\0')
					nwaiting++;
			}
			free(copy2);

			if (*f[2] == '1') {
				*waiting = '\0';
				if (nwaiting != 0) {
					xsnprintf(waiting, sizeof waiting,
					    ", %u waiting", nwaiting);
				}
				xasprintf(&text, "%u agent%s%s%s", count,
				    count == 1 ? "" : "s", waiting,
				    strcmp(f[1], "0") != 0 ? " (attached)" : "");
			} else {
				xasprintf(&text, "%u window%s%s", count,
				    count == 1 ? "" : "s",
				    strcmp(f[1], "0") != 0 ? " (attached)" : "");
			}
			item = window_server_add_item(data,
			    WINDOW_SERVER_SESSION, ws, f[0], -1);
			smti = mode_tree_add(data->data, mti, item,
			    window_server_tag(ws->path, f[0], -1), f[0], text,
			    -1);
			free(text);
			last = f[0];
		}

		idx = (int)strtonum(f[3], 0, INT_MAX, NULL);
		if (*f[6] != '\0') {
			if (*f[8] != '\0')
				xasprintf(&key, " %s", f[8]);
			if (*f[9] != '\0')
				xasprintf(&question, " \"%s\"", f[9]);
			if (*f[10] != '\0')
				xasprintf(&task, " %s", f[10]);
			xasprintf(&text, "%s%s [%s]%s%s%s%s", f[4],
			    *f[7] == '1' ? "*" : "", f[6],
			    task != NULL ? task : "",
			    strcmp(f[5], "control") == 0 ? " (control)" : "",
			    question != NULL ? question : "",
			    key != NULL ? key : "");
			free(task);
			task = NULL;
			free(question);
			question = NULL;
			free(key);
			key = NULL;
		} else
			xasprintf(&text, "%s%s", f[4], *f[7] == '1' ? "*" : "");
		xasprintf(&name, "%d", idx);
		item = window_server_add_item(data, WINDOW_SERVER_WINDOW, ws,
		    f[0], idx);
		mode_tree_add(data->data, smti, item,
		    window_server_tag(ws->path, f[0], idx), name, text, -1);
		free(name);
		free(text);
	}
	free(copy);
}

static void
window_server_build(void *modedata,
    __unused struct mode_tree_sort_criteria *sort_crit, uint64_t *tag,
    __unused const char *filter)
{
	struct window_server_modedata	*data = modedata;
	u_int				 i;

	window_server_free_items(data);
	if (data->server_list == NULL ||
	    time(NULL) - data->server_time >= WINDOW_SERVER_REFRESH)
		window_server_scan(data);
	else {
		/* This server is cheap to list, so always keep it current. */
		for (i = 0; i < data->server_size; i++) {
			if (data->server_list[i]->current) {
				free(data->server_list[i]->output);
				data->server_list[i]->output =
				    window_server_local();
			}
		}
	}

	for (i = 0; i < data->server_size; i++)
		window_server_build_server(data, data->server_list[i]);

	/* Start on the window the mode was entered from. */
	if (data->start_tag != 0) {
		*tag = data->start_tag;
		data->start_tag = 0;
	}
}

/* Target for an item as understood by attach-session or switch-client. */
static char *
window_server_target(struct window_server_itemdata *item)
{
	char	*target;

	if (item->type == WINDOW_SERVER_SERVER)
		return (NULL);
	if (item->type == WINDOW_SERVER_SESSION)
		xasprintf(&target, "=%s:", item->session);
	else
		xasprintf(&target, "=%s:%d", item->session, item->idx);
	return (target);
}

/* Draw lines of text into the preview, as many as fit from the bottom. */
static void
window_server_draw_text(struct screen_write_ctx *ctx, const char *text,
    u_int sx, u_int sy)
{
	struct screen	*s = ctx->s;
	u_int		 cx = s->cx, cy = s->cy, n = 0, i = 0, skip;
	char		*copy, *next, *line;
	size_t		 len;

	copy = xstrdup(text);
	len = strlen(copy);
	while (len != 0 && copy[len - 1] == '\n')
		copy[--len] = '\0';
	for (next = copy; *next != '\0'; next++) {
		if (*next == '\n')
			n++;
	}
	n++;
	skip = (n > sy) ? n - sy : 0;

	next = copy;
	while ((line = strsep(&next, "\n")) != NULL) {
		if (skip != 0) {
			skip--;
			continue;
		}
		screen_write_cursormove(ctx, cx, cy + i++, 0);
		screen_write_nputs(ctx, sx, &grid_default_cell, "%s", line);
	}
	free(copy);
}

/* An agent's armored public key, if it has one. */
static char *
window_server_public_key(struct window *w)
{
	struct evbuffer	*evb;
	FILE		*f;
	char		*home, *path, buf[BUFSIZ], *out = NULL;
	size_t		 n;

	if (w->agent_id == NULL || w->agent_key == NULL)
		return (NULL);
	home = agent_key_home(w);
	xasprintf(&path, "%s/public.asc", home);
	free(home);
	f = fopen(path, "r");
	free(path);
	if (f == NULL)
		return (NULL);

	evb = evbuffer_new();
	while ((n = fread(buf, 1, sizeof buf, f)) != 0)
		evbuffer_add(evb, buf, n);
	fclose(f);
	if (EVBUFFER_LENGTH(evb) != 0) {
		evbuffer_add(evb, "", 1);
		out = xstrdup((const char *)EVBUFFER_DATA(evb));
	}
	evbuffer_free(evb);
	return (out);
}

/*
 * Draw the question an agent is waiting on and its public key at the top of
 * the preview and return how many lines they took, leaving one blank line
 * under each.
 */
static u_int
window_server_draw_key(struct screen_write_ctx *ctx, struct window *w,
    u_int sx, u_int sy)
{
	struct screen		*s = ctx->s;
	struct grid_cell	 gc;
	u_int			 cx = s->cx, cy = s->cy, i = 0;
	char			*key, *copy, *next, *line, *question;

	if ((question = agent_question(w)) != NULL && sy >= 2) {
		memcpy(&gc, &grid_default_cell, sizeof gc);
		gc.attr |= GRID_ATTR_BRIGHT;
		screen_write_cursormove(ctx, cx, cy, 0);
		screen_write_nputs(ctx, sx, &gc, "question: %s", question);
		i = 2;
		screen_write_cursormove(ctx, cx, cy + i, 0);
	}
	free(question);

	if ((key = window_server_public_key(w)) == NULL)
		return (i);
	copy = next = key;
	while ((line = strsep(&next, "\n")) != NULL && i < sy) {
		if (*line == '\0' && (next == NULL || *next == '\0'))
			break;
		screen_write_cursormove(ctx, cx, cy + i++, 0);
		screen_write_nputs(ctx, sx, &grid_default_cell, "%s", line);
	}
	free(copy);
	if (i < sy)
		i++;
	screen_write_cursormove(ctx, cx, cy + i, 0);
	return (i);
}

static void
window_server_draw(void *modedata, void *itemdata,
    struct screen_write_ctx *ctx, u_int sx, u_int sy)
{
	struct window_server_modedata	*data = modedata;
	struct window_server_itemdata	*item = itemdata;
	struct window_server_server	*ws = item->server;
	struct session			*s;
	struct winlink			*wl;
	char				*target, *text;
	u_int				 used;
	const char			*args[] = { "capture-pane", "-p", "-t",
					    NULL };

	if (item->type == WINDOW_SERVER_SERVER) {
		xasprintf(&text, "socket: %s\nbinary: %s\n", ws->path,
		    ws->binary);
		window_server_draw_text(ctx, text, sx, sy);
		free(text);
		return;
	}

	if (ws->current) {
		s = session_find(item->session);
		if (s == NULL)
			return;
		if (item->type == WINDOW_SERVER_WINDOW)
			wl = winlink_find_by_index(&s->windows, item->idx);
		else
			wl = s->curw;
		if (wl == NULL)
			return;
		used = 0;
		if (item->type == WINDOW_SERVER_WINDOW)
			used = window_server_draw_key(ctx, wl->window, sx, sy);
		if (used < sy) {
			screen_write_preview(ctx, &wl->window->active->base, sx,
			    sy - used);
		}
		return;
	}

	/* Capture the pane from the other server, at most once a second. */
	target = window_server_target(item);
	if (data->preview == NULL || data->preview_target == NULL ||
	    strcmp(data->preview_target, target) != 0 ||
	    time(NULL) != data->preview_time) {
		free(data->preview);
		free(data->preview_target);
		args[3] = target;
		data->preview = window_server_run(ws, args, nitems(args));
		data->preview_target = xstrdup(target);
		data->preview_time = time(NULL);
	}
	if (data->preview != NULL)
		window_server_draw_text(ctx, data->preview, sx, sy);
	free(target);
}

static void
window_server_menu(void *modedata, struct client *c, key_code key)
{
	struct window_server_modedata	*data = modedata;
	struct window_pane		*wp = data->wp;
	struct window_mode_entry	*wme;

	wme = TAILQ_FIRST(&wp->modes);
	if (wme == NULL || wme->data != modedata)
		return;
	window_server_key(wme, c, NULL, NULL, key, NULL);
}

/* Refresh every second for agent status and the other servers. */
static void
window_server_timer_callback(__unused int fd, __unused short events,
    void *arg)
{
	struct window_server_modedata	*data = arg;
	struct timeval			 tv = { .tv_sec = 1 };

	mode_tree_build(data->data);
	mode_tree_draw(data->data);
	data->wp->flags |= PANE_REDRAW;
	evtimer_add(&data->timer, &tv);
}

static struct screen *
window_server_init(struct window_mode_entry *wme, struct cmd_find_state *fs,
    struct args *args)
{
	struct window_pane		*wp = wme->wp;
	struct window_server_modedata	*data;
	struct screen			*s;
	struct timeval			 tv = { .tv_sec = 1 };

	wme->data = data = xcalloc(1, sizeof *data);
	data->wp = wp;

	data->data = mode_tree_start(wp, args, window_server_build,
	    window_server_draw, NULL, window_server_menu, NULL, NULL, data,
	    window_server_menu_items, NULL, 0, &s);
	mode_tree_zoom(data->data, args);

	if (fs != NULL && fs->s != NULL && fs->wl != NULL) {
		data->start_tag = window_server_tag(socket_path, fs->s->name,
		    fs->wl->idx);
	}

	mode_tree_build(data->data);
	mode_tree_draw(data->data);

	evtimer_set(&data->timer, window_server_timer_callback, data);
	evtimer_add(&data->timer, &tv);

	return (s);
}

static void
window_server_free(struct window_mode_entry *wme)
{
	struct window_server_modedata	*data = wme->data;

	if (data == NULL)
		return;

	evtimer_del(&data->timer);
	mode_tree_free(data->data);

	window_server_free_items(data);
	window_server_free_servers(data);
	free(data->preview);
	free(data->preview_target);

	free(data);
}

static void
window_server_resize(struct window_mode_entry *wme, u_int sx, u_int sy)
{
	struct window_server_modedata	*data = wme->data;

	mode_tree_resize(data->data, sx, sy);
}

static void
window_server_update(struct window_mode_entry *wme)
{
	struct window_server_modedata	*data = wme->data;

	mode_tree_build(data->data);
	mode_tree_draw(data->data);
	data->wp->flags |= PANE_REDRAW;
}

/* Quote a string for the shell. */
static char *
window_server_quote(const char *s)
{
	struct evbuffer	*evb = evbuffer_new();
	char		*out;

	evbuffer_add(evb, "'", 1);
	for (; *s != '\0'; s++) {
		if (*s == '\'')
			evbuffer_add(evb, "'\\''", 4);
		else
			evbuffer_add(evb, s, 1);
	}
	evbuffer_add(evb, "'", 1);
	evbuffer_add(evb, "", 1);
	out = xstrdup((const char *)EVBUFFER_DATA(evb));
	evbuffer_free(evb);
	return (out);
}

/* Move the client to the chosen item. */
static void
window_server_choose(struct window_server_modedata *data, struct client *c)
{
	struct window_server_itemdata	*item;
	struct window_server_server	*ws, *cur = NULL;
	char				*target, *cmd, *t = NULL;
	char				*binary, *path, *self, *here;
	u_int				 i;

	item = mode_tree_get_current(data->data);
	ws = item->server;
	target = window_server_target(item);

	if (ws->current) {
		if (target != NULL)
			mode_tree_run_command(c, NULL, "switch-client -Zt '%%'",
			    target);
		free(target);
		return;
	}

	for (i = 0; i < data->server_size; i++) {
		if (data->server_list[i]->current)
			cur = data->server_list[i];
	}

	/*
	 * A client can only be attached to one server, so replace it with one
	 * for the chosen server. If that fails, come back to this one.
	 */
	binary = window_server_quote(ws->binary);
	path = window_server_quote(ws->path);
	self = window_server_quote(cur != NULL ? cur->binary : "tmux");
	here = window_server_quote(socket_path);
	if (target != NULL)
		t = window_server_quote(target);
	xasprintf(&cmd, "%s -S %s attach%s%s || %s -S %s attach", binary, path,
	    t != NULL ? " -t " : "", t != NULL ? t : "", self, here);
	server_client_exec(c, cmd);
	free(cmd);
	free(t);
	free(here);
	free(self);
	free(path);
	free(binary);
	free(target);
}

static void
window_server_key(struct window_mode_entry *wme, struct client *c,
    __unused struct session *s, __unused struct winlink *wl, key_code key,
    struct mouse_event *m)
{
	struct window_pane		*wp = wme->wp;
	struct window_server_modedata	*data = wme->data;
	struct mode_tree_data		*mtd = data->data;
	int				 finished;

	finished = mode_tree_key(mtd, c, &key, m, NULL, NULL);
	switch (key) {
	case '\r':
		window_server_choose(data, c);
		finished = 1;
		break;
	}
	if (finished)
		window_pane_reset_mode(wp);
	else {
		mode_tree_draw(mtd);
		wp->flags |= PANE_REDRAW;
	}
}
