# Reference models

Files the test suite reads. What each must contain is stated in the test that reads it, as
closed-form values, not in a file beside the model.

## format-1/

`primitives-occt-<version>.lcad` is one document, written by LCAD built against that version of
Open CASCADE: a box, a cylinder, a sphere, a cone, and a torus, each in a different place.
`tests/document/ReferenceModel.h` builds it and `tests/document/ReferenceModelChecks.h` says what
a reader must find.

Every build reads all of these files (`ReferenceModelTest.cpp`). Because CI builds against OCCT
7.6 and 7.9, that proves a file saved with either opens with the other.

## step/

`bracket-occt-<version>.step` is one STEP file, written by LCAD's STEP export built against that
version of Open CASCADE: a plate with a hole and a pin, as two named parts.
`tests/document/ReferenceStep.h` builds it and says what an import must find, and every build
imports all of these files (`ReferenceModelTest.cpp`), which proves that a STEP file exported on
one distribution imports on another. The same program writes it, as a second argument:

```sh
build/dev/bin/lcad_write_reference_model primitives.lcad tests/models/step/bracket-occt-X.Y.Z.step
```

CI's `reference-model-<job>` artifacts hold the one each job wrote, as `bracket.step`.

## About format-1/

They are also the sample files for format version 1. Keep them when the format changes: from then
on they test the migration to the newer version.

To write the file for the OCCT version of the current build:

```sh
cmake --build --preset dev --target lcad_write_reference_model
build/dev/bin/lcad_write_reference_model tests/models/format-1/primitives-occt-X.Y.Z.lcad
```

CI attaches the file each of its jobs wrote to every run, as the artifact
`reference-model-<job>`. That is where `primitives-occt-7.9.3.lcad` came from (the Arch Linux
job); `primitives-occt-7.6.3.lcad` was written on Ubuntu 24.04.
