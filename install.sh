#!/bin/sh
# Install (or update) nest-picker on a rooted Nest Gen 1 over SSH.
#
#   ./install.sh [host]              install/update, keeps the existing config
#   ./install.sh [host] --config     also push device/nest-picker.conf
#   ./install.sh [host] --uninstall  remove it and give the screen back to the Nest UI
#
# Needs out/nest-picker (run ./build.sh first). SSH prompts for the root password
# unless you have a key installed.
set -e
cd "$(dirname "$0")"
HOST=${1:-${NEST_HOST:-192.168.1.112}}
ACTION=${2:-}
SSH_OPTS="-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1"
STAGE_DIR=/media/scratch/.nest-picker-stage

[ "$ACTION" = "--uninstall" ] || [ -f out/nest-picker ] || { echo "run ./build.sh first" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp device/setup.sh "$STAGE/"
if [ "$ACTION" = "--uninstall" ]; then
    MODE=uninstall
else
    MODE=install
    cp out/nest-picker device/nest-picker-start "$STAGE/"
    [ "$ACTION" = "--config" ] && cp device/nest-picker.conf "$STAGE/"
fi

# Old dropbear limits command length, so keep the remote command short and
# do the real work in setup.sh.
tar -C "$STAGE" -cf - . | ssh $SSH_OPTS "root@$HOST" \
    "rm -rf $STAGE_DIR; mkdir -p $STAGE_DIR && cd $STAGE_DIR && tar -xf - && sh setup.sh $MODE; cd /; rm -rf $STAGE_DIR"
