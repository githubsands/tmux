# custom tmux for agent sessions

A tmux fork where a session can run a team of CLI agents (claude, codex or
openai). Each agent is a window running one agent process. The first window
is the head agent; the output of every other agent is fed to it.

It uses its own socket (`agents`) and `$TMUX_AGENTS`, so it never talks to a
stock tmux server.

## Build

    sh autogen.sh && ./configure && make
    ln -s "$PWD/tmux" ~/.local/bin/tmux    # put ~/.local/bin first in PATH

If `tmux -V` still shows the stock version in an old shell, run `rehash`.

## Commands

    tmux new-agent-session -s research          # head agent only
    tmux new-agent-session -s research -N 3     # head + 3 agents
    tmux new-agent-session -s research -a codex # use codex instead of claude
    tmux new-agent -t research                  # add an agent to a session
    tmux attach -t research

Aliases: `newas` and `newa`.

## Options

| option               | values                         | default  |
|----------------------|--------------------------------|----------|
| `agent`              | `claude`, `codex`, `openai`    | `claude` |
| `agent-count`        | number of agents besides head  | `0`      |
| `agent-command`      | command for the agents         | empty    |
| `head-agent-command` | command for the head agent     | empty    |
| `agent-feed`         | `off`, `file`, `input`, `both` | `file`   |

    tmux set -g agent codex
    tmux set -t research agent-command 'claude --model opus'
    tmux set -t research agent-feed both

With `agent-feed file`, agent output is written as `[index:name] line` to the
file named by `$TMUX_AGENT_FEED`. With `input`, it is typed into the head
agent.
