# Browser settings used on this machine

Firefox / Zen (`user.js` in the profile, or about:config):

    user_pref("media.webrtc.camera.allow-pipewire", true);   // camera through PipeWire/libcamera

Chromium (`~/.config/chromium-flags.conf`):

    --ozone-platform-hint=wayland
    --ignore-gpu-blocklist
    --enable-features=WebRtcPipeWireCamera

`WebRtcPipeWireCamera` is what makes the libcamera device appear; without it
Chromium only sees the raw CAMSS V4L2 nodes, which are hidden. `--ignore-gpu-blocklist`
keeps GPU rasterisation on with freedreno/turnip.
