#!/bin/sh
# Install (or update) nest-picker on a rooted Nest Gen 1 over SSH.
#
#   ./install.sh [host] [options]
#     (no options)        install/update, keeps the config already on the Nest
#     --config            also push device/nest-picker.conf
#     --night-gif FILE    convert FILE (GIF/PNG) for night mode and push it
#     --uninstall         remove it and give the screen back to the Nest UI
#
# Options combine, e.g.  ./install.sh 192.168.1.112 --config --night-gif moon.gif
# Needs out/nest-picker (run ./build.sh first). SSH prompts for the root password
# unless you have a key installed.
set -e
cd "$(dirname "$0")"

HOST=${NEST_HOST:-192.168.1.112}
case "${1:-}" in -*|"") ;; *) HOST=$1; shift ;; esac

PUSH_CONFIG=0
NIGHT_GIF=
MODE=install
while [ $# -gt 0 ]; do
    case "$1" in
        --config) PUSH_CONFIG=1 ;;
        --night-gif) [ $# -ge 2 ] || { echo "--night-gif needs a file" >&2; exit 1; }; NIGHT_GIF=$2; shift ;;
        --uninstall) MODE=uninstall ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

SSH_OPTS="-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1"
STAGE_DIR=/media/scratch/.nest-picker-stage

[ "$MODE" = uninstall ] || [ -f out/nest-picker ] || { echo "run ./build.sh first" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp device/setup.sh "$STAGE/"
if [ "$MODE" = install ]; then
    cp out/nest-picker device/nest-picker-start "$STAGE/"
    [ "$PUSH_CONFIG" = 1 ] && cp device/nest-picker.conf "$STAGE/"
    if [ -n "$NIGHT_GIF" ]; then
        # Validate and convert locally first; nothing is sent to the Nest if this fails.
        uv run --project python gif2anim "$NIGHT_GIF" "$STAGE/night.anim"
    fi
fi

# Old dropbear limits command length, so keep the remote command short and
# do the real work in setup.sh.
tar -C "$STAGE" -cf - . | ssh $SSH_OPTS "root@$HOST" \
    "rm -rf $STAGE_DIR; mkdir -p $STAGE_DIR && cd $STAGE_DIR && tar -xf - && sh setup.sh $MODE; cd /; rm -rf $STAGE_DIR"
