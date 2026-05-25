# Cadmium Experiments [![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/sdavtaker/cadmium-experiments/badge)](https://scorecard.dev/viewer/?uri=github.com/sdavtaker/cadmium-experiments)

A collection of experiments using the Cadmium simulator.

## Structure

```
cadmium-impl-notes.tex   Shared implementation notes (simulator conventions,
                         port system, TIME type, log format) included in the paper.
docs/
  main.tex               Root LaTeX document — compiles to main.pdf.
  main.pdf               Built paper.
vdw14-pdevs/             Experiment reproducing the tick-counter from VDW14, PDEVS variant.
  spec.tex               Model definition and expected observations.
  main.cpp               Simulation driver.
  tick_gen.hpp           Periodic tick generator (period 1/10 s).
  reset_gen.hpp          Periodic reset generator (period 1 s).
  test_models.cpp        Unit tests for the atomic models.
  CMakeLists.txt         Build rules for the experiment.
vdw14-devs/              Experiment reproducing the tick-counter from VDW14, classic DEVS variant.
  spec.tex               Model definition and expected observations.
  main.cpp               Simulation driver.
  tick_gen.hpp           Periodic tick generator (period 1/10 s).
  reset_gen.hpp          Periodic reset generator (period 1 s).
  test_models.cpp        Unit tests for the atomic models.
  CMakeLists.txt         Build rules for the experiment.
vcpkg.json               vcpkg dependency manifest.
CMakeLists.txt           Root build configuration.
```

## Building

Requires CMake and vcpkg.

```sh
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build -j2
ctest --test-dir build
```

Simulation executables write NDJSON logs to stdout.
See `simulators/cadmium/docs/log-format.md` for the log schema.
