# Package fixtures (copied from AkiTools)

A copy of AkiTools `schemas/fixtures` (aki-roadmap#154): one valid package per kind and broken ones, with each one's
verdict in `expected.json`. `packages_tests.cpp` checks that the launcher refuses exactly the ones with `"exit": 2`,
the same packages `aki-package.py validate` refuses. Don't edit them here: change
`AkiTools/tests/make_package_fixtures.py`, run it, and copy `schemas/fixtures` over this folder (keeping this file).

Copied from AkiTools main e3d6d10.
