# Clipper2

The offsetting and clipping core of [Clipper2](https://github.com/AngusJohnson/Clipper2) by Angus
Johnson, release `Clipper2_2.0.1`, under the Boost Software License 1.0 (see `LICENSE`).

These files are the release's `CPP/Clipper2Lib`, unchanged, without the source of the
triangulation and the export layer, which LCAD does not use. `clipper.version.h` is the one that release ships.

It is kept here and not taken from the systems LCAD builds on because Ubuntu, conda-forge, and the
Flatpak runtime do not package it, and because what a machine is told to do should not change with
the distribution. To update it, replace these files from a newer release and run the tests in
`tests/cam`, which look at the toolpaths themselves.

The source archive of this release has the SHA-256 checksum
`2a3693aceab4aed3e39b743e038d87701acc53cf05ed7b2013aab3e0aec5287e`.
