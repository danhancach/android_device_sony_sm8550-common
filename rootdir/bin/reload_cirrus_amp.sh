#!/vendor/bin/sh
# Re-probe cs35l45 amps after /vendor/firmware is available.
# Fixes L speaker DSP firmware race at early boot (DSP1 legacy parse -19).

wait_firmware() {
    local f="$1"
    local i=0
    while [ ! -f "$f" ] && [ "$i" -lt 50 ]; do
        sleep 0.2
        i=$((i + 1))
    done
    [ -f "$f" ]
}

wait_firmware /vendor/firmware/L-cs35l45-dsp1-spk-prot.wmfw || exit 0
wait_firmware /vendor/firmware/R-cs35l45-dsp1-spk-prot.wmfw || exit 0

DEVICES=""
for dev in /sys/bus/i2c/devices/*; do
    [ -e "$dev/driver" ] || continue
    case "$(readlink "$dev/driver" 2>/dev/null)" in
        */cs35l45) DEVICES="$DEVICES $(basename "$dev")" ;;
    esac
done

for name in $DEVICES; do
    echo "$name" > /sys/bus/i2c/drivers/cs35l45/unbind 2>/dev/null
done

sleep 1

for name in $DEVICES; do
    echo "$name" > /sys/bus/i2c/drivers/cs35l45/bind 2>/dev/null
done
