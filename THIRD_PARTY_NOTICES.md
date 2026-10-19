# Third-party notices

## Linux UHID backend

The trigger mechanism feedback in `src/platform/linux/UhidDevice.cpp`, USB
report descriptor and firmware identity are adapted from the patched VIIPER
v0.7.0 backend previously distributed by the original project:

- VIIPER: https://github.com/Alia5/VIIPER
- Source patch: https://github.com/ReynArts/ApexSenseBridge/blob/1adf78dacbfcc6eb6f2f41ae7cc8060962e148b6/third_party/viiper-patches/viiper-v0.7.0-asb.patch
- Author: Alia5
- License: GPL-3.0; complete text in `packaging/linux/VIIPER-LICENSE.txt`.

The adapted UHID implementation and combined Linux executable are provided
under GPL-3.0; existing MIT source files retain their licenses. The Linux
source package includes the complete source used to build its executable.
No VIIPER server, USBip, HidHide, Windows installer or Playnite component is
included or required by this Linux repository.

## OpenFlydigi

The Flydigi identity request/reply layout, Apex 5 DeviceType allowlist, and
related protocol behavior, and the DualSense adaptive-trigger translation in
`src/dualsense/AdaptiveTriggerTranslation.*`, were ported from and
validated against OpenFlydigi:

- Project: https://github.com/mkaliaha/openflydigi
- Reference commit: `8477300f1bd0cdd0e4a277a544aa9b151c623e62`
- Reference files: `flydigi/device.py`, `flydigi/identity.py`,
  `flydigi/motion.py`, `flydigi/effects.py`, `flydigi/ds5.py`,
  `flydigi/relay.py`, and `PROTOCOL.md`
- Copyright: 2026 Mikalai Kaliaha
- License for the referenced implementation: MIT

MIT License

Copyright (c) 2026 Mikalai Kaliaha

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
## SteamlessController DualSense fork

The shared DualSense feedback/input types originate in upstream code adapted
from the SteamlessController DualSense fork. The Linux bridge uses native UHID
reports; the Windows sidecar lifecycle and VIIPER transport serializers have
been removed. Attribution and the applicable MIT notice are retained here:

- Project: https://github.com/david419kr/steamless-controller-XB-PS-NS
- Reference commit: `e19e874e00938eff2ee28d1a928dc2ab58bf9b75`
- Reference files: `src/app/ViiperClient.*`, `src/app/VirtualControllerTypes.*`,
  `src/steam/SteamController.cpp`, and
  `third_party/viiper-patches/viiper-v0.7.0-asb.patch`
- Copyright: 2026 Dylan Deverill; 2026 david419kr
- License for the referenced main application implementation: MIT

MIT License

Copyright (c) 2026 Dylan Deverill
Copyright (c) 2026 david419kr

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
