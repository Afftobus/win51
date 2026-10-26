#!/bin/bash
# Development from another machine: copies the working tree to a Windows PC over SSH, keeping the PC's
# deps\ (EWDK, FFmpeg) and out\. The PC's OpenSSH must use PowerShell as its default shell.
#   WIN_HOST   ssh host (default: win51)
#   WIN_DIR    repository location on the PC (default: D:\win51\src)
set -euo pipefail
HOST=${WIN_HOST:-win51}
DIR=${WIN_DIR:-'D:\win51\src'}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
COPYFILE_DISABLE=1 tar -C "$ROOT" --exclude .git --exclude .idea --exclude '*.iml' --exclude out --exclude deps \
    -czf "$TMP/src.tgz" .
scp -q "$TMP/src.tgz" "$HOST:win51-src.tgz"   # lands in the remote user's home directory
# Windows OpenSSH strips double quotes from the command line, so only single quotes are used remotely.
ssh "$HOST" "\$tgz = Join-Path \$HOME 'win51-src.tgz'; New-Item -ItemType Directory -Force '$DIR' | Out-Null; Get-ChildItem -Force '$DIR' | Where-Object Name -notin 'out','deps' | Remove-Item -Recurse -Force; tar -xzf \$tgz -C '$DIR'; Remove-Item \$tgz"
echo "synced to $HOST:$DIR"
