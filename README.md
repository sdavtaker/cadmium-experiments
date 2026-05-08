# Cadmium Experiments

Reproduction of the tick-counter study from VDW14 using the Cadmium C++17 DEVS simulator,
covering both the PDEVS and classic DEVS formalism variants.

## What this repository is about

Cadmium is the C++17 successor to CDBoost, introducing port-based models, CMake,
and structured logging.  These experiments run the same tick-counter model under two
formalisms to compare their behaviour when the TIME type cannot represent 1/10 exactly.

The reference experiment is the tick-counter from:

> Vicino, Dalle, Wainer. *A Data Type for Discretized Time Representation in DEVS.*
> SIMUTOOLS 2014. hal-01055555.

## Structure

```
cadmium-impl-notes.tex   Shared implementation notes (simulator conventions,
                         port system, TIME type, log format) included in the paper.
docs/
  main.tex               Root LaTeX document — compiles to main.pdf.
  main.pdf               Built paper.
vdw14-pdevs/
  spec.tex               Model definition for the PDEVS variant.
  main.cpp               Simulation driver.
  tick_gen.hpp           Tick generator (PDEVS, period 1/10 s).
  reset_gen.hpp          Reset generator (PDEVS, period 1 s).
  test_models.cpp        Unit tests.
  CMakeLists.txt
vdw14-devs/
  spec.tex               Model definition for the classic DEVS variant.
  main.cpp               Simulation driver.
  tick_gen.hpp           Tick generator (classic DEVS).
  reset_gen.hpp          Reset generator (classic DEVS).
  test_models.cpp        Unit tests.
  CMakeLists.txt
vcpkg.json               vcpkg dependency manifest.
CMakeLists.txt           Root build configuration.
```

## Building

Requires CMake, vcpkg, and a C++17-capable compiler.

```sh
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build -j2
ctest --test-dir build
```

Simulation executables write NDJSON logs to stdout.
See `simulators/cadmium/docs/log-format.md` for the log schema.
