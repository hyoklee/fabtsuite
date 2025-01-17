![logo](fabtsuite_logo.png) 
# fabtsuite: a libfabric test suite
[![check spelling](https://github.com/mercury-hpc/fabtsuite/actions/workflows/spell.yml/badge.svg)](https://github.com/mercury-hpc/fabtsuite/actions/workflows/spell.yml)
[![cmake fabtsuite](https://github.com/mercury-hpc/fabtsuite/actions/workflows/cmake.yml/badge.svg)](https://github.com/mercury-hpc/fabtsuite/actions/workflows/cmake.yml)
[![spack fabtsuite](https://github.com/mercury-hpc/fabtsuite/actions/workflows/spack.yml/badge.svg)](https://github.com/mercury-hpc/fabtsuite/actions/workflows/spack.yml)
[![test latest libfabric](https://github.com/mercury-hpc/fabtsuite/actions/workflows/fabric.yml/badge.svg)](https://github.com/mercury-hpc/fabtsuite/actions/workflows/fabric.yml)

This is a test suite for libfabric designed to exercise features
having particular interest for the Mochi project.

# Project contents

The major constituents of this project are the test program under
[transfer/](transfer/), a test script under [scripts/](scripts/), and the project documentation
under [doc/](doc/).

The test program, which lives under [transfer/](transfer/), assumes either
a server or client personality, depending on the name by which
it is invoked.  Invoked as `fabtget`, it is the test server, and as `fabtput`,
the test client.

[scripts/fabtrun](scripts/fabtrun) is the main test script.

[doc/building.md](doc/building.md), 
[doc/building_cmake.md](doc/building_cmake.md), 
and [doc/building_spack.md](doc/building_spack.md)
tell how to build the project.

[doc/tests.md](doc/tests.md) describes the tests in this suite.
[doc/tests_mn.md](doc/tests_mn.md) describes how to run test using multiple nodes.
