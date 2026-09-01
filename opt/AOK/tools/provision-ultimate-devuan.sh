#!/bin/sh
# provision-ultimate-devuan.sh
# ---------------------------------------------------------------------------
# Turn a fresh Devuan 6 "Excalibur" minirootfs (i386 OR amd64) running under
# iSH-AOK into a full-featured, terminal-only Linux system. This is the Devuan
# twin of provision-ultimate-alpine.sh -- same end result, translated to
# apt/dpkg + sysvinit instead of apk + OpenRC:
#   * generous "ultimate terminal" CLI tool set
#   * services enabled on boot via sysvinit (sshd, rsyslog, cron, chrony, ...)
#   * US/Pacific timezone (configurable)
#   * chrony in iSH-aware monitoring mode (the guest clock is the host clock)
#   * shell niceties: bash login shells, colour prompt, MOTD, login summary,
#     fzf/dircolors integration, machine-id, periodic maintenance via cron
#   * a dependency-free Neovim starter config (OSC52 clipboard on nvim >= 0.10)
#
# It is IDEMPOTENT: safe to run repeatedly. Run as root:
#       sudo sh provision-ultimate-devuan.sh
#   or  doas sh provision-ultimate-devuan.sh
#
# When run on a terminal it PROMPTS for the timezone and the primary login
# (creating that user if it does not exist). Pre-set any tunable via the
# environment to skip its prompt / run non-interactively:
#       TZ_NAME=America/Los_Angeles    # timezone (else prompted)
#       TARGET_USER=mke                # primary login to set up (else prompted)
#       NEW_HOSTNAME=                  # hostname to set (else prompted)
#       SUDO_NOPASSWD=0                # 1 = passwordless sudo-group sudo
#
# Arch note: every package below is arch-independent in Devuan, so the same
# script provisions an i386 and an amd64 rootfs identically.
# ---------------------------------------------------------------------------
set -u
export DEBIAN_FRONTEND=noninteractive

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
[ -n "$DEF_HOSTNAME" ] && [ "$DEF_HOSTNAME" != localhost ] || DEF_HOSTNAME="devuan-ish"

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
    adduser --disabled-password --gecos "" --shell /bin/bash "$TARGET_USER" >/dev/null 2>&1 \
        || adduser --disabled-password --gecos "" "$TARGET_USER" >/dev/null 2>&1 \
        || useradd -m -s /bin/bash "$TARGET_USER" 2>/dev/null
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
# Debian/Devuan equivalents of the Alpine "ultimate terminal" set. A few Alpine
# names differ here: openrc -> sysvinit-core, procps-ng -> procps,
# build-base -> build-essential, cronie -> cron, syslog-ng -> rsyslog,
# bind-tools -> bind9-dnsutils, fd -> fd-find (fdfind), bat -> bat (batcat),
# p7zip -> p7zip-full, xz -> xz-utils, mtr -> mtr-tiny. (yq and lazygit are not
# in Devuan main and are simply skipped.)
PKGS="
  bash bash-completion cmake
  coreutils findutils grep sed gawk diffutils util-linux bsdextrautils
  procps passwd adduser file less
  sysvinit-core locales
  openssh-client openssh-server sudo
  rsyslog iputils-ping wtmpdb
  chrony cron logrotate dialog
  tzdata ca-certificates openssl
  man-db manpages libpam-systemd
  curl wget rsync bind9-dnsutils iproute2
  git strace build-essential gdb
  python3 python3-pip python3-venv
  vim neovim nano tmux sysstat
  htop btop ncdu lsof pv tree
  mc fzf ripgrep fd-find bat eza jq most
  w3m lynx nmap socat netcat-openbsd mtr-tiny
  tar unzip zip p7zip-full bzip2 gzip zstd xz-utils
  fastfetch figlet ncurses-bin ncurses-term
"

# Pre-seed the timezone so the tzdata postinst never tries to prompt.
ln -sf "/usr/share/zoneinfo/$TZ_NAME" /etc/localtime 2>/dev/null || true
printf '%s\n' "$TZ_NAME" > /etc/timezone

apt-get update >/dev/null 2>&1 || note "apt-get update failed (continuing with cached index)"
# apt aborts the whole transaction on a single unavailable package, so try the
# batch first and fall back to installing package-by-package (skipping any that
# are unavailable in this suite/arch).
if apt-get install -y --no-install-recommends $PKGS; then
    note "packages installed"
else
    note "batch install failed; retrying package-by-package (skipping unavailable)"
    for p in $PKGS; do
        apt-get install -y --no-install-recommends "$p" >/dev/null 2>&1 \
            || note "  skipped: $p (unavailable or failed)"
    done
fi
apt-get clean >/dev/null 2>&1 || true

# Debian ships these tools under disambiguated names; add the conventional
# command names in /usr/local/bin so muscle memory (and the fzf/profile glue
# below) works. Only created when the target exists and the name is free.
link_alt() {  # <real-binary> <wanted-name>
    if command -v "$1" >/dev/null 2>&1 && ! command -v "$2" >/dev/null 2>&1; then
        ln -sf "$(command -v "$1")" "/usr/local/bin/$2" && note "ln /usr/local/bin/$2 -> $1"
    fi
}
link_alt batcat bat
link_alt fdfind fd

# ===========================================================================
log "Timezone -> $TZ_NAME"
# ===========================================================================
if [ -f "/usr/share/zoneinfo/$TZ_NAME" ]; then
    ln -sf "/usr/share/zoneinfo/$TZ_NAME" /etc/localtime
    echo "$TZ_NAME" > /etc/timezone
    dpkg-reconfigure -f noninteractive tzdata >/dev/null 2>&1 || true
    note "$(date)"
else
    note "zoneinfo for '$TZ_NAME' not found; leaving clock as-is"
fi

# ===========================================================================
log "Locale -> C.UTF-8"
# ===========================================================================
# Devuan/Debian minirootfs images ship no locale set, so plain UTF-8-aware
# tools (less, python3, git, man-db) fall back to the C/POSIX locale and mangle
# non-ASCII. glibc on this base has C.UTF-8 built in (no locale-gen needed);
# the locale just has to be named.
#
# /etc/default/locale is where Debian keeps it, and it is enough: every PAM
# service that matters reads it by name --
#
#     /etc/pam.d/login:  session required pam_env.so readenv=1 envfile=/etc/default/locale
#     /etc/pam.d/su, /etc/pam.d/sshd, /etc/pam.d/cron:  the same
#
# This used to write the locale into /etc/environment as well, on the theory
# that pam_env reads that one with no configuration. It does -- but Debian
# treats locale variables there as deprecated and complains about each one at
# every boot, from cron's init script of all places:
#
#     /etc/environment has been deprecated for locale information;
#       use /etc/default/locale for LANG=C.UTF-8 instead ... (warning).
#
# (/etc/init.d/cron, and a stock Devuan install leaves /etc/environment empty
# for exactly this reason.) It also set LC_ALL, which outranks every
# per-category variable and would have stopped anyone ever setting, say,
# LC_TIME on their own machine. Neither belongs here.
printf 'LANG=C.UTF-8\n' > /etc/default/locale
update-locale LANG=C.UTF-8 2>/dev/null || true
# Both files above are PAM's, and ssh does not go through PAM here: Excalibur's
# stock sshd_config leaves UsePAM at the upstream default of "no" (`sshd -T`
# confirms it), so an ssh session gets neither, and lands in the C/POSIX locale
# with no UTF-8 charmap -- which is enough for btop to refuse to start at all
# ("ERROR: No UTF-8 locale detected!"). /etc/profile.d is what every shell
# login reads, PAM or not, so the locale is named there too. Guarded, so a
# locale forwarded by the ssh client (AcceptEnv LANG) or set by PAM wins.
mkdir -p /etc/profile.d
cat > /etc/profile.d/00-aok-locale.sh <<'LOCALESH'
# iSH-AOK: default to a UTF-8 locale when nothing else named one.
# Reaches sessions PAM never touches -- ssh (stock sshd has UsePAM no).
[ -n "${LANG:-}" ] || export LANG=C.UTF-8
LOCALESH
chmod 0644 /etc/profile.d/00-aok-locale.sh
note "LANG=C.UTF-8 (via /etc/default/locale, /etc/profile.d)"

# ===========================================================================
log "machine-id"
# ===========================================================================
if [ ! -s /etc/machine-id ]; then
    { openssl rand -hex 16 2>/dev/null || head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n'; } > /etc/machine-id
    chmod 0444 /etc/machine-id
fi
# Keep the legacy D-Bus machine-id in sync (some tools still read it).
if [ -d /var/lib/dbus ] && [ ! -e /var/lib/dbus/machine-id ]; then
    ln -sf /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true
fi
note "$(cat /etc/machine-id)"

# ===========================================================================
log "Hostname"
# ===========================================================================
if [ -n "$NEW_HOSTNAME" ]; then
    echo "$NEW_HOSTNAME" > /etc/hostname
elif [ ! -s /etc/hostname ] || [ "$(cat /etc/hostname 2>/dev/null)" = localhost ]; then
    echo "devuan-ish" > /etc/hostname
fi
hostname "$(cat /etc/hostname)" 2>/dev/null || true
# Make sure the hostname resolves (Debian expects a 127.0.1.1 line).
_hn="$(cat /etc/hostname 2>/dev/null)"
if [ -n "$_hn" ] && ! grep -qE "[[:space:]]$_hn(\$|[[:space:]])" /etc/hosts 2>/dev/null; then
    printf '127.0.1.1\t%s\n' "$_hn" >> /etc/hosts
fi
note "$(cat /etc/hostname)"

# ===========================================================================
log "sudo for the sudo group"
# ===========================================================================
# Devuan/Debian use the 'sudo' group (not Alpine's 'wheel'). The stock
# /etc/sudoers already has '#includedir /etc/sudoers.d'; we drop a file there.
getent group sudo >/dev/null 2>&1 || groupadd sudo 2>/dev/null || true
if [ "$SUDO_NOPASSWD" = 1 ]; then
    echo '%sudo ALL=(ALL:ALL) NOPASSWD: ALL' > /etc/sudoers.d/aok-sudo
    note "passwordless sudo for the sudo group"
else
    echo '%sudo ALL=(ALL:ALL) ALL' > /etc/sudoers.d/aok-sudo
    note "sudo-group sudo (password required; set SUDO_NOPASSWD=1 for passwordless)"
fi
chmod 0440 /etc/sudoers.d/aok-sudo
# Refuse to leave an invalid sudoers fragment in place.
if command -v visudo >/dev/null 2>&1 && ! visudo -cf /etc/sudoers.d/aok-sudo >/dev/null 2>&1; then
    rm -f /etc/sudoers.d/aok-sudo
    note "WARNING: generated sudoers fragment failed validation; removed it"
fi
if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx sudo || adduser "$TARGET_USER" sudo >/dev/null 2>&1
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

   Devuan GNU/Linux 6 (excalibur)  .  iSH-AOK  (terminal-only userspace)
   ------------------------------------------------------------
   services :  service <name> {start|stop|status}
   on boot  :  update-rc.d <name> {enable|disable}
   status   :  service --status-all     logs : /var/log/syslog
   time     :  chronyc tracking         docs : man <command>
   ------------------------------------------------------------

MOTD

# ===========================================================================
log "Shell niceties (/etc/profile.d)"
# ===========================================================================
cat > /etc/profile.d/30-aok-niceties.sh <<'NICETIES'
# AOK "full Linux feel" interactive niceties.  Safe for dash & bash.
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
    "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-Devuan GNU/Linux}")" \
    "$(uname -r)" "$(uname -m)"
  printf '  uptime:%s\n' "$(uptime 2>/dev/null | sed 's/^[[:space:]]*//;s/^/ /')"
  printf '  disk /: %s   mem: %s\n\n' \
    "$(command df -h / 2>/dev/null | awk 'NR==2{print $3" / "$2" ("$5")"}')" \
    "$(command free -m 2>/dev/null | awk '/^Mem:/{print $3"M / "$2"M"}')"
fi
NICETIES
chmod 0644 /etc/profile.d/30-aok-niceties.sh

cat > /etc/profile.d/40-aok-tools.sh <<'TOOLS'
# Interactive niceties for the installed CLI tool set. Safe for dash & bash.
case $- in *i*) ;; *) return 2>/dev/null || exit 0;; esac

command -v dircolors >/dev/null 2>&1 && eval "$(dircolors -b 2>/dev/null)"
alias ip='ip -color=auto'
# On Debian/Devuan these ship as batcat / fdfind; fall back if the
# provisioner's /usr/local/bin/{bat,fd} symlinks are absent.
command -v bat    >/dev/null 2>&1 || { command -v batcat >/dev/null 2>&1 && alias bat='batcat'; }
command -v fd     >/dev/null 2>&1 || { command -v fdfind >/dev/null 2>&1 && alias fd='fdfind'; }
if command -v bat >/dev/null 2>&1 || command -v batcat >/dev/null 2>&1; then export BAT_THEME=ansi; fi

if [ -n "${BASH:-}" ]; then
  # Debian ships fzf's shell glue under /usr/share/doc/fzf/examples.
  for f in /usr/share/doc/fzf/examples/key-bindings.bash \
           /usr/share/doc/fzf/examples/completion.bash \
           /usr/share/fzf/key-bindings.bash /usr/share/fzf/completion.bash; do
    [ -f "$f" ] && . "$f"
  done
  export FZF_DEFAULT_OPTS="--height 40% --layout=reverse --border"
  if command -v fdfind >/dev/null 2>&1; then
    export FZF_DEFAULT_COMMAND='fdfind --type f'
  elif command -v fd >/dev/null 2>&1; then
    export FZF_DEFAULT_COMMAND='fd --type f'
  fi
fi
TOOLS
chmod 0644 /etc/profile.d/40-aok-tools.sh
note "wrote /etc/profile.d/30-aok-niceties.sh and 40-aok-tools.sh"

# ===========================================================================
log "chrony (iSH-aware: monitor only, host owns the clock)"
# ===========================================================================
# In iSH the guest system clock IS the host (iOS) clock, already NTP-synced by
# iOS. chronyd therefore runs with -x (no clock control). Under iSH the unix
# command socket does not work, so reach chronyc over the localhost UDP port if
# needed:  chronyc -h 127.0.0.1 tracking
if [ -d /etc/chrony ]; then
    cat > /etc/chrony/chrony.conf <<'CHRONYCONF'
# chrony.conf -- tuned for iSH-AOK (monitoring mode; chronyd runs with -x)
pool pool.ntp.org iburst
server time.cloudflare.com iburst
server time.google.com iburst
driftfile /var/lib/chrony/chrony.drift
logdir /var/log/chrony
# NB: no 'rtcsync' / 'initstepslew' (no clock control under iSH). chronyd is
# started with -x via /etc/default/chrony so it only *monitors* the clock.
CHRONYCONF
    [ -d /var/log/chrony ] && chown _chrony:_chrony /var/log/chrony 2>/dev/null || true

    # Debian keeps the daemon flags in /etc/default/chrony (DAEMON_OPTS).
    if [ -f /etc/default/chrony ]; then
        if grep -q '^DAEMON_OPTS=' /etc/default/chrony 2>/dev/null; then
            sed -i 's/^DAEMON_OPTS=.*/DAEMON_OPTS="-x"/' /etc/default/chrony
        else
            echo 'DAEMON_OPTS="-x"' >> /etc/default/chrony
        fi
    else
        echo 'DAEMON_OPTS="-x"' > /etc/default/chrony
    fi
    note "chronyd: DAEMON_OPTS=-x (monitor only), localhost command port"
fi

# ===========================================================================
log "Periodic maintenance (run-parts /etc/cron.*)"
# ===========================================================================
# Debian's stock /etc/crontab already runs run-parts on
# /etc/cron.{hourly,daily,weekly,monthly}, so logrotate et al. just work once
# cron is enabled (below). We only make sure the directories exist.
mkdir -p /etc/cron.hourly /etc/cron.daily /etc/cron.weekly /etc/cron.monthly \
         /var/spool/cron/crontabs
chmod 1730 /var/spool/cron/crontabs 2>/dev/null || true
note "cron run-parts dirs present (Debian /etc/crontab drives them)"

# ===========================================================================
log "Neovim starter config"
# ===========================================================================
NVIM_MARKER="-- AOK starter config (provision-ultimate-devuan.sh)"
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
-- AOK starter config (provision-ultimate-devuan.sh)
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
TMUX_MARKER="# AOK tmux.conf (provision-ultimate-devuan.sh)"
write_tmux() {  # <homedir> <owner>
    _hd="$1"; _own="$2"
    [ -n "$_hd" ] || return 0
    _cfg="$_hd/.tmux.conf"
    if [ -f "$_cfg" ] && ! grep -qF -e "$TMUX_MARKER" "$_cfg" 2>/dev/null; then
        note "tmux: keeping your existing $_cfg"
        return 0
    fi
    cat > "$_cfg" <<'TMUXCONF'
# AOK tmux.conf (provision-ultimate-devuan.sh)
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
# sysvinit equivalents of the Alpine OpenRC handling. update-rc.d enables a
# service at boot (runlevels 2-5); `service` starts it now. For everything
# EXCEPT ssh we (re)start so config changes apply; ssh is only ever 'start'ed so
# we never drop the provisioning session.
apply_svc() {  # <init-script-name>
    [ -x "/etc/init.d/$1" ] || { note "  no init script for $1 (skipped)"; return 0; }
    update-rc.d "$1" defaults >/dev/null 2>&1
    update-rc.d "$1" enable   >/dev/null 2>&1
    if [ "$1" = ssh ]; then
        service "$1" start >/dev/null 2>&1 || "/etc/init.d/$1" start >/dev/null 2>&1 || true
    elif service "$1" status >/dev/null 2>&1; then
        service "$1" restart >/dev/null 2>&1 || true
    else
        service "$1" start >/dev/null 2>&1 || "/etc/init.d/$1" start >/dev/null 2>&1 || true
    fi
}
# rsyslog first (so other daemons' early logs land), then user-facing daemons.
for s in rsyslog ssh cron chrony; do apply_svc "$s"; done
note "enabled at boot (runlevel 2):"
ls /etc/rc2.d/ 2>/dev/null | sed -n 's/^S[0-9]*//p' | sort -u | sed 's/^/      /'

# ===========================================================================
log "Done"
# ===========================================================================
printf '    %s\n' "$(date)"
note "Running services:"
service --status-all 2>&1 | grep -E '\[ \+ \]' | awk '{print $4}' | sort | tr '\n' ' ' | sed 's/^/      /'; echo
cat <<EOF

    Next:
      * Re-login (or relaunch the app) to pick up bash + the new prompt/MOTD,
        and to boot real sysvinit (sysvinit-core was just installed).
      * 'chronyc -h 127.0.0.1 tracking' / '... sources' to see NTP status.
      * 'service --status-all' for service health;  logs in /var/log/syslog.
EOF
