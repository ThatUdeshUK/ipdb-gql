PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckgql
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# iPDB build defaults. Each one can be overridden on the command line or from
# the environment, e.g. `make release GEN= IPDB_FLAGS=`.
GEN ?= ninja
VCPKG_TOOLCHAIN_PATH ?= ${PROJ_DIR}vcpkg/scripts/buildsystems/vcpkg.cmake
IPDB_FLAGS ?= -DENABLE_PREDICT=1 -DENABLE_LLM_API=1
override EXT_FLAGS += ${IPDB_FLAGS}

# Bootstrap the repo-local vcpkg checkout on first build when it is the one in use.
ifeq (${VCPKG_TOOLCHAIN_PATH},${PROJ_DIR}vcpkg/scripts/buildsystems/vcpkg.cmake)
EXTENSION_CONFIG_STEP ?= vcpkg/scripts/buildsystems/vcpkg.cmake
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
include extension-ci-tools/makefiles/vcpkg.Makefile

# Only test/sql contains the promoted, release-gating SQLLogicTests.
# test/features also contains generated conformance candidates that are
# intentionally allowed to fail until they are reviewed and promoted.
TESTS_BASE_DIRECTORY = "test/sql/"
