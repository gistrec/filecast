#!/bin/sh
# Filecast installer: downloads the prebuilt binary for this platform from
# GitHub Releases, verifies its SHA-256 against the release's checksums.txt,
# and installs it as `filecast`.
#
#   curl -fsSL https://raw.githubusercontent.com/gistrec/Filecast/master/install.sh | sh
#
# Options (flags or environment variables):
#   --version vX.Y.Z   / FILECAST_VERSION   install a specific release (default: latest)
#   --bin-dir <dir>    / BIN_DIR            install directory (default: /usr/local/bin)
#
# The whole script is wrapped in main() and the last line calls it, so a
# partially downloaded script fails to parse instead of running half-way.
set -eu

REPO="gistrec/Filecast"
# Overridable base URL so the script can be tested against a local mock server.
DOWNLOAD_BASE="${FILECAST_DOWNLOAD_BASE:-https://github.com/${REPO}/releases}"

info() { printf '%s\n' "install.sh: $*" >&2; }
error() { printf '%s\n' "install.sh: error: $*" >&2; exit 1; }

has() { command -v "$1" >/dev/null 2>&1; }

detect_target() {
    os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    arch="$(uname -m | tr '[:upper:]' '[:lower:]')"

    case "$arch" in
        x86_64|amd64) arch="x86_64" ;;
        aarch64|arm64) arch="arm64" ;;
        *) error "unsupported architecture: $arch (prebuilt binaries cover x86_64 and arm64;
           see https://github.com/${REPO}#building-from-source)" ;;
    esac

    case "$os" in
        linux) TARGET="filecast-linux-${arch}" ;;
        # One universal binary covers both Apple Silicon and Intel Macs.
        darwin) TARGET="filecast-macos-universal" ;;
        mingw*|msys*|cygwin*)
            if [ "$arch" != "x86_64" ]; then
                error "no prebuilt Windows binary for $arch"
            fi
            TARGET="filecast-windows-x86_64.exe"
            ;;
        *) error "unsupported OS: $os (see https://github.com/${REPO}#building-from-source)" ;;
    esac
}

# download <url> <output-file>; fails on HTTP errors (curl -f) instead of
# saving an HTML error page as the "binary". Real release URLs are pinned to
# HTTPS; plain http is only reachable via FILECAST_DOWNLOAD_BASE (tests).
download() {
    if has curl; then
        case "$1" in
            https://*) curl -fsSL --proto '=https' --tlsv1.2 -o "$2" "$1" ;;
            *) curl -fsSL -o "$2" "$1" ;;
        esac
    elif has wget; then
        case "$1" in
            https://*) wget --https-only -q -O "$2" "$1" ;;
            *) wget -q -O "$2" "$1" ;;
        esac
    else
        error "neither curl nor wget found"
    fi
}

# verify_checksum <file> <checksums.txt>: extract the file's expected SHA-256
# from the checksums list and compare with whichever sha tool this system has
# (sha256sum on Linux, shasum on macOS, openssl as a last resort).
verify_checksum() {
    file="$1"
    sums="$2"
    name="$(basename "$file")"
    expected="$(awk -v n="$name" '{ sub(/\r$/, "") } $2 == n || $2 == "*"n {print $1}' "$sums" | head -n 1)"
    [ -n "$expected" ] || error "no checksum for $name in checksums.txt"

    if has sha256sum; then
        actual="$(sha256sum "$file" | awk '{print $1}')"
    elif has shasum; then
        actual="$(shasum -a 256 "$file" | awk '{print $1}')"
    elif has openssl; then
        actual="$(openssl dgst -sha256 -r "$file" | awk '{print $1}')"
    else
        error "no SHA-256 tool found (need sha256sum, shasum, or openssl)"
    fi

    if [ "$actual" != "$expected" ]; then
        error "checksum mismatch for $name
           expected: $expected
           actual:   $actual
         The download may be corrupted or tampered with. Not installing."
    fi
    info "checksum verified ($expected)"
}

install_binary() {
    src="$1"
    dest_dir="$2"
    dest="$dest_dir/filecast"
    case "$TARGET" in *.exe) dest="$dest.exe" ;; esac

    if [ ! -d "$dest_dir" ]; then
        mkdir -p "$dest_dir" 2>/dev/null || true
    fi

    # Probe with an actual write: `test -w` lies on some systems (e.g. root
    # squash, ACLs); mktemp avoids following a pre-planted symlink. `install`
    # also sets the execute bit and replaces a running binary without ETXTBSY
    # errors, unlike `cp`.
    if probe="$(mktemp "$dest_dir/.filecast-XXXXXX" 2>/dev/null)"; then
        rm -f "$probe"
        install -m 755 "$src" "$dest"
    elif has sudo; then
        info "$dest_dir is not writable, retrying with sudo"
        # `install` without -d does not create the destination directory, and
        # a fresh macOS has /usr/local but no /usr/local/bin.
        [ -d "$dest_dir" ] || sudo install -d -m 755 "$dest_dir"
        sudo install -m 755 "$src" "$dest"
    else
        error "$dest_dir is not writable and sudo is unavailable.
         Re-run with a user-writable directory, e.g.:
           curl -fsSL https://raw.githubusercontent.com/${REPO}/master/install.sh \\
             | BIN_DIR=\"\$HOME/.local/bin\" sh"
    fi
    DEST="$dest"
}

main() {
    version="${FILECAST_VERSION:-latest}"
    bin_dir="${BIN_DIR:-/usr/local/bin}"

    while [ $# -gt 0 ]; do
        case "$1" in
            --version) [ $# -ge 2 ] || error "--version requires a value"
                       version="$2"; shift 2 ;;
            --bin-dir) [ $# -ge 2 ] || error "--bin-dir requires a value"
                       bin_dir="$2"; shift 2 ;;
            -h|--help)
                printf '%s\n' \
                    "usage: install.sh [--version vX.Y.Z] [--bin-dir <dir>]" \
                    "  --version vX.Y.Z   / FILECAST_VERSION   release to install (default: latest)" \
                    "  --bin-dir <dir>    / BIN_DIR            install directory (default: /usr/local/bin)"
                exit 0 ;;
            *) error "unknown option: $1" ;;
        esac
    done

    detect_target

    # `releases/latest/download/<asset>` is a plain HTTP redirect to the
    # newest stable release: no GitHub API, no JSON parsing, no rate limits.
    if [ "$version" = "latest" ]; then
        url_base="${DOWNLOAD_BASE}/latest/download"
    else
        url_base="${DOWNLOAD_BASE}/download/${version}"
    fi

    tmpdir="$(mktemp -d)"
    # Cleanup on EXIT; a signal must actually exit (a trapped signal otherwise
    # resumes the script, which would then read from the deleted tmpdir).
    trap 'rm -rf "$tmpdir"' EXIT
    trap 'exit 130' INT TERM

    hint=""
    case "$version" in
        latest|v*) ;;
        *) hint=" (did you mean 'v${version}'?)" ;;
    esac

    info "downloading ${url_base}/${TARGET}"
    download "${url_base}/${TARGET}" "$tmpdir/$TARGET" \
        || error "download failed${hint}.
         Releases: https://github.com/${REPO}/releases"
    download "${url_base}/checksums.txt" "$tmpdir/checksums.txt" \
        || error "failed to download checksums.txt for $version"

    verify_checksum "$tmpdir/$TARGET" "$tmpdir/checksums.txt"
    install_binary "$tmpdir/$TARGET" "$bin_dir"

    info "installed $("$DEST" --version 2>/dev/null || echo filecast) to $DEST"
    case ":$PATH:" in
        *":$bin_dir:"*) ;;
        *) info "note: $bin_dir is not in your PATH" ;;
    esac
}

main "$@"
