#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <profile> [make-args...]" >&2
    exit 2
fi

PROFILE="$1"
shift

IMAGE="${OS01_CI_IMAGE:-ghcr.io/aagu/os01-ci:latest}"
SCRIPT_DIR="$(cd -- "${BASH_SOURCE[0]%/*}" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"

RUNTIME=""
if command -v docker >/dev/null 2>&1; then
    RUNTIME="docker"
elif command -v podman >/dev/null 2>&1; then
    RUNTIME="podman"
else
    echo "ERROR: neither 'docker' nor 'podman' was found in PATH." >&2
    echo "Install Docker (https://docs.docker.com/get-docker/) or Podman (https://podman.io/docs/installation) to use ci-local." >&2
    exit 2
fi

if ! "$RUNTIME" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "ERROR: container image '$IMAGE' is not available locally (auto-pull disabled)." >&2
    echo "Pull it manually first:" >&2
    echo "  $RUNTIME pull $IMAGE" >&2
    exit 2
fi

EXTRA_MOUNTS=()
if [ -f "$REPO_ROOT/.git" ] && command -v git >/dev/null 2>&1; then
    COMMON_GIT_DIR="$(git -C "$REPO_ROOT" rev-parse --git-common-dir 2>/dev/null || true)"
    if [ -n "$COMMON_GIT_DIR" ] && [ -d "$COMMON_GIT_DIR" ]; then
        EXTRA_MOUNTS+=(-v "$COMMON_GIT_DIR:$COMMON_GIT_DIR")
    fi
fi

echo "+ $RUNTIME run --rm -v $REPO_ROOT:$REPO_ROOT ${EXTRA_MOUNTS[*]:+${EXTRA_MOUNTS[*]} }-w $REPO_ROOT $IMAGE make PROFILE=$PROFILE $*"
exec "$RUNTIME" run --rm \
    -v "$REPO_ROOT:$REPO_ROOT" \
    "${EXTRA_MOUNTS[@]}" \
    -w "$REPO_ROOT" \
    "$IMAGE" \
    make "PROFILE=$PROFILE" "$@"
