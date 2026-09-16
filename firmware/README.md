# Firmware for the NP750XQA

`qcom/x1p42100/SAMSUNG/NP750XQA/` is what `/usr/lib/firmware` needs for this board.

| file | role | origin |
|---|---|---|
| `qcadsp8380.mbn` | audio DSP; also runs the battery and charger service | Samsung Windows driver package (`qcsubsys_ext_adsp8380.inf`) |
| `qccdsp8380.mbn` | compute DSP | Samsung Windows driver package (`qcnspmcdm_ext_cdsp8380.inf`) |
| `adsp_dtbs.elf`, `cdsp_dtbs.elf` | DSP device-tree blobs | same packages |
| `qcdxkmsucpurwa.mbn` | GPU zap shader; without it the GPU does not start | Samsung Windows driver package (`qcdx8380.inf`) |
| `*.jsn` | pd-mapper service descriptions | linux-firmware, `qcom/x1e80100/`, unchanged |

The five signed files are Qualcomm firmware signed for this board and distributed
by Samsung with Windows. They are not in linux-firmware and carry no licence that
covers redistribution; they are included so the port works without a Windows
install. If you have Windows, `scripts/extract-firmware.sh` pulls the same files
from it and checks them against `SHA256SUMS`.

Not included: the Wi-Fi board file. `scripts/make-board-2.sh` rebuilds it from
linux-firmware in a second, and its licence forbids redistributing the modified copy.
