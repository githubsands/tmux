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
#include <sys/wait.h>

#include <ctype.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Provision a cloud server on GCP, AWS or Azure with its command line tool
 * (gcloud, aws or az), and delete it again. Provisioned servers are recorded
 * in a file named provisioned in the socket directory, so they can be deleted
 * by name.
 */

static enum cmd_retval	cmd_provision_server_exec(struct cmd *,
			    struct cmdq_item *);
static enum cmd_retval	cmd_delete_provisioned_server_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_provision_server_entry = {
	.name = "provision-server",
	.alias = NULL,

	.args = { "i:k:l:m:p:", 1, 1, NULL },
	.usage = "[-i image] [-k key-name] [-l location] [-m machine-type] "
		 "[-p gcp|aws|azure] name",

	.flags = 0,
	.exec = cmd_provision_server_exec
};

const struct cmd_entry cmd_delete_provisioned_server_entry = {
	.name = "delete-provisioned-server",
	.alias = NULL,

	.args = { "l:p:", 1, 1, NULL },
	.usage = "[-l location] [-p gcp|aws|azure] name",

	.flags = 0,
	.exec = cmd_delete_provisioned_server_exec
};

struct cmd_provision_provider {
	const char	*name;
	const char	*tool;
	const char	*location;
	const char	*machine;
	const char	*image;
};

static const struct cmd_provision_provider cmd_provision_providers[] = {
	{ "gcp", "gcloud", "us-central1-a", "e2-small", "debian-12" },
	{ "aws", "aws", "us-east-1", "t3.small",
	  "resolve:ssm:/aws/service/ami-amazon-linux-latest/"
	  "al2023-ami-kernel-default-x86_64" },
	{ "azure", "az", "eastus", "Standard_B1s",
	  "Debian:debian-12:12-gen2:latest" },
};

struct cmd_provision_data {
	struct cmdq_item	*item;
	char			*cmd;
	int			 delete;

	char			*name;
	char			*provider;
	char			*location;
};

/* Find a provider by name. */
static const struct cmd_provision_provider *
cmd_provision_find_provider(const char *name)
{
	u_int	i;

	for (i = 0; i < nitems(cmd_provision_providers); i++) {
		if (strcmp(cmd_provision_providers[i].name, name) == 0)
			return (&cmd_provision_providers[i]);
	}
	return (NULL);
}

/*
 * Check a value is safe to put in a shell command and is accepted by all the
 * providers: a server name is lowercase letters, digits and -, starting with
 * a letter, at most 63 long; other values may also have uppercase letters and
 * ._:/ and be longer (AWS image paths are).
 */
static int
cmd_provision_check(const char *value, int name)
{
	const char	*cp;

	if (*value == '\0' || strlen(value) > (name ? 63 : 255))
		return (0);
	if (name && !islower((u_char)*value))
		return (0);
	for (cp = value; *cp != '\0'; cp++) {
		if (islower((u_char)*cp) || isdigit((u_char)*cp) || *cp == '-')
			continue;
		if (!name && (isupper((u_char)*cp) || strchr("._:/", *cp)))
			continue;
		return (0);
	}
	return (1);
}

/* Path of the file recording provisioned servers. */
static char *
cmd_provision_record_path(void)
{
	char	*copy, *path;

	copy = xstrdup(socket_path);
	xasprintf(&path, "%s/provisioned", dirname(copy));
	free(copy);
	return (path);
}

/* Look up a provisioned server; the record is name provider location id. */
static int
cmd_provision_record_find(const char *name, char **provider, char **location,
    char **id)
{
	char	*path, line[512], *fields[4], *cp, *next;
	FILE	*f;
	u_int	 n;
	int	 found = 0;

	path = cmd_provision_record_path();
	f = fopen(path, "r");
	free(path);
	if (f == NULL)
		return (0);
	while (!found && fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		next = line;
		for (n = 0; n < 4 && (cp = strsep(&next, "\t")) != NULL; n++)
			fields[n] = cp;
		if (n != 4 || strcmp(fields[0], name) != 0)
			continue;
		*provider = xstrdup(fields[1]);
		*location = xstrdup(fields[2]);
		*id = xstrdup(fields[3]);
		found = 1;
	}
	fclose(f);
	return (found);
}

/* Rewrite the record without a server, then add it again if id is given. */
static void
cmd_provision_record_update(const char *name, const char *provider,
    const char *location, const char *id)
{
	char	*path, *tmp, line[512];
	FILE	*f, *out;
	size_t	 len = strlen(name);

	path = cmd_provision_record_path();
	xasprintf(&tmp, "%s.tmp", path);
	if ((out = fopen(tmp, "w")) == NULL) {
		log_debug("%s: open %s failed", __func__, tmp);
		goto out;
	}
	if ((f = fopen(path, "r")) != NULL) {
		while (fgets(line, sizeof line, f) != NULL) {
			if (strncmp(line, name, len) == 0 && line[len] == '\t')
				continue;
			fputs(line, out);
		}
		fclose(f);
	}
	if (id != NULL)
		fprintf(out, "%s\t%s\t%s\t%s\n", name, provider, location, id);
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		log_debug("%s: write %s failed", __func__, path);
		unlink(tmp);
	}
out:
	free(tmp);
	free(path);
}

static void
cmd_provision_free(void *data)
{
	struct cmd_provision_data	*cdata = data;

	free(cdata->cmd);
	free(cdata->name);
	free(cdata->provider);
	free(cdata->location);
	free(cdata);
}

static void
cmd_provision_callback(struct job *job)
{
	struct cmd_provision_data	*cdata = job_get_data(job);
	struct bufferevent		*event = job_get_event(job);
	struct cmdq_item		*item = cdata->item;
	char				*line, *last = NULL;
	int				 status = job_get_status(job);

	while ((line = evbuffer_readline(event->input)) != NULL) {
		if (*line != '\0') {
			cmdq_print(item, "%s", line);
			free(last);
			last = line;
		} else
			free(line);
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		cmdq_error(item, "%s %s on %s failed",
		    cdata->delete ? "deleting" : "provisioning", cdata->name,
		    cdata->provider);
	} else if (cdata->delete) {
		cmd_provision_record_update(cdata->name, NULL, NULL, NULL);
		cmdq_print(item, "deleted %s on %s", cdata->name,
		    cdata->provider);
	} else {
		/* AWS prints the instance id first; the others use the name. */
		if (strcmp(cdata->provider, "aws") != 0 || last == NULL) {
			free(last);
			last = xstrdup(cdata->name);
		} else
			last[strcspn(last, " \t")] = '\0';
		cmd_provision_record_update(cdata->name, cdata->provider,
		    cdata->location, last);
		cmdq_print(item, "provisioned %s on %s (%s)", cdata->name,
		    cdata->provider, last);
	}
	free(last);
	cmdq_continue(item);
}

/* Run a provider command and wait for it. */
static enum cmd_retval
cmd_provision_run(struct cmdq_item *item, struct cmd_provision_data *cdata,
    const struct cmd_provision_provider *pp)
{
	char	*cmd;

	xasprintf(&cmd, "command -v %s >/dev/null 2>&1 || "
	    "{ echo '%s not found: install it and log in first'; exit 127; }; "
	    "%s 2>&1", pp->tool, pp->tool, cdata->cmd);
	free(cdata->cmd);
	cdata->cmd = cmd;
	cdata->item = item;

	log_debug("%s: %s", __func__, cmd);
	if (job_run(cmd, 0, NULL, NULL, NULL, "/", NULL,
	    cmd_provision_callback, cmd_provision_free, cdata, 0, -1,
	    -1) == NULL) {
		cmdq_error(item, "failed to run %s", pp->tool);
		cmd_provision_free(cdata);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_WAIT);
}

static enum cmd_retval
cmd_provision_server_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args				*args = cmd_get_args(self);
	const struct cmd_provision_provider	*pp;
	struct cmd_provision_data		*cdata;
	const char				*name = args_string(args, 0);
	const char				*location, *machine, *image, *key;
	const char				*provider;
	char					*p, *l, *id;

	provider = args_get(args, 'p');
	if (provider == NULL)
		provider = "gcp";
	if ((pp = cmd_provision_find_provider(provider)) == NULL) {
		cmdq_error(item, "unknown provider: %s (use gcp, aws or azure)",
		    provider);
		return (CMD_RETURN_ERROR);
	}
	if (!cmd_provision_check(name, 1)) {
		cmdq_error(item, "invalid server name: %s", name);
		return (CMD_RETURN_ERROR);
	}
	if (cmd_provision_record_find(name, &p, &l, &id)) {
		cmdq_error(item, "%s is already provisioned on %s", name, p);
		free(p);
		free(l);
		free(id);
		return (CMD_RETURN_ERROR);
	}

	if ((location = args_get(args, 'l')) == NULL)
		location = pp->location;
	if ((machine = args_get(args, 'm')) == NULL)
		machine = pp->machine;
	if ((image = args_get(args, 'i')) == NULL)
		image = pp->image;
	key = args_get(args, 'k');
	if (!cmd_provision_check(location, 0) ||
	    !cmd_provision_check(machine, 0) ||
	    !cmd_provision_check(image, 0) ||
	    (key != NULL && !cmd_provision_check(key, 0))) {
		cmdq_error(item, "invalid location, machine type, image or key "
		    "name");
		return (CMD_RETURN_ERROR);
	}

	cdata = xcalloc(1, sizeof *cdata);
	cdata->name = xstrdup(name);
	cdata->provider = xstrdup(pp->name);
	cdata->location = xstrdup(location);

	if (strcmp(pp->name, "gcp") == 0) {
		xasprintf(&cdata->cmd, "gcloud compute instances create '%s' "
		    "--zone='%s' --machine-type='%s' --image-family='%s' "
		    "--image-project=debian-cloud --labels=tmux=provisioned "
		    "--format='value(name,networkInterfaces[0].accessConfigs[0]"
		    ".natIP)'", name, location, machine, image);
	} else if (strcmp(pp->name, "aws") == 0) {
		xasprintf(&cdata->cmd, "aws ec2 run-instances --region '%s' "
		    "--image-id '%s' --instance-type '%s'%s%s%s "
		    "--tag-specifications 'ResourceType=instance,Tags=["
		    "{Key=Name,Value=%s},{Key=tmux,Value=provisioned}]' "
		    "--query 'Instances[0].InstanceId' --output text",
		    location, image, machine,
		    key != NULL ? " --key-name '" : "", key != NULL ? key : "",
		    key != NULL ? "'" : "", name);
	} else {
		xasprintf(&cdata->cmd, "az group create --name 'tmux-%s' "
		    "--location '%s' --tags tmux=provisioned --output none && "
		    "az vm create --resource-group 'tmux-%s' --name '%s' "
		    "--image '%s' --size '%s' --admin-username tmux "
		    "--generate-ssh-keys --query publicIpAddress --output tsv",
		    name, location, name, name, image, machine);
	}

	cmdq_print(item, "provisioning %s on %s (%s, %s)...", name, pp->name,
	    location, machine);
	return (cmd_provision_run(item, cdata, pp));
}

static enum cmd_retval
cmd_delete_provisioned_server_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args				*args = cmd_get_args(self);
	const struct cmd_provision_provider	*pp;
	struct cmd_provision_data		*cdata;
	const char				*name = args_string(args, 0);
	char					*provider, *location, *id;

	if (!cmd_provision_check(name, 1)) {
		cmdq_error(item, "invalid server name: %s", name);
		return (CMD_RETURN_ERROR);
	}

	/* Use the record, or -p and -l for a server that is not recorded. */
	if (!cmd_provision_record_find(name, &provider, &location, &id)) {
		if (!args_has(args, 'p')) {
			cmdq_error(item, "%s is not a provisioned server "
			    "(give -p to delete it anyway)", name);
			return (CMD_RETURN_ERROR);
		}
		provider = xstrdup(args_get(args, 'p'));
		location = NULL;
		id = xstrdup(name);
	}
	if ((pp = cmd_provision_find_provider(provider)) == NULL) {
		cmdq_error(item, "unknown provider: %s (use gcp, aws or azure)",
		    provider);
		goto fail;
	}
	if (args_has(args, 'l')) {
		free(location);
		location = xstrdup(args_get(args, 'l'));
	} else if (location == NULL)
		location = xstrdup(pp->location);
	if (!cmd_provision_check(location, 0) || !cmd_provision_check(id, 0)) {
		cmdq_error(item, "invalid location or id");
		goto fail;
	}

	cdata = xcalloc(1, sizeof *cdata);
	cdata->delete = 1;
	cdata->name = xstrdup(name);
	cdata->provider = xstrdup(pp->name);
	cdata->location = location;

	if (strcmp(pp->name, "gcp") == 0) {
		xasprintf(&cdata->cmd, "gcloud compute instances delete '%s' "
		    "--zone='%s' --quiet", id, location);
	} else if (strcmp(pp->name, "aws") == 0) {
		if (strncmp(id, "i-", 2) != 0) {
			/* Not recorded: find the instance by its Name tag. */
			xasprintf(&cdata->cmd, "id=$(aws ec2 describe-instances "
			    "--region '%s' --filters 'Name=tag:Name,Values=%s' "
			    "'Name=instance-state-name,Values=pending,running,"
			    "stopping,stopped' --query "
			    "'Reservations[].Instances[].InstanceId' "
			    "--output text) && [ -n \"$id\" ] && "
			    "aws ec2 terminate-instances --region '%s' "
			    "--instance-ids $id --output text", location, id,
			    location);
		} else {
			xasprintf(&cdata->cmd, "aws ec2 terminate-instances "
			    "--region '%s' --instance-ids '%s' --output text",
			    location, id);
		}
	} else {
		xasprintf(&cdata->cmd, "az group delete --name 'tmux-%s' --yes",
		    name);
	}
	free(provider);
	free(id);

	cmdq_print(item, "deleting %s on %s...", name, pp->name);
	return (cmd_provision_run(item, cdata, pp));

fail:
	free(provider);
	free(location);
	free(id);
	return (CMD_RETURN_ERROR);
}
