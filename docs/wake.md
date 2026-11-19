# Wake and idle power

On the [Waveshare ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06),
use **BOOT** (GPIO0, active-low) after the device starts. A short press toggles the screen; hold
for about 1.2 seconds to start or end a voice session. A key already held
when the app starts is ignored until released. GPIO0 remains an input; do
not hold BOOT when turning on or resetting the board unless entering download
mode. The firmware does not use PWRON or GPIO18 for key input. The 2.16-inch
board has a different layout and is not supported by this firmware.

The default build does **not** recognize a wake word. The optional WakeNet
profile uses [Espressif ESP-SR 2.6.1](https://components.espressif.com/components/espressif/esp-sr/versions/2.6.1/readme)
and its official "Hi, ESP" model (`wn9_hiesp`), resolved independently by
ESP-IDF Component Manager under Espressif's terms. Use a separate build
directory and a fresh profile-specific sdkconfig:

```sh
idf.py -B build-wakenet -DWR_WAKE_WAKENET=ON -DSDKCONFIG=sdkconfig.local.wakenet build
```

Apply your Wi-Fi and relay settings to that sdkconfig, just as for the default
build. The WakeNet profile reserves an 8192-byte `app_main` stack so local
inference can run synchronously without a second microphone owner. Espressif
packages the selected model into `srmodels.bin`. Initial
provisioning of this profile must include the matching partition table and
`model` partition; an app-only write does not install model data. Install
only on a device intended for this profile. Use only the official model image
generated for the profile. A missing model reports an initialization error and
leaves BOOT available.

While the relay is configured, the network is ready and no call owns the
microphone, the firmware reads 24 kHz PCM, extracts the first microphone
channel, and resamples it locally to the 16 kHz input required by WakeNet.
Wake-word audio is not sent to the relay during this idle classification.
A confirmed match can start, but cannot stop, a call. The detector performs
no capture while a session connects, runs, or stops. It clears model/filter
state and discards an initial 250 ms of microphone input before listening
again. No model, vocabulary, or recordings are included in this repository.

Wi-Fi uses minimum modem sleep after two seconds of screen-off idle on
confirmed battery power. Battery-only state comes from read-only AXP2101
status over the board's shared I2C bus. USB, unknown supply, a lit screen,
or a pending/active voice session use `WIFI_PS_NONE`. This is Wi-Fi power
management, not MCU deep sleep; the ESP32-S3 stays available to handle BOOT.
