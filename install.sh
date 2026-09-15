#!/usr/bin/env bash
#
# install.sh - one-shot installer for daoTools.
#
# daoTools depends on daoBase (DAOROOT/DAODATA, dao.shm, waf, the conda env).
# This script:
#   1. daoBase        checks whether it's already installed (DAOROOT set and
#                      pointing at a real daoBase install). If not: looks for
#                      a sibling ../daoBase checkout, git-clones it otherwise,
#                      and runs *its* install.sh first.
#   2. system packages required by daoTools (CLI11, cfitsio, yaml-cpp, fmt,
#                      zmq, protobuf, ncurses) plus what it can for the
#                      optional ones (BLAS, FFTW - CUDA is left to you, see
#                      NVIDIA's own install docs, same as the README).
#   3. python deps     numpy/astropy/etc. are already covered by daoBase's
#                      own install; this adds the GUI stack (PyQt5,
#                      pyqtgraph, matplotlib) since gui/ is one of daoTools'
#                      three core deliverables, plus magicPlot.
#   4. build + install waf configure/build/install against the same DAOROOT.
#   5. verify          `import dao`, `import daoTools`, and libdaoTools.so
#                      are all reachable.
#
# Notes
#   - Idempotent: re-run freely.
#   - Non-destructive: never touches an already-working daoBase install; the
#     daoBase bootstrap (if needed) inherits that installer's own safety
#     properties (shell rc only touched after an explicit yes, etc.).
#   - Rehearse safely: --home <dir> redirects every path (a cloned daoBase,
#     DAOROOT default, ...) into a throwaway tree.
#
# Run  ./install.sh --help  for options.

set -euo pipefail

# Capture the inherited $DAOROOT (if any) before it gets overwritten below by
# this script's own --prefix/default computation - ensure_daobase() needs
# the *ambient* value to detect an already-active daoBase install.
AMBIENT_DAOROOT="${DAOROOT:-}"

# --------------------------------------------------------------------------
TOOLS_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

HOME_DIR="${HOME}"
HOME_OVERRIDDEN=0
DRY_RUN=0
ASSUME_YES=0
DO_SYSTEM_DEPS=1
DO_PYTHON=1
DO_BUILD=1
SKIP_DAOBASE=0
DAOBASE_SRC_IN=""
DAOBASE_REPO="https://github.com/Durham-Adaptive-Optics/daoBase.git"
DAOROOT_IN=""
DAODATA_IN=""

# --------------------------------------------------------------------------
if [ -t 1 ]; then
  C_B="\033[1m"; C_G="\033[32m"; C_Y="\033[33m"; C_R="\033[31m"; C_0="\033[0m"
else
  C_B=""; C_G=""; C_Y=""; C_R=""; C_0=""
fi
step()  { printf "\n${C_B}==> %s${C_0}\n" "$*"; }
info()  { printf "    %s\n" "$*"; }
ok()    { printf "    ${C_G}ok${C_0} %s\n" "$*"; }
warn()  { printf "    ${C_Y}warning:${C_0} %s\n" "$*" >&2; }
die()   { printf "\n${C_R}error:${C_0} %s\n" "$*" >&2; exit 1; }

run() {
  printf "    ${C_B}\$${C_0} %s\n" "$*"
  [ "$DRY_RUN" -eq 1 ] && return 0
  "$@"
}
run_sh() {
  printf "    ${C_B}\$${C_0} %s\n" "$*"
  [ "$DRY_RUN" -eq 1 ] && return 0
  bash -c "$*"
}
ask() {
  local q="$1" ans
  [ "$ASSUME_YES" -eq 1 ] && { info "$q -> yes (--yes)"; return 0; }
  read -r -p "    $q [y/N] " ans </dev/tty || ans=""
  [[ "$ans" == [yY] || "$ans" == [yY][eE][sS] ]]
}
ask_value() {
  local __var="$1" prompt="$2" def="$3" ans
  if [ "$ASSUME_YES" -eq 1 ]; then
    printf -v "$__var" '%s' "$def"; info "$prompt -> $def (--yes)"; return
  fi
  read -r -p "    $prompt [$def] " ans </dev/tty || ans=""
  printf -v "$__var" '%s' "${ans:-$def}"
}

# --------------------------------------------------------------------------
usage() {
  sed -n '3,29p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  --home DIR              Treat DIR as \$HOME for every path this installer
                           touches (including where a cloned daoBase lands).
  --daobase-src DIR        Existing daoBase checkout to use/install from.
                           Default: <this repo>/../daoBase
  --daobase-repo URL       git clone URL if daoBase needs fetching.
                           Default: $DAOBASE_REPO
  --prefix DIR             DAOROOT, only used if daoBase needs installing.
                           Default: /opt/dao/DAOROOT (or <home>/DAOROOT
                           under --home) - same default as daoBase's own
                           installer.
  --data DIR               DAODATA, same default rule as --prefix.
  --skip-daobase           Assume daoBase is already on PATH/PYTHONPATH;
                           don't check, clone, or install it.
  --skip-system-deps       Assume OS packages are already present.
  --skip-python            Do not pip install the Python (GUI) dependencies.
  --skip-build             Do not run the waf build/install.
  --yes                    Accept every default / prompt.
  --dry-run                Print commands without executing.
  -h, --help               This help.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --home)             HOME_DIR="$2"; HOME_OVERRIDDEN=1; shift 2 ;;
    --daobase-src)       DAOBASE_SRC_IN="$2"; shift 2 ;;
    --daobase-repo)      DAOBASE_REPO="$2"; shift 2 ;;
    --prefix)            DAOROOT_IN="$2"; shift 2 ;;
    --data)              DAODATA_IN="$2"; shift 2 ;;
    --skip-daobase)      SKIP_DAOBASE=1; shift ;;
    --skip-system-deps)  DO_SYSTEM_DEPS=0; shift ;;
    --skip-python)       DO_PYTHON=0; shift ;;
    --skip-build)        DO_BUILD=0; shift ;;
    --yes|-y)            ASSUME_YES=1; shift ;;
    --dry-run)           DRY_RUN=1; shift ;;
    -h|--help)           usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

HOME_DIR="$(cd "$HOME_DIR" 2>/dev/null && pwd || echo "$HOME_DIR")"
if [ "$HOME_OVERRIDDEN" -eq 1 ]; then
  DAOROOT_DEFAULT="$HOME_DIR/DAOROOT"
  DAODATA_DEFAULT="$HOME_DIR/DAODATA"
  DAOBASE_SRC_DEFAULT="$HOME_DIR/daoBase"
else
  DAOROOT_DEFAULT="/opt/dao/DAOROOT"
  DAODATA_DEFAULT="/opt/dao/DAODATA"
  DAOBASE_SRC_DEFAULT="$TOOLS_SRC/../daoBase"
fi
DAOROOT="${DAOROOT_IN:-$DAOROOT_DEFAULT}"
DAODATA="${DAODATA_IN:-$DAODATA_DEFAULT}"
DAOBASE_SRC="${DAOBASE_SRC_IN:-$DAOBASE_SRC_DEFAULT}"

# --------------------------------------------------------------------------
OS="unknown"; DISTRO="unknown"; PKG=""; ARCH="$(uname -m)"
detect_platform() {
  case "$(uname -s)" in
    Linux)  OS="linux" ;;
    Darwin) OS="macos" ;;
    *) die "unsupported OS: $(uname -s)" ;;
  esac
  if [ "$OS" = macos ]; then
    DISTRO="macos"; PKG="brew"
  elif [ -r /etc/os-release ]; then
    . /etc/os-release
    DISTRO="${ID:-linux}"
    local like="${ID_LIKE:-}"
    if   command -v apt-get >/dev/null 2>&1; then PKG="apt"
    elif command -v dnf     >/dev/null 2>&1; then PKG="dnf"
    elif command -v yum     >/dev/null 2>&1; then PKG="yum"
    elif command -v pacman  >/dev/null 2>&1; then PKG="pacman"
    elif command -v zypper  >/dev/null 2>&1; then PKG="zypper"
    fi
    [ -n "$PKG" ] || case "$like" in
      *debian*) PKG="apt" ;; *rhel*|*fedora*) PKG="dnf" ;;
      *suse*)   PKG="zypper" ;; *arch*) PKG="pacman" ;;
    esac
  fi
  case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    aarch64|arm64) ARCH="arm64" ;;
  esac
  info "OS=$OS  distro=$DISTRO  pkg-manager=${PKG:-none}  arch=$ARCH"
}

# --------------------------------------------------------------------------
# Is $1 a real, built daoBase install prefix (not just an empty directory)?
daobase_looks_installed() {
  local root="$1"
  [ -f "$root/include/dao.h" ] || return 1
  ls "$root"/lib*/libdao.* >/dev/null 2>&1 || return 1
  return 0
}

CONDA_ENV=""; CONDA_BIN=""
find_conda() {
  local c
  for c in "${CONDA_EXE:-}" "$(command -v conda 2>/dev/null || true)" \
           "$HOME_DIR/miniconda3/bin/conda" "$HOME_DIR/anaconda3/bin/conda" \
           "$HOME_DIR/miniforge3/bin/conda" "$HOME_DIR/mambaforge/bin/conda" \
           "/opt/miniconda3/bin/conda" "/opt/conda/bin/conda"; do
    [ -n "$c" ] && [ -x "$c" ] && { CONDA_BIN="$c"; return 0; }
  done
  return 1
}

# run a command in the daoBase conda env, if one was found; plainly otherwise
env_run() {
  if [ -z "$CONDA_BIN" ] || [ -z "$CONDA_ENV" ]; then run "$@"; return; fi
  run "$CONDA_BIN" run --no-capture-output -n "$CONDA_ENV" "$@"
}
# Shell prefix string (source conda + activate) to embed ahead of a command
# run via run_sh's own fresh `bash -c` child - safe there even though this
# script's own top-level `set -u` would otherwise be a problem for conda's
# activate script (same reasoning as daoBase's own env_prefix()). Used for
# things env_run can't do cleanly, like a python heredoc on stdin.
env_prefix() {
  local p=""
  if [ -n "$CONDA_BIN" ] && [ -n "$CONDA_ENV" ]; then
    local croot; croot="$(cd "$(dirname "$CONDA_BIN")/.." && pwd)"
    p=". \"$croot/etc/profile.d/conda.sh\"; conda activate $CONDA_ENV;"
  fi
  printf '%s' "$p"
}

# --------------------------------------------------------------------------
ensure_daobase() {
  step "daoBase dependency"

  if [ "$SKIP_DAOBASE" -eq 1 ]; then
    info "skipped (--skip-daobase) - assuming daoBase is already set up"
    [ -n "$AMBIENT_DAOROOT" ] && [ -d "$AMBIENT_DAOROOT" ] || warn "\$DAOROOT is not set/doesn't exist in this shell; the build below may fail"
    return
  fi

  # 1) Already installed? Check --prefix (if given), the ambient $DAOROOT
  #    (if this shell already has one exported), then the two default
  #    locations daoBase's own installer would have used.
  local candidate found=""
  for candidate in "$DAOROOT_IN" "$AMBIENT_DAOROOT" "/opt/dao/DAOROOT" "$HOME_DIR/DAOROOT"; do
    [ -n "$candidate" ] || continue
    if daobase_looks_installed "$candidate"; then found="$candidate"; break; fi
  done

  if [ -n "$found" ]; then
    ok "daoBase already installed: DAOROOT=$found"
    DAOROOT="$found"
    export DAOROOT
    export PATH="$PATH:$DAOROOT/bin"
    export PYTHONPATH="${PYTHONPATH:-}:$DAOROOT/python"
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$DAOROOT/lib:$DAOROOT/lib64"
    # Respect whatever conda env (if any) is already active in this shell -
    # we don't know which one produced this DAOROOT, so don't guess further.
    if [ -n "${CONDA_DEFAULT_ENV:-}" ] && find_conda; then
      CONDA_ENV="$CONDA_DEFAULT_ENV"
      info "using already-active conda env '$CONDA_ENV'"
    fi
    return
  fi

  info "daoBase not found (checked --prefix, \$DAOROOT=${AMBIENT_DAOROOT:-<unset>}, /opt/dao/DAOROOT, $HOME_DIR/DAOROOT)"

  # 2) Locate or clone a daoBase source checkout.
  if [ -d "$DAOBASE_SRC/.git" ] || [ -f "$DAOBASE_SRC/install.sh" ]; then
    ok "found daoBase source: $DAOBASE_SRC"
  else
    ask_value DAOBASE_SRC "Where should daoBase be cloned to?" "$DAOBASE_SRC"
    if [ -e "$DAOBASE_SRC" ]; then
      die "$DAOBASE_SRC already exists but doesn't look like a daoBase checkout (no install.sh) - remove it, or pass --daobase-src to point elsewhere"
    fi
    ask "Clone $DAOBASE_REPO into $DAOBASE_SRC ?" || die "daoBase source is required (or pass --skip-daobase if it's set up some other way)"
    run mkdir -p "$(dirname "$DAOBASE_SRC")"
    run_sh "git clone '$DAOBASE_REPO' '$DAOBASE_SRC'"
  fi

  if [ "$DRY_RUN" -eq 0 ]; then
    [ -x "$DAOBASE_SRC/install.sh" ] || die "$DAOBASE_SRC/install.sh not found or not executable"
  fi

  # 3) Run daoBase's own installer, targeting the same DAOROOT/DAODATA this
  #    script will build daoTools against.
  step "Installing daoBase (delegating to $DAOBASE_SRC/install.sh)"
  local daobase_args=(--prefix "$DAOROOT" --data "$DAODATA")
  [ "$ASSUME_YES" -eq 1 ] && daobase_args+=(--yes)
  [ "$DRY_RUN" -eq 1 ] && daobase_args+=(--dry-run)
  [ "$HOME_OVERRIDDEN" -eq 1 ] && daobase_args+=(--home "$HOME_DIR")
  run_sh "cd '$DAOBASE_SRC' && ./install.sh $(printf '%q ' "${daobase_args[@]}")"

  if [ "$DRY_RUN" -eq 1 ]; then
    info "(dry-run) stopping here - daoBase's own install (previewed above) and this script's verification would follow"
    return
  fi

  daobase_looks_installed "$DAOROOT" || die "daoBase's installer finished but $DAOROOT still doesn't look installed - check the log above"
  ok "daoBase installed: DAOROOT=$DAOROOT"
  export DAOROOT
  export PATH="$PATH:$DAOROOT/bin"
  export PYTHONPATH="${PYTHONPATH:-}:$DAOROOT/python"
  export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$DAOROOT/lib:$DAOROOT/lib64"

  # Pick up the conda env daoBase just set up, by reading the concrete values
  # dao_env.sh recorded - NOT by sourcing the file (its conda_block sources
  # conda's own activate script, which is known to misbehave under `set -u`;
  # daoBase's own installer avoids this the same way, via `conda run -n`).
  local env_file="$DAOBASE_SRC/dao_env.sh"
  if [ -f "$env_file" ]; then
    CONDA_ENV="$(grep -m1 'conda activate ' "$env_file" 2>/dev/null | awk '{print $3}' || true)"
    if [ -n "$CONDA_ENV" ]; then
      find_conda || warn "dao_env.sh mentions conda env '$CONDA_ENV' but no conda binary was found"
      info "using conda env '$CONDA_ENV' from daoBase's install"
    fi
  fi
}

# --------------------------------------------------------------------------
system_deps() {
  step "System packages"
  if [ "$DO_SYSTEM_DEPS" -eq 0 ]; then info "skipped (--skip-system-deps)"; return; fi

  # Required: every app in apps/ links at least protobuf, zmq, and CLI11.
  # Optional: BLAS (daoMvM CPU path) and FFTW single+double (the FFT
  # correlation centroider) - waf configure auto-detects and silently skips
  # the app(s) that need them if missing, so these are best-effort here.
  # CUDA (daoMvMGPU) is deliberately not handled: see NVIDIA's own install
  # docs, same as the README - too toolkit/GPU-specific to script safely.
  local apt_pkgs="libcli11-dev libcfitsio-dev libyaml-cpp-dev libfmt-dev \
libzmq3-dev libprotobuf-dev protobuf-compiler libncurses-dev \
libopenblas-dev libfftw3-dev"
  local dnf_pkgs="cfitsio-devel yaml-cpp-devel fmt-devel zeromq-devel \
protobuf-devel protobuf-compiler ncurses-devel openblas-devel fftw-devel"
  local pacman_pkgs="cli11 cfitsio yaml-cpp fmt zeromq protobuf ncurses openblas fftw"
  local zypper_pkgs="cfitsio-devel yaml-cpp-devel fmt-devel libzmq5-devel \
protobuf-devel ncurses-devel openblas-devel fftw3-devel"
  local brew_pkgs="cli11 cfitsio yaml-cpp fmt zeromq protobuf openblas fftw"

  local sudo=""; [ "$(id -u)" -ne 0 ] && sudo="sudo"

  case "$PKG" in
    apt)
      ask "Run: $sudo apt-get install -y <build deps> ?" || { warn "skipped; install manually"; return; }
      run_sh "$sudo apt-get update"
      run_sh "$sudo apt-get install -y $apt_pkgs"
      ;;
    dnf|yum)
      ask "Run: $sudo $PKG install -y <build deps> ?" || { warn "skipped; install manually"; return; }
      # --allowerasing: minimal RHEL/Rocky/Fedora images ship curl-minimal;
      # harmless here too if some other package needs swapping in. dnf-only.
      local dnf_extra=""; [ "$PKG" = dnf ] && dnf_extra="--allowerasing"
      run_sh "$sudo $PKG install -y $dnf_extra $dnf_pkgs" || warn "some packages may require EPEL/PowerTools"
      info "CLI11 has no RHEL/Fedora package - configure falls back to a vendored header if pkg-config can't find it"
      ;;
    pacman)
      ask "Run: $sudo pacman -S --needed <build deps> ?" || { warn "skipped"; return; }
      run_sh "$sudo pacman -S --needed --noconfirm $pacman_pkgs"
      ;;
    zypper)
      ask "Run: $sudo zypper install <build deps> ?" || { warn "skipped"; return; }
      run_sh "$sudo zypper --non-interactive install $zypper_pkgs"
      ;;
    brew)
      command -v brew >/dev/null 2>&1 || die "Homebrew not found - install from https://brew.sh then re-run"
      run_sh "brew install $brew_pkgs"
      ;;
    *)
      warn "unknown package manager - install manually: CLI11, cfitsio, yaml-cpp, fmt,"
      info "  zeromq, protobuf(+compiler), ncurses (required), openblas, fftw (optional)"
      ;;
  esac
}

python_deps() {
  step "Python dependencies (pip)"
  if [ "$DO_PYTHON" -eq 0 ]; then info "skipped (--skip-python)"; return; fi

  # numpy/astropy/pyzmq/protobuf/etc. are already installed by daoBase's own
  # installer - this only adds what daoTools needs on top: the GUI stack
  # (gui/ is one of daoTools' three core deliverables, so installed
  # unconditionally rather than gated behind an --with-extras flag) plus
  # magicPlot, which some plotting tools use and the README recommends.
  local pkgs="PyQt5 pyqtgraph matplotlib magicPlot"

  env_run python -m pip install --upgrade pip
  # shellcheck disable=SC2086
  env_run python -m pip install $pkgs
}

build_waf() {
  step "Build & install daoTools (waf)"
  if [ "$DO_BUILD" -eq 0 ]; then info "skipped (--skip-build)"; return; fi
  local waf="waf"
  command -v waf >/dev/null 2>&1 || waf="$HOME_DIR/bin/waf"
  [ -x "$waf" ] || command -v "$waf" >/dev/null 2>&1 || die "waf not found - it should have come with daoBase; pass --daobase-src or install waf yourself"
  run_sh "$(env_prefix) cd \"$TOOLS_SRC\" && \"$waf\" configure --prefix=\"$DAOROOT\" && \"$waf\" && \"$waf\" install"
}

verify() {
  step "Verify"
  [ "$DRY_RUN" -eq 1 ] && { info "(dry-run) skipping"; return; }
  if run_sh "$(env_prefix) python - <<PY
import dao, daoTools
print('dao        OK ->', dao.__file__)
print('daoTools   OK ->', daoTools.__file__)
PY"; then
    ok "Python import of dao + daoTools works"
  else
    warn "verification failed - open a new shell (or make sure DAOROOT/PYTHONPATH are set) and retry: python -c 'import dao, daoTools'"
    return
  fi
  if ls "$DAOROOT"/lib*/libdaoTools.* >/dev/null 2>&1; then
    ok "libdaoTools found in $DAOROOT"
  else
    warn "libdaoTools.* not found under $DAOROOT/lib(64) - check the build log above"
  fi
}

# --------------------------------------------------------------------------
main() {
  step "daoTools installer"
  info "source tree : $TOOLS_SRC"
  info "home        : $HOME_DIR"
  [ "$DRY_RUN" -eq 1 ] && warn "DRY RUN - nothing will be changed"
  detect_platform

  ensure_daobase
  system_deps
  python_deps
  build_waf
  verify

  step "Done"
  info "DAOROOT = $DAOROOT"
  [ -n "$CONDA_ENV" ] && info "conda activate $CONDA_ENV"
  info "Then:  python -c 'import daoTools'"
}

main
