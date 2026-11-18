# S22 (SM-S901E) Linux - what's sorted and what's not (updated 2026-11-18)

## Working
- [x] Boots postmarketOS from internal storage (uniLoader + mainline 7.2.11)
- [x] USB networking + SSH (USB 3.0 SuperSpeed + USB 2.0 fallback, auto-connects at boot)
- [x] Display: real S6E3FAC panel driver, frames straight to panel at 120 Hz + dynamic VRR/HBM/mDNIe/AOD
- [x] Touch (FTS2BA61Y multitouch, portrait orientation correct)
- [x] GPU (Adreno 730 freedreno/Turnip Wave 5 + `msm-w10`, KWin GPU compositing on `card0`)
- [x] Bluetooth (address from EFS, A2DP audio in & car-stereo audio out)
- [x] zram swap, thermal sensors, writable hardware RTC (`pmk8350_sdam_21`) + NTP clock
- [x] Plasma Mobile (tinydm autologin, 2.5x scale); power, volume down and volume up buttons work

## Sorted bring-up items
- [x] **Wi-Fi CONNECTS** (2.4 GHz + 5 GHz, Wi-Fi 6 2x2, 960 Mbit/s link), autoconnects at boot (`s22-wlan-pdc` enables 2.2 V `s3e` PA rail; PCIe ASPM `powersave` active)
- [x] **Wi-Fi signal** fixed by Samsung board file (-35 dBm 2.4 GHz, -51 dBm 5 GHz at 6 m)
- [x] **RAM**: +854 MiB total reclaimed, **7.0 GiB (7,161 MiB) usable** (full 32 MiB `cdsp_mem`, modem/camera/CVP kept)
- [x] **On-screen keyboard + auto-hide**: `plasma-keyboard` as KWin input method + `s22-kbd-autohide.service` (tapping above keyboard hides it and re-arms for next text field)
- [x] **Charging + battery % + charge limit**: `max77705-charger` + `max170xx_battery` linked via `power-supplies = <&max77705_charger>`, accurate 2 mOhm scaling (`3,700 mAh`), USB-C Rp 3.0 A `CHGIN_ILIM`, `MAX77705_AICL_I` 345 Hz IRQ storm masked (`0 IRQs/s`), and `charge_control_end_threshold` (`50-100%`) + `s22-charge-limit`
- [x] **Idle & active power optimizations**: `lpass-va-macro` releases `LPASS_HW_MACRO`/`LPASS_HW_DCODEC` on runtime suspend (ADSP sleep `0.017%` -> `100%` idle), `fts2ba61y` `sense_enable` gates touch scanning/IRQ on `dpms=Off` via `panel-samsung-s6e3fac` `KOBJ_CHANGE` uevent (`hw=0Hz`) + `s22-display-extrasd` netlink monitor, `org.kde.plasma.quicksetting.flashlight` drops its 2.5 s polling timer, `max77705-charger` debounces redundant `chgin-irq` notifications, and `uniLoader` `boot.img` was rebuilt & flashed with **`ufs-qcom` lane clk/ICC fix (`clk_enable_count=0` suspended)**, **`dwc3-qcom` `USB_ROLE_NONE` DDR ICC gating**, **`llcc_bwmon`/`cpu_bwmon` 200 MHz DDR OPPs**, **Adreno 730 GPU 818 MHz devfreq unlock + 66 ms autosuspend + Cortex-A710 `gpu-irq` affinity**, and demand-gated `s2mpb02-l6/l7/l9/l10` camera VIO LDOs
- [x] **USB auto-connect at boot** (MAX77705 Type-C role switch; SSH ~25 s after boot, no replug)
- [x] **Firewall** (nftables)
- [x] **Apps**: QMLKonsole, Google Chrome (Flathub ARM64), Discover + Flathub, KRecorder (GStreamer backend), Dolphin, Plasma Camera
- [x] **Volume up** button (gpio-keys on PM8350 GPIO 6)
- [x] **Audio + Speaker Protection**: stereo loudspeakers (top = left, bottom = right) + handset Earpiece + 2 microphones + VoiceCall UCM2 + **Cirrus CS35L41 Halo DSP speaker protection** (`cirrus,subsystem-id = "r0q"`, `s22-speaker-prot` impedance/ambient calibration, ~+5 dB louder safe output)
- [x] **Cellular modem + SM8450 IPA v5.1 (`qcom,sm8450-ipa`)**: auto-starts at boot (`s22-modem.service`, read-only `rmtfs`), IPA v5.1 + `rmnet` + `rpmsg_wwan_ctrl` in **ModemManager (`qcom-soc`)** for 4G/5G data, SMS, Voice Calls (`81voltd` + `VoiceCall` UCM2), Dual-SIM UIM & GNSS
- [x] **Sleep / suspend / resume**: `deep` sleep + RTC/power-button wake verified; Wi-Fi, touch, sensors and USB NCM survive resume without replug
- [x] **Brightness control in the UI** (DBV 6-2047) + **auto-brightness** via `STK33915` (`iio-sensor-proxy`)
- [x] **Sensors**: SLPI sensor hub (`remoteproc_slpi` + `L2C`/`L9C` rails + `s22-hex-cb3` + `hexagonrpcd`) -> auto-rotate (`LSM6DSO`), proximity & ambient light (`STK33915`), compass (`AK09918`)
- [x] **Vibration / haptics (full CS40L26 Halo Core DSP)**: `cs40l26-full` on `i2c9 @ 0x40` with authentic Samsung `cs40l26.wmfw` (v39.2.22), 143 RAM waveforms (`cs40l26-wt3.bin`), F0/Redc calibration + `cs40l26-svc3.bin` + `cs40l26-a2h.bin`, 100% `FF_GAIN`, `FF_RUMBLE`/`FF_SINE` & custom PWLE waveforms (`s22-vibrate` + `feedbackd`)
- [x] **Cameras (all 4 sensors + thermal/power fix)**: front **Sony IMX374**, tele **Samsung S5K3K1**, ultrawide **Sony IMX563**, and 50MP wide **Samsung S5KGN3**; fixed `VBLANK_MIN` overdrive, capped full-res modes to 15 fps, enabled `2040x1532` 2x2 binned zero-copy DMABUF (`3.28 ms/frame`, GPU idles at `220 MHz`), single-frame async JPEG q92 capture
- [x] **Camera LED flash & flashlight**: `S2MPB02` FLED (`s2mpb02:white:flash` + `gpio87`) + `PM8350C` (`white:flash-0..3`), `s22-flash` CLI, **Plasma Mobile QuickSettings Flashlight** toggle (KAuth-free direct `s22-flash`), and `plasma-camera` strobe
- [x] **Fingerprint hardware bridge (`Qualcomm 3D Sonic Gen 2 QBT2608 / qcom,qbt2000`)**: Linux `qbt2000-r0q` driver exposes `/dev/qbt2000_ipc`, `qbt2000_key_input`, `VDD_BTP_1P8` (`0 mW` off in idle), `WUHB` ultrasonic finger-detect wake IRQ (`gpio89`), and `/sys/class/fingerprint/fingerprint/*` (ultrasonic pixel SPI & matching stay hardware-locked in QSEE `qbt1000` TA)
- [x] **NFC**: NXP `PN547/SN110` on `i2c17 @ 0x28` (`nxp-nci_i2c`, `nfc0`, `neard` + `nfctool` polling `Felica/MIFARE/Jewel/ISO-DEP/NFC-DEP/ISO-15693`; `0 mW` off in idle)
- [x] **USB-C**: host/OTG (VBUS, FAT/exFAT/ISO/UDF), USB-PD (5V/3A & 9V/2A), **USB 3.0 SuperSpeed (5 Gbps, CC1/CC2)**, and **DisplayPort Alt-Mode (`card1-DP-1`, SVDM `0xff01` Pin C/D/E)**
- [x] **Writable hardware RTC**: `pmk8350_rtc` wired to `pmk8350_sdam_21` (`rtc-offset@78`)
- [x] **120 Hz + Display extras (`panel-samsung-s6e3fac` + `s22-display-ctl`)**: VRR (10-120 Hz PHS/LFD + seamless 120/96/60/48/30/24 Hz), HBM (~1300 nits + sunlight auto-boost), mDNIe/IRC/OTP colour calibration, and HLPM Always-On Display

## Resolved tech debt & permanent hardware notes
- [x] **CDSP full 32 MiB + FastRPC userspace**: `cdsp_mem` restored to full 32 MiB (`0x2000000`) with `uniLoader` DTB relocation (`0xb6000000`), `fastrpc` (`libcdsprpc.so` + persistent `s22-cdsp-rpcd.service` via `hexagonrpcd` + `system_heap`), and read-only extracted `/usr/lib/dsp/{cdsp,adsp}` libraries
- [x] **Plasma Mobile**: UI scale (2.5x), nav bar, KWin GPU rendering on unified `msm` DRM `card0` (`startplasmamobile` + `s22-adreno.conf` drop-in hardened against `apk upgrade`, plus `deviceinfo_flash_kernel_on_update="false"` protecting `uniLoader`), plus **Flashlight**, **Always-On Display** & **Game 90Hz** QuickSettings
- [x] **Debug leftovers cleaned**: stale test `.ko.zst`/`.so`/`.log` and `.bak-phase*` module trees removed on phone; Phosh sessions hidden (Plasma Mobile is the sole session)
- [x] **Safe `reboot2` helper**: `reboot2` with no argument now prints `Usage: reboot2 <normal|recovery|bootloader|download>` and exits `2` instead of rebooting into TWRP (`reboot2 normal` reboots cleanly into Linux)
- [x] **Portrait panel orientation** (`DRM_MODE_PANEL_ORIENTATION_NORMAL`) wired in `panel-samsung-s6e3fac`
- [x] **USB plug/unplug detection & Type-C auto-connect**: MAX77705 IRQ pin `gpio5` `power-source = <1>` + `max77705-usbc` wired into `apply-kernel-patches.sh`
- [x] **MTP file transfer (PC + Android OTG hosts)**: `umtprd` configured with standard USB Still Image / MTP class (`06:01:01`, `interface "MTP"`, `s22-mtp.service`) next to NCM
- [x] **Runtime clock/PD gating verified**: all 16 unused GDSCs are `off-0` and all 11 unused `cam_cc`/`video_cc` PLLs are `N` (`0 mW`) at runtime; `dispcc=m` + `clk_ignore_unused pd_ignore_unused` are kept intentionally for `simple-framebuffer` -> `msm-mdss` boot handoff and RPMh `rf_clk1..4`; `qcrypto` stays blocked in `/etc/modprobe.d/s22-bringup.conf` (TZ xPU); update `msm-mdss` via module install + reboot rather than live unbind
- [x] Upstream patch finalized (`Signed-off-by` & `checkpatch.pl` verified): `kernel/upstream-patches/0001-PCI-qcom-Parse-iommu-map-entries-using-iommu-cells.patch`
