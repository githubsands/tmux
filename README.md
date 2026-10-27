# custom tmux for agent sessions

A tmux fork where a session can run a team of CLI agents (claude, codex or
openai). Each agent is a window running one agent process. The first window
is `operator`, a shell for you; the second is `control-agent`, the head agent,
which gets the output of every other agent as one multiplexed stream.

```mermaid
flowchart TB
    subgraph SRV["tmux server - socket: agents ($TMUX_TMPDIR/tmux-UID/agents)"]
        direction TB
        subgraph S["session: research (new-agent-session -s research -N 3)"]
            direction TB
            OP["<b>0: operator</b><br/>your shell"]
            CA["<b>1: control-agent</b><br/>head agent - claude / codex / openai"]

            MUX{{"multiplexer<br/>strip escapes, tag lines [index:name]"}}
            FEED[("$TMUX_AGENT_FEED<br/>SOCKET-agents-ID.log")]

            A1["<b>2: agent-1</b><br/>running"]
            A2["<b>3: agent-2</b><br/>idle"]
            A3["<b>4: agent-3</b><br/>exited"]

            A1 & A2 & A3 -- output --> MUX
            MUX -- "agent-feed file" --> FEED
            FEED -. reads .-> CA
            MUX -- "agent-feed input" --> CA
        end
        MORE["other agent sessions on the same server"]
    end

    STOCK["stock tmux server - socket: default<br/>never contacted"]
    SRV ~~~ STOCK

    classDef srv fill:#f7f9fc,stroke:#4a5a78,color:#1c2740
    classDef op fill:#e8f1ff,stroke:#3b6fd8,color:#0b2a66
    classDef ctl fill:#f3e8ff,stroke:#8a3fd1,color:#3b0a66
    classDef run fill:#e6f7ec,stroke:#2f9e5a,color:#0d3d20
    classDef idle fill:#fff6e0,stroke:#d19a1f,color:#5a3d00
    classDef dead fill:#fde8e8,stroke:#c94040,color:#5c1010
    classDef mux fill:#f2f2f2,stroke:#666,color:#222
    classDef faded fill:#fafafa,stroke:#aaa,color:#777,stroke-dasharray:4 3
    class SRV srv
    class OP op
    class CA ctl
    class A1 run
    class A2 idle
    class A3 dead
    class MUX,FEED mux
    class MORE,STOCK faded
```

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
