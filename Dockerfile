# Build environment for cpu_tweaks.skprx - vitasdk pinned to
# arm-vita-eabi-gcc 10.3.0 (known-good against this plugin's code; the
# vendored psp2spl splib was itself originally built with the same major
# version). Pinned by digest so this stays reproducible even if the
# upstream tag moves.
FROM atamanenko/vitasdk-softfp@sha256:fa66f683d397885ffec3dcccf915ca977c19e20fc895c2d7e9ce25201c39ecdd

WORKDIR /work
CMD ["bash", "-c", "cmake -B build -S . -DVITASDK=$VITASDK && cmake --build build && echo built: build/cpu_tweaks.skprx"]
