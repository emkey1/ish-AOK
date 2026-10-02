# LLM Chat: a built-in AI assistant that can work in your guest

iSH-AOK includes an in-app LLM chat client that can talk to an
OpenAI-compatible API, Anthropic's Claude, Google Gemini, or (on iOS/iPadOS
26+) Apple's on-device Foundation Models — and, if you let it, read and edit
files and run shell commands in your guest for you, the way a coding agent
does.

## Enabling and opening it

It's off by default. Turn it on in Settings; once enabled, "LLM Chat"
appears both in the terminal's "Switch Terminal" menu and in
[Workspace](workspace.md), under **Utilities… → Workspace**.

## Chats

The client keeps as many separate conversations as you want. The first
toolbar button is named after the chat you're in and opens a menu with:

- **New Chat** and **All Chats…** (the full list, where you can rename,
  delete, or switch)
- your five most recent chats, for one-tap switching
- **Rename Chat…**, **System Prompt…**, **Working Directory…**,
  **Summarize Chat**, **Changes…**, **Clear Messages**, **Delete Chat**

Each chat keeps its own history, its own destination, its own working
directory and its own optional system prompt (standing instructions sent with
every message in that chat only). An unnamed chat titles itself from your first
message.

**Every chat is an agent that keeps working on its own.** A reply carries on
when you switch to another chat or close the window, and several chats can work
at once, each on its own destination. A command that needs your approval waits
in its chat until you open it; nothing pops up over the chat you are reading.
The status panel under the transcript shows what the chat is doing — the phase
and the command it is running, elapsed time, the tool round, the model, how much
of the context window is used, its task list, file changes and sub-agents — and
the chat list and **/agents** say which chats need approval, are working, or
have an answer you have not read. A message you send while a reply is running
is queued; **Stop** shows only while the message field is empty.

## Destinations

A destination is one saved endpoint: provider, server URL, model and API
key. The second toolbar button is named after the destination you're
talking to, as "name · model", and is **the one place to choose and edit
them**: tap another saved destination to switch in one tap; **Edit "name"…**
opens the destination's editor, with **Choose Model…** (the server's own list)
and **Test Connection**, both run against that destination; **Add
Destination…** starts a new one from a provider preset.

**Settings → LLM Client → Models** reaches the same list from the app's
Settings: select, edit, duplicate (handy for the same server with two models),
or delete. Opened from inside a chat, LLM Settings has no model rows at all.

**API keys are kept in the iOS Keychain**, on this device only, not in the app's
preferences. Every preference is readable from the guest as
`/proc/ish/defaults/<name>`, so anything you ran there — a package script, or
the chat's own model through a shell command — could have read a key kept that
way. Keys saved by an older build move to the Keychain the first time they are
read; `cat /proc/ish/defaults/llm_api_key` reads back empty.

## Providers

The **Provider** choice in a destination's editor covers the common cases, or
you can point it at any OpenAI-compatible endpoint yourself:

| Preset | Notes |
|---|---|
| Apple Foundation Models | On-device, no API key, no network required (iOS/iPadOS 26+) |
| OpenRouter Free | Hosted, needs an API key |
| Groq Llama | Hosted, needs an API key |
| Anthropic Claude | Anthropic's own Messages API (`https://api.anthropic.com/v1`), needs an API key; prompt caching on |
| Gemini Flash | Uses Google's `generateContent` REST API, key passed as a query param |
| LM Studio | Local server, defaults to `127.0.0.1:1234` |
| Ollama | Local server, defaults to `127.0.0.1:11434`; `http://localhost:11434/v1` is also the fallback if you blank out the Server URL field |
| OpenAI | Hosted, needs an API key |
| Custom | Any OpenAI-compatible chat completions endpoint |

Out of the box the client is configured for **OpenRouter Free**
(`https://openrouter.ai/api/v1`, model `openrouter/free`), which needs an API
key; add another destination to change that.

API keys are sent as an `Authorization: Bearer` header for OpenAI-style
providers, as `x-api-key` for Anthropic, or as a `?key=` query parameter for
Gemini.

Replies stream in via Server-Sent Events where the provider supports it,
rendered incrementally with Markdown/code-fence awareness as tokens arrive —
including the rounds of a reply that uses tools. **Stop** cancels the request
and keeps what had arrived, marked "(stopped)".

## Tools: letting the model work in your guest

Turn **Tools** on under **Settings → LLM Client** and the model gets tools that
act in your guest, as your guest account:

- `read_file`, `list_directory`, `glob` and `grep` to look around;
- `write_file` and `edit_file` to change files — refused for a file the model has
  not read in this chat, or one that changed since it did, so a stale picture is
  never written back over somebody else's edit;
- `run_shell` to run a command, starting in the chat's working directory;
- `todo_write`, a task list it keeps for multi-step work (shown in the
  transcript as "todo: 1 of 3 done");
- tools from any [MCP servers](#mcp-servers-more-tools) you add.

Apple Foundation Models gets `run_shell` alone; Gemini gets none.

**The working directory** is per chat (**Working Directory…**, your `$HOME` to
begin with). Relative paths resolve against it, `run_shell` starts in it, and an
edit outside it always asks. The nearest `AGENTS.md` at or above it — or
`CLAUDE.md` where there is none — is sent with every prompt as project
instructions, so editing it takes effect on your next message.

**Permissions.** Each kind of tool — **Read Files**, **Edit Files**, **Shell
Commands**, **MCP Tools** — is Allow, Ask or Deny (by default reading is
allowed and the rest ask), under **Settings → LLM Client → Tool Permissions**.
Shell commands also meet an ordered list of wildcard rules, first match wins:
a command line is split at `;`, `&`, `|`, `&&`, `||` and newlines, and every
piece must pass. A piece that redirects into a file or uses `$( )` or backquotes
is never allowed by a rule alone. The default rules allow only commands that
look (`ls`, `cat`, `git status` and the like). When the chat asks, you can
**Run** once, **Run, don't ask again this reply**, **Always allow** — which
adds a rule such as `git status *` for a command, or allows the category for
files and MCP tools — or **Run, allow all this chat**. The first time you choose
that last one, an "Allow every tool call this chat?" warning asks you to confirm
with **Allow All**; after it, every command and file change for the rest of that
chat happens without asking, until you clear the chat. Deny still applies.

Worth understanding before you use it: content the model fetches — a web page, a
file, an MCP server's answer — can instruct it to run destructive commands or
read private data, and whatever you have allowed, nothing stops that but the
model itself.

Command output is capped (64 KB by default) and a command is killed if it runs
too long (30 seconds by default); a reply also stops after a capped number of
tool rounds (20 by default). All three are under **Settings → LLM Client** as
Command Timeout, Output Limit and Tool Call Rounds.

**Every file change can be reviewed and reverted.** **Changes…** (or `/changes`)
lists what the model wrote, newest first — "edited · −1 +1 · 11:00" — and each
opens as a coloured unified diff with **Revert**. `/undo` reverts the newest one
not yet reverted. Reverting a file that changed again since asks before it
throws the later change away, and the chat tells the model what you reverted.
History holds 16 MB, oldest dropped first.

## Sub-agents

With tools on, a model can hand self-contained tasks to **sub-agents** that run
in parallel and report back — one level deep, at most four working per chat.
Their chats are listed with a ↳ in front, and their approvals come up in the
parent's chat.

## MCP servers: more tools

**Settings → LLM Client → MCP Servers** (or `/mcp`) adds tool servers that speak
the Model Context Protocol, of two kinds:

- **remote** servers over Streamable HTTP, with an optional bearer token that is
  kept in the Keychain;
- **stdio** servers started inside your guest as your account — a command line
  such as `npx -y some-mcp-server`, run where the chat's own commands run.

Each tool is offered to the model as `mcp__<server>__<tool>`, and every call
follows the **MCP Tools** permission (Ask by default). A server that fails to
connect is named in the chat and left alone for five minutes, unless you change
its settings or a **Check** succeeds.

## Long chats

A chat is **summarised** before it outgrows the model: at three quarters of its
context window, before a prompt and between tool rounds, the model is asked to
summarise the conversation so far, and from then on is sent only that summary and
what follows. The transcript on screen keeps everything. `/compact` or
**Summarize Chat** does it on demand. The window is **Settings → LLM Client →
Context Window**: Automatic takes the server's figure, and a server that reports
none is taken to have 64K.

## Commands

Type these in the message field:

| command | what it does |
|---|---|
| `/new`, `/chats` | start a chat; list them |
| `/agents` | every chat's state: needs approval, working, unread |
| `/compact` | summarise the chat now |
| `/changes`, `/undo` | review the model's file changes; revert the newest |
| `/mcp` | the MCP server list |
| `/models`, `/model NAME` | list the server's models; switch this destination to one |

## Persistence

Chats are saved under `/AOK/persist/llm-chats` — one file per chat, plus an
`index.json` naming them and recording which one is selected — so they
survive root switches and app restarts. A transcript from before multiple
chats existed is carried in as your first chat; the old
`/AOK/persist/llm-chat.json` is copied, not moved, so an older build still
finds it where it left it.

Saved extracts and prompt templates live under `/AOK/persist/llm-extracts`
and `/AOK/persist/llm-prompts` respectively — see [persist.md](persist.md).
