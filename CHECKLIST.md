# S22 Checklist (Linux Install)
tags: #s22 #checklist
Updated 2026-11-18 · kernel 7.2.11

## ✅ Working
- [x] Boots postmarketOS from internal storage (~35 s)
- [x] USB 3.0 SuperSpeed (5 Gbps, CC1/CC2 orientation) + USB 2.0 fallback + SSH at boot, no replug ([[USB-C]])
- [x] USB plug/unplug detection (attempt 28)
- [x] **MTP file transfer to PC & Android OTG phones**: `umtprd` over FunctionFS configured with standard USB Still Image / MTP class (`06:01:01`, `interface "MTP"`, `s22-mtp.service`) alongside USB NCM
- [x] Wi-Fi 2.4 GHz + 5 GHz, autoconnects at boot with PCIe ASPM `powersave` ([[Wi-Fi]])
- [x] Bluetooth
- [x] Bluetooth audio in (A2DP sink): phone plays music streamed from another device (2026-10-21)
- [x] Bluetooth audio out: connects to a car stereo, clear audio (2026-10-21)
- [x] Display 120/96/60/48/30/24 Hz + **Dynamic VRR (10-120 Hz PHS/LFD & seamless switch), HBM (~1300 nits + auto-lux), hardware mDNIe/IRC/OTP colour calibration & Always-On Display (HLPM 2/10/30/60 nit)**, touch, GPU ([[Display]]); CPU frequency scaling + PSCI idle states verified
- [x] **KWin on Adreno 730**: session (`startplasmamobile` + `s22-adreno.conf` drop-in) unsets `EGL_PLATFORM=wayland`, sets `KWIN_DRM_DEVICES=/dev/dri/card0`, and waits for `/dev/dri/card0` + `renderD128` before starting; `freedreno/FD730`, no `llvmpipe` + `msm-w10` & Mesa 26.3.0 Turnip/Gallium Wave 5 optimizations
- [x] **Wayland socket discovery** (`wayland-[0-9]`) in `s22-display-ctl` / `s22-display-extrasd`; 120↔60 switching works through the helper
- [x] **Stereo speakers + handset Earpiece + 2 mics + VoiceCall UCM2 + CS35L41 Halo DSP speaker protection** ([[Audio]]): `cirrus,subsystem-id = "r0q"` loads authentic Samsung tunings (`R0_SPKPV2_PPsoff_20211022.bin` bottom + `R0_RCVPV2_PPsoff_20211020.bin` top), calibrated via `s22-speaker-prot` (~+5 dB louder safe output)
- [x] Power, volume down, volume up buttons
- [x] **On-screen keyboard + auto-hide**: `plasma-keyboard` + `s22-kbd-autohide.service` (hides KWin `VirtualKeyboard` when tapping above the keyboard panel and re-arms automatically)
- [x] **Charging + battery % + charge limit control**: accurate 2 mOhm fuel gauge (`3,700 mAh`), USB-C Rp 3.0 A `CHGIN_ILIM`, `MAX77705_AICL_I` 345 Hz IRQ storm masked (`0 IRQs/s`), and **`charge_control_end_threshold` (`50-100%`)** + `s22-charge-limit` (holds MAX77705 in Buck-only USB power-path at limit)
- [x] RAM 7.0 GiB (7,161 MiB) usable (+854 MiB reclaimed from Android-only carve-outs; full 32 MiB `cdsp_mem`, modem/camera/CVP regions kept)
- [x] Firewall (nftables)
- [x] Apps: Google Chrome, QMLKonsole, Discover + Flathub, KRecorder, Dolphin ([[Software]])
- [x] USB OTG (FAT/exFAT/ISO/UDF) + USB-PD (5V/9V) + DisplayPort Alt-Mode out (`card1-DP-1`, SVDM `0xff01` Pin C/D/E, up to HBR3)
- [x] **CDSP (NPU processor) & FastRPC userspace**: full 32 MiB `cdsp_mem`, `fastrpc` (`libcdsprpc.so` + persistent `s22-cdsp-rpcd.service` via `hexagonrpcd` + `system_heap`), and read-only extracted Samsung `/usr/lib/dsp/{cdsp,adsp}` skeleton libraries (persistent daemon eliminates cold-start skel race)
- [x] **Vulkan (Turnip Wave 5) on Adreno 730**
- [x] **Camera subsystem (CAMSS) & S2MPB02 PMIC**: 6 CSIPHY, 5 CSID, 5 VFE, 17 video nodes, 21 S2MPB02 regulators (`L1CTRL = 0x1e`)
- [x] **All 4 cameras + thermal/power optimization**: front **Sony IMX374**, 3x tele **Samsung S5K3K1**, ultrawide **Sony IMX563**, and 50MP wide **Samsung S5KGN3**; fixed `VBLANK_MIN` overdrive, capped full-res modes to 15 fps, enabled `2040x1532` 2x2 binned zero-copy DMABUF (`3.28 ms/frame` vs `18.77 ms/frame`, GPU idles at `220 MHz`), single-frame async JPEG q92 capture
- [x] **Camera LED flash & flashlight**: `S2MPB02` FLED (`s2mpb02:white:flash` + `gpio87`) + `PM8350C` (`white:flash-0..3`), `s22-flash` CLI, **Plasma Mobile QuickSettings Flashlight toggle** (KAuth-free direct `s22-flash`), and `plasma-camera` flash strobe
- [x] **Vibration (full CS40L26 Halo Core DSP)**: `cs40l26-full` with authentic Samsung `cs40l26.wmfw` (v39.2.22), **143 RAM waveforms** (`cs40l26-wt3.bin`), F0/Redc calibration + `cs40l26-svc3.bin` + `cs40l26-a2h.bin`, 100% `FF_GAIN`, `FF_RUMBLE`/`FF_SINE` & custom PWLE waveforms (`s22-vibrate`)
- [x] **NFC**: NXP `PN547/SN110` on `i2c17 @ 0x28` (`nxp-nci_i2c`, `nfc0`, `neard` + `nfctool` polling `Felica/MIFARE/Jewel/ISO-DEP/NFC-DEP/ISO-15693`; powered off `0 mW` in idle)
- [x] Brightness slider (DBV 6-2047) + auto-brightness via `STK33915`
- [x] **Memory bandwidth voting & runtime PM gating**: CPU + DDR bwmon + GPU votes idle at lowest step; all 16 unused GDSCs (`mdss`, `gpu`, `titan_top`, `video`, `pcie_1`, `mss`, `mxc`, `mx`, `lmx`, `lcx`, `gfx`, `ebi`) are `off-0` and all 11 unused `cam_cc`/`video_cc` PLLs are gated off (`N`) in idle
- [x] **Idle & active battery**: `MAX77705_AICL_I` masked + `chgin-irq` debounced + PCIe ASPM `powersave` (`s22-power-save.service`); **ADSP Q6PRM `LPASS_HW_{MACRO,DCODEC}` runtime PM fix** (ADSP sleep `0.017%` -> `100%` idle); **screen-off `fts2ba61y` `sense_enable` gating + `s22-display-extrasd` netlink uevent sync**; removed 2.5 s `plasmashell` flashlight polling timer; rebuilt/flashed `uniLoader` `boot.img` with **`ufs-qcom` lane clk/ICC fix (`clk_enable_count=0` suspended)**, **`dwc3-qcom` `USB_ROLE_NONE` DDR ICC gating**, **`llcc_bwmon`/`cpu_bwmon` 200 MHz DDR OPPs**, **Adreno 730 GPU 818 MHz idle/wake devfreq unlock + 66 ms autosuspend + Cortex-A710 IRQ affinity**, and demand-gated `s2mpb02-l6/l7/l9/l10` camera VIO LDOs
- [x] **Sensors**: SLPI sensor hub (`remoteproc_slpi` + `L2C`/`L9C` + `s22-hex-cb3` + `hexagonrpcd` + `iio-sensor-proxy`): auto-rotate (`LSM6DSO`), ALS & proximity (`STK33915`), compass (`AK09918`)
- [x] **Writable hardware RTC**: `pmk8350_rtc` wired to `pmk8350_sdam_21` (`rtc-offset@78`)
- [x] **Suspend/resume**: `deep` sleep + RTC/power-button wake verified; Wi-Fi, touch, sensors and USB NCM survive resume without replug
- [x] **Cellular modem + SM8450 IPA v5.1 (`qcom,sm8450-ipa`)**: auto-starts at boot (`s22-modem.service` + read-only `rmtfs`), exposes `rmnet_ipa0` + `qrtr0` + `wwan0at0` to **ModemManager (`qcom-soc`)**, Dual-SIM USIM, VoLTE (`81voltd`), VoiceCall UCM2 & GNSS ([[Modem]])

## 🟡 Hardware / TrustZone Boundary Notes
- [x] **Fingerprint hardware bridge (`Qualcomm 3D Sonic Gen 2 QBT2608 / qcom,qbt2000`)**: Linux GPIO/IRQ/regulator driver (`qbt2000-r0q.ko`) brings up `/dev/qbt2000_ipc`, `qbt2000_key_input`, `VDD_BTP_1P8` (`0 mW` off in idle), `WUHB` ultrasonic finger-detect wake IRQ (`gpio89`), and `/sys/class/fingerprint/fingerprint/*`; ultrasonic pixel SPI & biometric matching are hardware-isolated by xPU inside Qualcomm TrustZone (`QSEE` `qbt1000` TA, no open `fprintd` driver)
- [x] **Boot handoff flags (`clk_ignore_unused pd_ignore_unused`)**: verified via `pm_genpd_summary` and `clk_summary` that all 16 unused GDSCs and 11 unused multimedia PLLs gate off at runtime; boot flags are intentionally kept because `late_initcall_sync` runs before loadable `dispcc`/`msm` modules bind and protects RPMh `rf_clk1..4`

## 📋 Housekeeping
- [x] Clean debug leftovers (stale test `.ko.zst`/`.so`/`.log` & backup module dirs cleaned on phone; safe no-arg guard added to `reboot2`)
- [x] Battery charge limit control (`charge_control_end_threshold` + `s22-charge-limit`)
- [x] Portrait panel orientation (`DRM_MODE_PANEL_ORIENTATION_NORMAL`) reported by S6E3FAC driver
- [x] Upstream `pcie-qcom` patch (`kernel/upstream-patches/0001-PCI-qcom-Parse-iommu-map-entries-using-iommu-cells.patch` finalized with author/Signed-off-by & `checkpatch.pl` verified)
