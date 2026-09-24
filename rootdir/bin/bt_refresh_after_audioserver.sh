#!/system/bin/sh
# Re-sync BT A2DP offload after audioserver/vendor.audio-hal restart.
# Sony btaudio_offload_if keeps a stale session unless Bluetooth is toggled.

if [ "$(settings get global bluetooth_on 2>/dev/null)" != "1" ]; then
    exit 0
fi

cmd bluetooth_manager disable
cmd bluetooth_manager wait-for-state:STATE_OFF
cmd bluetooth_manager enable
cmd bluetooth_manager wait-for-state:STATE_ON
