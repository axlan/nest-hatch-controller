#!/bin/sh
# Runs ON the Nest from the staging dir that install.sh unpacks.
#   sh setup.sh install | uninstall
RCLINE='(/usr/bin/nest-picker-start > /dev/null 2>&1) &'
RCHLINE='killall nest-picker-start nest-picker 2>/dev/null; sleep 1; /usr/bin/nest-picker --release 2>/dev/null'

stop_picker() {
    killall nest-picker-start 2>/dev/null
    killall nest-picker 2>/dev/null   # it resumes nlclient on exit
    sleep 1
}

case "$1" in
install)
    stop_picker
    cp nest-picker nest-picker-start /usr/bin/ || exit 1
    chmod 755 /usr/bin/nest-picker /usr/bin/nest-picker-start
    if [ -f nest-picker.conf ]; then
        cp nest-picker.conf /etc/nest-picker.conf && echo "config pushed"
    elif [ ! -f /etc/nest-picker.conf ]; then
        echo "WARNING: no /etc/nest-picker.conf; rerun install.sh with --config"
    fi
    grep -q nest-picker-start /etc/init.d/rcS || echo "$RCLINE" >> /etc/init.d/rcS
    # On shutdown, stop the picker first so nlclient is resumed and can be
    # stopped normally (a frozen nlclient stalls reboot for ~6 minutes).
    grep -q nest-picker /etc/init.d/rcH || sed -i "/^# Stop system services/a $RCHLINE" /etc/init.d/rcH
    sync
    (/usr/bin/nest-picker-start --now > /dev/null 2>&1 &)
    echo "installed; nest-picker started"
    ;;
uninstall)
    sed -i '/nest-picker/d' /etc/init.d/rcS /etc/init.d/rcH
    stop_picker
    /usr/bin/nest-picker --release 2>/dev/null
    rm -f /usr/bin/nest-picker /usr/bin/nest-picker-start
    sync
    echo "uninstalled (config left at /etc/nest-picker.conf); stock UI restored"
    ;;
esac
