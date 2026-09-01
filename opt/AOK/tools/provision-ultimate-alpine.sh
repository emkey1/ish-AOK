#!/bin/sh
# provision-ultimate-alpine.sh
# ---------------------------------------------------------------------------
# Turn a fresh Alpine 3.23 rootfs (i686 OR x86_64) running under iSH-AOK into a
# full-featured, terminal-only Linux system:
#   * generous "ultimate terminal" CLI tool set
#   * services enabled on boot via OpenRC (sshd, syslog-ng, cronie, chronyd, ...)
#   * US/Pacific timezone (configurable)
#   * chrony in iSH-aware monitoring mode (the guest clock is the host clock)
#   * shell niceties: bash login shells, colour prompt, MOTD, login summary,
#     fzf/dircolors integration, machine-id, periodic-maintenance cron
#   * a dependency-free Neovim starter config (OSC52 clipboard on nvim >= 0.10)
#
# It is IDEMPOTENT: safe to run repeatedly. Run as root:
#       sudo sh provision-ultimate-alpine.sh
#   or  doas sh provision-ultimate-alpine.sh
#
# When run on a terminal it PROMPTS for the timezone and the primary login
# (creating that user if it does not exist). Pre-set any tunable via the
# environment to skip its prompt / run non-interactively:
#       TZ_NAME=America/Los_Angeles    # timezone (else prompted)
#       TARGET_USER=mke                # primary login to set up (else prompted)
#       NEW_HOSTNAME=                  # hostname to set (else prompted)
#       SUDO_NOPASSWD=0                # 1 = passwordless %wheel sudo
#
# Arch note: every package below is arch-independent in Alpine, so the same
# script provisions an x86_64 3.23.3 rootfs identically.
# ---------------------------------------------------------------------------
set -u

# ---- must be root --------------------------------------------------------
if [ "$(id -u)" != 0 ]; then
    echo "This script must run as root:  sudo sh $0" >&2
    exit 1
fi

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }

# ---- config (env overrides; prompts interactively when run on a TTY) ------
NEW_HOSTNAME="${NEW_HOSTNAME:-}"
SUDO_NOPASSWD="${SUDO_NOPASSWD:-0}"

# Defaults offered at the prompts.
DEF_TZ="${TZ_NAME:-America/Los_Angeles}"
DEF_USER="${TARGET_USER:-${SUDO_USER:-}}"
if [ -z "$DEF_USER" ] || [ "$DEF_USER" = root ]; then
    DEF_USER="$(awk -F: '$3>=1000 && $3<2000 {print $1; exit}' /etc/passwd)"
fi
[ -n "$DEF_USER" ] || DEF_USER="aok"
DEF_HOSTNAME="$(cat /etc/hostname 2>/dev/null)"
[ -n "$DEF_HOSTNAME" ] && [ "$DEF_HOSTNAME" != localhost ] || DEF_HOSTNAME="alpine-ish"

# ask <var> <prompt> <default>: keep an env-provided value; else prompt on a
# TTY; else use the default (so piped/ssh runs never block).
ask() {
    eval "_cur=\${$1:-}"
    [ -n "$_cur" ] && return
    if [ -t 0 ]; then
        printf '%s [%s]: ' "$2" "$3"
        read _a || _a=""
        [ -n "$_a" ] || _a="$3"
    else
        _a="$3"
    fi
    eval "$1=\$_a"
}
ask TZ_NAME     "Timezone (e.g. America/New_York, UTC)" "$DEF_TZ"
ask TARGET_USER "Primary login username to set up"      "$DEF_USER"
ask NEW_HOSTNAME "Hostname"                              "$DEF_HOSTNAME"

# Create the chosen login if it does not exist yet (fresh rootfs).
if [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ] && ! id "$TARGET_USER" >/dev/null 2>&1; then
    adduser -D -s /bin/bash "$TARGET_USER" 2>/dev/null || adduser -D "$TARGET_USER" 2>/dev/null
    note "created login '$TARGET_USER' (no password set; give it one with: passwd $TARGET_USER)"
fi
TARGET_HOME=""
if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    TARGET_HOME="$(awk -F: -v u="$TARGET_USER" '$1==u{print $6}' /etc/passwd)"
fi
note "timezone=$TZ_NAME  login=${TARGET_USER:-<none>}  hostname=${NEW_HOSTNAME:-<keep>}"

# ===========================================================================
log "Installing packages (this is the slow part under emulation)"
# ===========================================================================
PKGS="
  bash bash-completion cmake
  coreutils findutils grep sed gawk diffutils util-linux-misc
  procps-ng shadow file less
  openrc openssh openssh-server sudo
  syslog-ng syslog-ng-openrc
  chrony cronie cronie-openrc logrotate
  tzdata ca-certificates openssl
  man-db man-pages
  curl wget rsync bind-tools iproute2
  git strace build-base gdb linux-headers
  python3 py3-pip
  vim neovim nano tmux
  htop btop ncdu lsof pv tree
  mc fzf ripgrep fd bat eza jq yq most
  w3m lynx nmap socat netcat-openbsd mtr
  tar unzip zip p7zip bzip2 gzip zstd xz
  fastfetch figlet ncurses lazygit
"
apk update >/dev/null 2>&1 || note "apk update failed (continuing with cached index)"
if apk add --no-progress $PKGS; then
    note "packages installed"
else
    note "WARNING: 'apk add' reported errors; review above and re-run if needed"
fi

# ===========================================================================
log "Timezone -> $TZ_NAME"
# ===========================================================================
if [ -f "/usr/share/zoneinfo/$TZ_NAME" ]; then
    ln -sf "/usr/share/zoneinfo/$TZ_NAME" /etc/localtime
    echo "$TZ_NAME" > /etc/timezone
    note "$(date)"
else
    note "zoneinfo for '$TZ_NAME' not found; leaving clock as-is"
fi

# ===========================================================================
log "Locale -> C.UTF-8"
# ===========================================================================
# Alpine's musl libc treats any requested locale as UTF-8-aware except plain
# "C"/"POSIX", so C.UTF-8 works with no locale-gen step. Minirootfs images
# don't set a default, so tools expecting UTF-8 (less, python3, git) silently
# fall back to byte-oriented C/POSIX behavior. Set it system-wide via
# /etc/environment (read by busybox login for both interactive and
# non-interactive sessions) so it's not limited to shells that source
# /etc/profile.d.
# LANG only, never LC_ALL: LC_ALL outranks every per-category variable, so
# seeding it would stop anyone setting, say, LC_TIME=en_GB.UTF-8 afterwards.
if ! grep -q '^LANG=' /etc/environment 2>/dev/null; then
    printf 'LANG=C.UTF-8\n' >> /etc/environment
fi
note "LANG=C.UTF-8 (via /etc/environment)"

# ===========================================================================
log "machine-id"
# ===========================================================================
if [ ! -s /etc/machine-id ]; then
    { openssl rand -hex 16 2>/dev/null || head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n'; } > /etc/machine-id
    chmod 0444 /etc/machine-id
fi
note "$(cat /etc/machine-id)"

# ===========================================================================
log "Hostname"
# ===========================================================================
if [ -n "$NEW_HOSTNAME" ]; then
    echo "$NEW_HOSTNAME" > /etc/hostname
elif [ ! -s /etc/hostname ] || [ "$(cat /etc/hostname 2>/dev/null)" = localhost ]; then
    echo "alpine-ish" > /etc/hostname
fi
hostname "$(cat /etc/hostname)" 2>/dev/null || true
note "$(cat /etc/hostname)"

# ===========================================================================
log "sudo for the wheel group"
# ===========================================================================
grep -q '^@includedir /etc/sudoers.d' /etc/sudoers 2>/dev/null || \
    echo '@includedir /etc/sudoers.d' >> /etc/sudoers
if [ "$SUDO_NOPASSWD" = 1 ]; then
    echo '%wheel ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/wheel
    note "passwordless sudo for %wheel"
else
    echo '%wheel ALL=(ALL) ALL' > /etc/sudoers.d/wheel
    note "%wheel sudo (password required; set SUDO_NOPASSWD=1 for passwordless)"
fi
chmod 0440 /etc/sudoers.d/wheel
if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx wheel || adduser "$TARGET_USER" wheel 2>/dev/null
    note "$TARGET_USER is in: $(id -nG "$TARGET_USER")"
fi

# ===========================================================================
log "Login shells -> bash"
# ===========================================================================
grep -qx /bin/bash /etc/shells 2>/dev/null || echo /bin/bash >> /etc/shells
for u in root $TARGET_USER; do
    id "$u" >/dev/null 2>&1 || continue
    chsh -s /bin/bash "$u" >/dev/null 2>&1 || usermod -s /bin/bash "$u" 2>/dev/null || true
done
note "root + ${TARGET_USER:-} now use bash"

# ===========================================================================
log "MOTD"
# ===========================================================================
cat > /etc/motd <<'MOTD'

   Alpine Linux 3.23  .  iSH-AOK  (terminal-only userspace)
   ------------------------------------------------------------
   services :  rc-service <name> {start|stop|status}
   on boot  :  rc-update {add|del} <name> <runlevel>
   status   :  rc-status            logs : /var/log/messages
   time     :  chronyc tracking     docs : man <command>
   ------------------------------------------------------------

MOTD

# ===========================================================================
log "Shell niceties (/etc/profile.d)"
# ===========================================================================
cat > /etc/profile.d/30-aok-niceties.sh <<'NICETIES'
# AOK "full Linux feel" interactive niceties.  Safe for ash & bash.
export EDITOR=vim VISUAL=vim PAGER=less
export LESS='-R -M -i'
export TERM="${TERM:-xterm-256color}"
export LC_ALL="${LC_ALL:-C.UTF-8}" LANG="${LANG:-C.UTF-8}"

case $- in *i*) ;; *) return 2>/dev/null || exit 0;; esac

alias ls='ls --color=auto'
alias ll='ls -alF --color=auto'
alias la='ls -A --color=auto'
alias l='ls -CF --color=auto'
alias grep='grep --color=auto'
alias df='df -h'
alias free='free -m'
alias ..='cd ..'
alias ...='cd ../..'

if [ -n "${BASH:-}" ]; then
  if [ "$(id -u)" = 0 ]; then
    PS1='\[\e[1;31m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]# '
  else
    PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
  fi
  HISTSIZE=5000; HISTFILESIZE=10000; HISTCONTROL=ignoreboth
  shopt -s histappend checkwinsize 2>/dev/null
  [ -f /usr/share/bash-completion/bash_completion ] && . /usr/share/bash-completion/bash_completion
fi

if [ -z "${_AOK_SUMMARY_DONE:-}" ]; then
  export _AOK_SUMMARY_DONE=1
  printf '\n  \033[1;36m%s\033[0m  .  kernel \033[1m%s\033[0m  .  %s\n' \
    "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-Alpine Linux}")" \
    "$(uname -r)" "$(uname -m)"
  printf '  uptime:%s\n' "$(uptime 2>/dev/null | sed 's/^[[:space:]]*//;s/^/ /')"
  printf '  disk /: %s   mem: %s\n\n' \
    "$(command df -h / 2>/dev/null | awk 'NR==2{print $3" / "$2" ("$5")"}')" \
    "$(command free -m 2>/dev/null | awk '/^Mem:/{print $3"M / "$2"M"}')"
fi
NICETIES
chmod 0644 /etc/profile.d/30-aok-niceties.sh

cat > /etc/profile.d/40-aok-tools.sh <<'TOOLS'
# Interactive niceties for the installed CLI tool set. Safe for ash & bash.
case $- in *i*) ;; *) return 2>/dev/null || exit 0;; esac

command -v dircolors >/dev/null 2>&1 && eval "$(dircolors -b 2>/dev/null)"
alias ip='ip -color=auto'
if command -v bat >/dev/null 2>&1; then export BAT_THEME=ansi; fi

if [ -n "${BASH:-}" ]; then
  for f in /usr/share/fzf/key-bindings.bash /usr/share/fzf/completion.bash; do
    [ -f "$f" ] && . "$f"
  done
  export FZF_DEFAULT_OPTS="--height 40% --layout=reverse --border"
  command -v fd >/dev/null 2>&1 && export FZF_DEFAULT_COMMAND='fd --type f'
fi
TOOLS
chmod 0644 /etc/profile.d/40-aok-tools.sh
note "wrote /etc/profile.d/30-aok-niceties.sh and 40-aok-tools.sh"

# ===========================================================================
log "chrony (iSH-aware: monitor only, host owns the clock)"
# ===========================================================================
# In iSH the guest system clock IS the host (iOS) clock, already NTP-synced by
# iOS. chronyd therefore runs with -x (no clock control) and we keep the
# default localhost UDP command port: the unix command socket does NOT work
# under iSH (chronyc -> "506 Cannot talk to daemon"), so we must NOT set
# 'cmdport 0'. Dropping 'need net' avoids the unusable ifupdown service.
if [ -d /etc/chrony ]; then
    cat > /etc/chrony/chrony.conf <<'CHRONYCONF'
# chrony.conf -- tuned for iSH-AOK (monitoring mode; see /etc/conf.d/chronyd -x)
pool pool.ntp.org iburst
server time.cloudflare.com iburst
server time.google.com iburst
driftfile /var/lib/chrony/chrony.drift
logdir /var/log/chrony
# NB: no 'rtcsync' / 'initstepslew' (no clock control), and intentionally no
# 'cmdport 0' so chronyc can reach chronyd on 127.0.0.1:323 (the unix command
# socket is non-functional under iSH).
CHRONYCONF
    [ -d /var/log/chrony ] && chown chrony:chrony /var/log/chrony 2>/dev/null || true

    if grep -q '^ARGS=' /etc/conf.d/chronyd 2>/dev/null; then
        sed -i 's/^ARGS=.*/ARGS="-x"/' /etc/conf.d/chronyd
    else
        echo 'ARGS="-x"' >> /etc/conf.d/chronyd
    fi
    grep -q '^rc_need=' /etc/conf.d/chronyd 2>/dev/null || \
        printf '\n# iSH-AOK: network is always host-provided; do not require the\n# (non-functional) ifupdown "networking" service.\nrc_need="!net"\n' >> /etc/conf.d/chronyd
    note "chronyd: ARGS=-x, rc_need=!net, localhost command port"
fi

# ===========================================================================
log "Periodic maintenance cron (run-parts /etc/periodic/*)"
# ===========================================================================
mkdir -p /etc/periodic/15min /etc/periodic/hourly /etc/periodic/daily \
         /etc/periodic/weekly /etc/periodic/monthly /var/spool/cron/crontabs
if [ ! -s /var/spool/cron/crontabs/root ] || \
   ! grep -q 'run-parts /etc/periodic' /var/spool/cron/crontabs/root 2>/dev/null; then
    cat > /var/spool/cron/crontabs/root <<'CRONTAB'
# Alpine periodic maintenance (logrotate etc. live under /etc/periodic/*)
*/15  *  *  *  *  run-parts /etc/periodic/15min
0     *  *  *  *  run-parts /etc/periodic/hourly
0     2  *  *  *  run-parts /etc/periodic/daily
0     3  *  *  6  run-parts /etc/periodic/weekly
0     5  1  *  *  run-parts /etc/periodic/monthly
CRONTAB
    chmod 0600 /var/spool/cron/crontabs/root
    chown root:root /var/spool/cron/crontabs/root
    note "installed root maintenance crontab"
else
    note "root crontab already has periodic entries; left as-is"
fi

# ===========================================================================
log "Neovim starter config"
# ===========================================================================
NVIM_MARKER="-- AOK starter config (provision-ultimate-alpine.sh)"
write_nvim() {  # <homedir> <owner>
    _hd="$1"; _own="$2"
    [ -n "$_hd" ] || return 0
    _cfg="$_hd/.config/nvim/init.lua"
    # -e: the marker starts with "--", which grep would otherwise read as options.
    if [ -f "$_cfg" ] && ! grep -qF -e "$NVIM_MARKER" "$_cfg" 2>/dev/null; then
        note "nvim: keeping your existing $_cfg"
        return 0
    fi
    mkdir -p "$_hd/.config/nvim"
    cat > "$_cfg" <<'NVIMCFG'
-- AOK starter config (provision-ultimate-alpine.sh)
-- Dependency-free Neovim starter. Edit freely; the provisioner only overwrites
-- this file while the marker line above is present (delete it to keep yours).

vim.g.mapleader = " "
vim.g.maplocalleader = " "

local o = vim.opt
o.number = true
o.relativenumber = true
o.mouse = "a"
o.ignorecase = true
o.smartcase = true
o.incsearch = true
o.hlsearch = true
o.expandtab = true
o.shiftwidth = 4
o.tabstop = 4
o.softtabstop = 4
o.smartindent = true
o.breakindent = true
o.wrap = false
o.scrolloff = 5
o.sidescrolloff = 8
o.termguicolors = true
o.signcolumn = "yes"
o.cursorline = true
o.splitright = true
o.splitbelow = true
o.undofile = true
o.swapfile = false
o.updatetime = 300
o.timeoutlen = 500
o.completeopt = "menuone,noselect"
o.list = true
o.listchars = { tab = "» ", trail = "·", nbsp = "␣" }
o.title = true

pcall(vim.cmd.colorscheme, "habamax")

-- netrw as a light built-in file explorer
vim.g.netrw_banner = 0
vim.g.netrw_liststyle = 3

-- yank to the host clipboard over the terminal (OSC52) on Neovim >= 0.10
if vim.fn.has("nvim-0.10") == 1 then
  local ok, osc52 = pcall(require, "vim.ui.clipboard.osc52")
  if ok then
    vim.g.clipboard = {
      name = "OSC52",
      copy = { ["+"] = osc52.copy("+"), ["*"] = osc52.copy("*") },
      paste = { ["+"] = osc52.paste("+"), ["*"] = osc52.paste("*") },
    }
  end
end

local map = vim.keymap.set
map("n", "<leader>w", "<cmd>write<cr>", { desc = "Save" })
map("n", "<leader>q", "<cmd>quit<cr>", { desc = "Quit" })
map("n", "<leader>Q", "<cmd>quitall!<cr>", { desc = "Quit all (force)" })
map("n", "<leader>e", "<cmd>Explore<cr>", { desc = "File explorer" })
map("n", "<esc>", "<cmd>nohlsearch<cr>", { silent = true })
map("n", "<C-h>", "<C-w>h"); map("n", "<C-j>", "<C-w>j")
map("n", "<C-k>", "<C-w>k"); map("n", "<C-l>", "<C-w>l")
map("v", "J", ":m '>+1<cr>gv=gv", { silent = true })
map("v", "K", ":m '<-2<cr>gv=gv", { silent = true })
map("x", "<leader>p", [["_dP]], { desc = "Paste without losing register" })
map({ "n", "v" }, "<leader>y", [["+y]], { desc = "Yank to host clipboard" })

local aug = vim.api.nvim_create_augroup("aok", { clear = true })
vim.api.nvim_create_autocmd("TextYankPost", {
  group = aug,
  callback = function() vim.highlight.on_yank({ timeout = 200 }) end,
})
vim.api.nvim_create_autocmd("BufReadPost", {
  group = aug,
  callback = function()
    local m = vim.api.nvim_buf_get_mark(0, '"')
    if m[1] > 0 and m[1] <= vim.api.nvim_buf_line_count(0) then
      pcall(vim.api.nvim_win_set_cursor, 0, m)
    end
  end,
})

-- Optional plugin manager (lazy.nvim) -- uncomment to enable (needs network):
-- local lazypath = vim.fn.stdpath("data") .. "/lazy/lazy.nvim"
-- if not (vim.uv or vim.loop).fs_stat(lazypath) then
--   vim.fn.system({ "git", "clone", "--filter=blob:none",
--     "https://github.com/folke/lazy.nvim.git", "--branch=stable", lazypath })
-- end
-- vim.opt.rtp:prepend(lazypath)
-- require("lazy").setup({ --[[ plugin specs here ]] })
NVIMCFG
    chown -R "$_own" "$_hd/.config" 2>/dev/null || true
    note "nvim: wrote $_cfg"
}
write_nvim /root root
[ -n "$TARGET_HOME" ] && write_nvim "$TARGET_HOME" "$TARGET_USER"

# ===========================================================================
log "tmux config"
# ===========================================================================
TMUX_MARKER="# AOK tmux.conf (provision-ultimate-alpine.sh)"
write_tmux() {  # <homedir> <owner>
    _hd="$1"; _own="$2"
    [ -n "$_hd" ] || return 0
    _cfg="$_hd/.tmux.conf"
    if [ -f "$_cfg" ] && ! grep -qF -e "$TMUX_MARKER" "$_cfg" 2>/dev/null; then
        note "tmux: keeping your existing $_cfg"
        return 0
    fi
    cat > "$_cfg" <<'TMUXCONF'
# AOK tmux.conf (provision-ultimate-alpine.sh)
# This file was "stolen" from
# https://www.hamvocke.com/blog/a-guide-to-customizing-your-tmux-conf/
#
# Enable mouse mode (tmux 2.1 and above)
set -g mouse on

setw -g mode-keys vi
bind-key -T copy-mode-vi MouseDragEnd1Pane send-keys -X copy-pipe-and-cancel "pbcopy"

# Map escape to caps lock
#set-option -g prefix Escape
#unbind-key C-b
#bind-key Escape send-prefix

# reload config file (change file location to your the tmux.conf you want to use)
bind r source-file ~/.tmux.conf

# split panes using | and -
bind | split-window -h
bind - split-window -v
unbind '"'
unbind %

#
# if defined use custom handling for navigation keys,
# set by /usr/local/bin/nav_keys.shell
#
run-shell "[ -f /etc/opt/AOK/tmux_nav_key_handling ] && tmux source /etc/opt/AOK/tmux_nav_key_handling"

# # switch panes using Alt-arrow without prefix
# bind -n M-Left select-pane -L
# bind -n M-Right select-pane -R
# bind -n M-Up select-pane -U
# bind -n M-Down select-pane -D

# don't rename windows automatically
set-option -g allow-rename off

######################
### DESIGN CHANGES ###
######################

# loud or quiet?
set -g visual-activity off
set -g visual-bell off
set -g visual-silence off
setw -g monitor-activity off
set -g bell-action none

#  modes
setw -g clock-mode-colour colour5
setw -g mode-style 'fg=colour1 bg=colour18 bold'

# panes
set -g pane-border-style 'fg=colour19 bg=colour0'
set -g pane-active-border-style 'bg=colour0 fg=colour9'

# statusbar
set -g status-position bottom
set -g status-justify left
set -g status-style 'bg=colour18 fg=colour137 dim'
set -g status-left ''
set -g status-right '#[fg=colour233,bg=colour19] %d/%m #[fg=colour233,bg=colour8] %H:%M:%S '
set -g status-right-length 50
set -g status-left-length 20

setw -g window-status-current-style 'fg=colour1 bg=colour19 bold'
setw -g window-status-current-format ' #I#[fg=colour249]:#[fg=colour255]#W#[fg=colour249]#F '

setw -g window-status-style 'fg=colour9 bg=colour18'
setw -g window-status-format ' #I#[fg=colour237]:#[fg=colour250]#W#[fg=colour244]#F '

setw -g window-status-bell-style 'fg=colour255 bg=colour1 bold'

# messages
set -g message-style 'fg=colour232 bg=colour16 bold'
TMUXCONF
    chown "$_own" "$_cfg" 2>/dev/null || true
    note "tmux: wrote $_cfg"
}
write_tmux /root root
[ -n "$TARGET_HOME" ] && write_tmux "$TARGET_HOME" "$TARGET_USER"

# ===========================================================================
log "Enable + start services"
# ===========================================================================
# Bring a service up. For everything EXCEPT sshd we (re)start so config changes
# apply; sshd is only ever 'start'ed so we never drop the provisioning session.
apply_svc() {  # <name> <runlevel>
    rc-update add "$1" "$2" >/dev/null 2>&1
    if [ "$1" = sshd ]; then
        rc-service "$1" start >/dev/null 2>&1
    elif rc-service "$1" status >/dev/null 2>&1; then
        rc-service "$1" restart >/dev/null 2>&1
    else
        rc-service "$1" start >/dev/null 2>&1
    fi
}
# boot runlevel: early setup + logging
for s in bootmisc hostname syslog-ng seedrng; do apply_svc "$s" boot; done
# default runlevel: user-facing daemons
for s in sshd cronie chronyd local; do apply_svc "$s" default; done
note "enabled at boot:"
rc-update show 2>/dev/null | grep -vE '^[[:space:]]*$' | sed 's/^/      /'

# ===========================================================================
log "Done"
# ===========================================================================
printf '    %s\n' "$(date)"
note "Running services:"
rc-status -a 2>/dev/null | grep -E '\[  started' | awk '{print $1}' | sort | tr '\n' ' ' | sed 's/^/      /'; echo
cat <<EOF

    Next:
      * Re-login (or relaunch the app) to pick up bash + the new prompt/MOTD.
      * 'chronyc tracking' / 'chronyc sources' to see NTP status.
      * 'rc-status' for service health;  logs in /var/log/messages.
EOF
