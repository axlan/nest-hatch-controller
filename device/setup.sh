#!/bin/sh
# Runs ON the Nest from the staging dir that install.sh unpacks.
#   sh setup.sh install | uninstall
RCLINE='(/usr/bin/nest-picker-start > /dev/null 2>&1) &'
RCHLINE='killall nest-picker-start nest-picker 2>/dev/null; sleep 1; /usr/bin/nest-picker --release 2>/dev/null'

CONF=/etc/nest-picker.conf
NIGHT_ANIM_DEFAULT=/media/data/nest-picker/night.anim

stop_picker() {
    killall nest-picker-start 2>/dev/null
    killall nest-picker 2>/dev/null   # it resumes nlclient on exit
    sleep 1
}

# Prints the value of KEY from the config (the file is shell KEY=VALUE).
conf_get() {
    [ -f "$CONF" ] || return 0
    (. "$CONF" >/dev/null 2>&1; eval "printf '%s' \"\$$1\"")
}

check_config() {
    tz=$(conf_get TIMEZONE)
    night=$(conf_get NIGHT_MODE)
    anim=$(conf_get NIGHT_ANIM)
    [ -n "$anim" ] || anim=$NIGHT_ANIM_DEFAULT
    if [ -n "$tz" ] && [ ! -f "/usr/share/zoneinfo/$tz" ]; then
        echo "WARNING: TIMEZONE=$tz is not in /usr/share/zoneinfo (e.g. America/Chicago)"
    fi
    if [ -n "$night" ]; then
        if ! echo "$night" | grep -qE '^[0-2][0-9]:[0-5][0-9]-[0-2][0-9]:[0-5][0-9]$'; then
            echo "WARNING: NIGHT_MODE=$night should look like 20:00-07:00"
        elif [ ! -f "$anim" ]; then
            echo "WARNING: NIGHT_MODE is set but $anim is missing; rerun install.sh with --night-gif"
        else
            echo "night mode $night (TIMEZONE=${tz:-system}), animation $anim"
        fi
    fi
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
    if [ -f night.anim ]; then
        anim=$(conf_get NIGHT_ANIM)
        [ -n "$anim" ] || anim=$NIGHT_ANIM_DEFAULT
        mkdir -p "$(dirname "$anim")" && cp night.anim "$anim" && echo "night animation pushed to $anim"
    fi
    check_config
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
    rm -rf /media/data/nest-picker
    sync
    echo "uninstalled (config left at /etc/nest-picker.conf); stock UI restored"
    ;;
esac
