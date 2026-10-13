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

They are also the sample files for format version 1. Keep them when the format changes: from then
on they test the migration to the newer version.

To write the file for the OCCT version of the current build:

```sh
cmake --build --preset dev --target lcad_write_reference_model
build/dev/bin/lcad_write_reference_model tests/models/format-1/primitives-occt-X.Y.Z.lcad
```

CI attaches the file its own build wrote to each run, as the artifact `reference-model-<job>`.
