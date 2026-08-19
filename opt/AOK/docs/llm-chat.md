# LLM Chat: a built-in AI assistant with guest shell access

iSH-AOK includes an in-app LLM chat client that can talk to an
OpenAI-compatible API, Google Gemini, or (on iOS/iPadOS 26+) Apple's
on-device Foundation Models — and, optionally, run shell commands in your
guest for you.

## Enabling and opening it

It's off by default. Turn it on in Settings; once enabled, "LLM Chat"
appears both in the terminal's "Switch Terminal" menu and in the
[Workspace](workspace.md) dock.

## Chats

The client keeps as many separate conversations as you want. The first
toolbar button is named after the chat you're in and opens a menu with:

- **New Chat** and **All Chats…** (the full list, where you can rename,
  delete, or switch)
- your five most recent chats, for one-tap switching
- **Rename Chat…**, **System Prompt…**, **Clear Messages**, **Delete Chat**

Each chat keeps its own history, its own destination, and its own optional
system prompt (standing instructions sent with every message in that chat
only). An unnamed chat titles itself from your first message.

Switching chats while a reply is still arriving asks first, and waits for
that reply to finish landing in the chat it belongs to before swapping.

## Destinations

A destination is one saved endpoint: provider, server URL, model and API
key. The second toolbar button is named after the destination you're
talking to and switches between saved destinations in one tap — you only
need Settings to add or edit one, and even adding can be done from that
menu via **Add Destination…**.

Manage the saved set under **Settings → Destinations**: select, edit,
duplicate (handy for the same server with two models), or delete. The
Settings rows below it — Provider, Server URL, Model, API Key — always
configure whichever destination is currently selected.

## Providers

A "Provider" preset picker covers the common cases, or you can point it at
any OpenAI-compatible endpoint yourself:

| Preset | Notes |
|---|---|
| Apple Foundation Models | On-device, no API key, no network required (iOS/iPadOS 26+) |
| OpenRouter Free | Hosted, needs an API key |
| Groq Llama | Hosted, needs an API key |
| Gemini Flash | Uses Google's `generateContent` REST API, key passed as a query param |
| LM Studio | Local server, defaults to `127.0.0.1:1234` |
| Ollama | Local server, defaults to `127.0.0.1:11434`; `http://localhost:11434/v1` is also the fallback if you blank out the Server URL field |
| OpenAI | Hosted, needs an API key |
| Custom | Any OpenAI-compatible chat completions endpoint |

Out of the box the client is configured for **OpenRouter Free**
(`https://openrouter.ai/api/v1`, model `openrouter/free`), which needs an API
key; switch the Provider preset to change that.

API keys are sent as an `Authorization: Bearer` header for OpenAI-style
providers, or as a `?key=` query parameter for Gemini.

Responses stream in via Server-Sent Events where the provider supports it,
rendered incrementally with Markdown/code-fence awareness as tokens
arrive.

## Shell Tools: letting the model run commands for you

For OpenAI-compatible providers and for Apple Foundation Models — but not for
Gemini — you can grant the assistant a `run_shell` tool. When it wants to run
something — for example, to fetch a web page via `curl` on your behalf —
**you're asked to confirm each command before it executes.** Command output is
capped (64 KB by default) and the command is killed if it runs too long (30
seconds by default); a reply also stops after a capped number of tool rounds (20
by default). All three are adjustable under **Settings → LLM Client** as Command
Timeout, Output Limit and Tool Call Rounds.

There IS a way to stop being asked. The confirmation sheet offers "Run, don't
ask again this reply", and a separate "Auto-run all commands this chat?" prompt
offers **Allow All**, after which every command for the rest of that chat runs
without confirmation. Auto-run re-arms when you clear the chat — but the
*warning* is shown only once ever: once you have acknowledged it, later chats
enter auto-run without showing it again.

Worth understanding before you use it: content the model fetches — a web page, a
file — can instruct it to run destructive commands or read private data, and in
auto-run nothing stops that but the model itself.

## Persistence

Chats are saved under `/AOK/persist/llm-chats` — one file per chat, plus an
`index.json` naming them and recording which one is selected — so they
survive root switches and app restarts. A transcript from before multiple
chats existed is carried in as your first chat; the old
`/AOK/persist/llm-chat.json` is copied, not moved, so an older build still
finds it where it left it.

Saved extracts and prompt templates live under `/AOK/persist/llm-extracts`
and `/AOK/persist/llm-prompts` respectively — see [persist.md](persist.md).
