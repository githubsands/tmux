# custom tmux for agent sessions

A tmux fork where a session can run a team of CLI agents (claude, codex or
openai). Each agent is a window running one agent process. The first window
is `operator`, a shell for you; the second is `control-agent`, the head agent,
which gets the output of every other agent as one multiplexed stream.

![agent system](docs/agent-system.png)

Feed lines look like:

    [2:agent-1] wrote parser.c
    [3:agent-2] tests passing

Every agent session lives in one tmux server, reached through its own socket
(`agents`, or the name in `$TMUX_AGENTS`), so it never talks to a stock tmux
server. `tmux choose-server` lists this server alongside any others.

## Build

    sh autogen.sh && ./configure && make
    ln -s "$PWD/tmux" ~/.local/bin/tmux    # put ~/.local/bin first in PATH

If `tmux -V` still shows the stock version in an old shell, run `rehash`.

## Commands

    tmux new-agent-session -s research          # operator + control-agent
    tmux new-agent-session -s research -N 3     # ... + agent-1..agent-3
    tmux new-agent-session -s research -a codex # use codex instead of claude
    tmux new-agent -t research                  # add an agent to a session
    tmux attach -t research

Aliases: `newas` and `newa`.

## Servers

    tmux choose-server                          # or prefix + S

Shows every tmux server with a socket in the socket directory (this fork's
`agents`, a stock `default` and any `-L` servers), with their sessions and
windows underneath. Agent sessions list their agents with their status
(`running`, `idle` or `exited`). The preview shows the selected window, even
on another server.

Enter on a session or window of another server replaces the client with one
attached to that server, using the binary that runs it, so a stock tmux
server works too. If that fails the client comes back to the current server.

## Cloud servers

    tmux provision-server -p gcp web-1                     # GCP, us-central1-a, e2-small
    tmux provision-server -p aws -l eu-west-1 -k key api-1 # AWS, t3.small
    tmux provision-server -p azure -m Standard_B2s db-1    # Azure, eastus
    tmux delete-provisioned-server api-1

Creates or deletes a VM with the provider's CLI (`gcloud`, `aws` or `az`),
which must be installed and logged in. `-l` is the zone or region, `-m` the
machine type, `-i` the image and `-k` the AWS key pair. Defaults:

| provider | location        | machine        | image             |
|----------|-----------------|----------------|-------------------|
| `gcp`    | `us-central1-a` | `e2-small`     | Debian 12         |
| `aws`    | `us-east-1`     | `t3.small`     | Amazon Linux 2023 |
| `azure`  | `eastus`        | `Standard_B1s` | Debian 12         |

Each server is recorded (name, provider, location, id) in `provisioned` in
the socket directory, so `delete-provisioned-server` only needs the name. On
Azure each server gets its own resource group, `tmux-<name>`, which is deleted
with it. Servers made elsewhere can be deleted with `-p` and `-l`.

## Options

| option               | values                         | default  |
|----------------------|--------------------------------|----------|
| `agent`              | `claude`, `codex`, `openai`    | `claude` |
| `agent-count`        | number of agents besides head  | `0`      |
| `agent-command`      | command for the agents         | empty    |
| `head-agent-command` | command for the head agent     | empty    |
| `agent-feed`         | `off`, `file`, `input`, `both` | `file`   |
| `agent-restore`      | `on`, `off`                    | `on`     |

    tmux set -g agent codex
    tmux set -t research agent-command 'claude --model opus'
    tmux set -t research agent-feed both

With `agent-feed file`, agent output is written as `[index:name] line` to the
file named by `$TMUX_AGENT_FEED`. With `input`, it is typed into the head
agent.

## Agent context

Every claude agent (including `control-agent`) gets its own conversation,
started with `claude --session-id <uuid>`. The session records each agent's
conversation in the file named by `$TMUX_AGENT_RECORD`
(`SOCKET-SESSION.agents`, next to the socket), which is kept after the
session ends:

    # session research: index name role conversation restores cwd
    1	control-agent	control	98357576-ac72-441a-a926-ffdc30f26924	0	/src/app
    2	agent-1	agent	1392b7be-167f-4872-9e55-ac525201dc94	1	/src/app

If a claude agent exits or crashes, it is restarted in the same window with
`claude --resume <uuid>`, so it picks up its saved context. An agent that
exits five times within 30 seconds is left closed, and killing an agent's
window or pane never restarts it. Turn it off with
`tmux set -t research agent-restore off`.

To bring back an agent by hand, even after the server is gone:

    cd /src/app && claude --resume 1392b7be-167f-4872-9e55-ac525201dc94
