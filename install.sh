#!/usr/bin/env sh

set -eu

REPO="sammwyy/clay"
ASSET="clay-linux-x86_64"
INSTALL_DIR="${HOME}/.local/bin"
INSTALL_PATH="${INSTALL_DIR}/clay"
LATEST_URL="https://github.com/${REPO}/releases/latest"

command -v curl >/dev/null 2>&1 || {
    echo "curl is required to install Clay." >&2
    exit 1
}

latest_url="$(curl -fsSL -o /dev/null -w '%{url_effective}' "${LATEST_URL}")"
version="${latest_url##*/}"

case "${version}" in
    v[0-9]*.[0-9]*.[0-9]*) ;;
    *)
        echo "Could not determine the latest Clay release." >&2
        exit 1
        ;;
esac

download_url="https://github.com/${REPO}/releases/download/${version}/${ASSET}"
temporary_path="$(mktemp "${TMPDIR:-/tmp}/clay.XXXXXX")"
trap 'rm -f "${temporary_path}"' EXIT INT TERM

echo "Downloading latest release (${version})..."
curl -fL --retry 3 --retry-delay 1 -o "${temporary_path}" "${download_url}"

mkdir -p "${INSTALL_DIR}"
install -m 0755 "${temporary_path}" "${INSTALL_PATH}"

echo "Installed under ${INSTALL_PATH}"
case ":${PATH:-}:" in
    *":${INSTALL_DIR}:"*) ;;
    *) echo "For the current shell: export PATH=\"${INSTALL_DIR}:\$PATH\"" ;;
esac
