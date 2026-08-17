SHELL := /bin/bash

.DEFAULT_GOAL := help

ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SCRIPTS_DIR := $(ROOT_DIR)/Scripts

MODEL ?= $(LOCALSCRIBE_MODEL_PATH)
WAV ?= $(LOCALSCRIBE_WAV_PATH)
SOAK_ARGS ?=
PACKAGE_ARGS ?=
RELEASE_ARGS ?=

.PHONY: \
	help \
	build build-debug build-release \
	verify check check-scope \
	test test-core check-swift test-swift \
	soak soak-smoke soak-accelerated \
	whisper-smoke \
	package package-overwrite \
	github-release github-release-overwrite github-release-skip-verify \
	github-release-skip-verify-overwrite

help: ## Show the available project commands.
	@printf '%s\n' \
		'LocalScribe project commands' \
		'' \
		'Build:' \
		'  make build                              Build the release app bundle' \
		'  make build-debug                        Build the debug app bundle' \
		'  make build-release                      Build the release app bundle' \
		'' \
		'Checks and tests:' \
		'  make verify                             Run the complete local MVP gate' \
		'  make check-scope                        Verify platform and privacy boundaries' \
		'  make test                               Run Core tests, Swift checks, and XCTest' \
		'  make test-core                          Run the portable C++ contract tests' \
		'  make check-swift                        Run the standalone Swift checks' \
		'  make test-swift                         Run Swift XCTest (full Xcode required)' \
		'' \
		'Soak and ASR:' \
		'  make soak-smoke                         Run the short deterministic soak probe' \
		'  make soak-accelerated                   Simulate two hours at 1000x speed' \
		'  make soak                               Run the two-hour real-time core soak' \
		'  make soak SOAK_ARGS="..."               Pass custom arguments to the soak runner' \
		'  make whisper-smoke MODEL=... WAV=...     Run real Whisper ASR validation' \
		'' \
		'Packaging:' \
		'  make package                            Package the existing app bundle as a DMG' \
		'  make package-overwrite                  Replace same-version DMG artifacts' \
		'  make github-release                     Verify, build release, and package a DMG' \
		'  make github-release-overwrite           Rebuild same-version release artifacts' \
		'  make github-release-skip-verify         Build/package without the verification gate' \
		'  make github-release-skip-verify-overwrite' \
		'                                            Skip verification and replace artifacts' \
		'' \
		'See Docs/Development.ru.md for variables and detailed examples.'

build: build-release ## Build the release app bundle.

build-debug: ## Build and ad-hoc sign the debug app bundle.
	@"$(SCRIPTS_DIR)/build-app-bundle.sh" debug

build-release: ## Build and ad-hoc sign the release app bundle.
	@"$(SCRIPTS_DIR)/build-app-bundle.sh" release

verify: ## Run the complete local MVP verification gate.
	@"$(SCRIPTS_DIR)/verify-mvp.sh"

check: verify ## Alias for the complete local MVP verification gate.

check-scope: ## Verify the macOS-only, offline product boundary.
	@"$(SCRIPTS_DIR)/verify-scope.sh"

test: test-core check-swift test-swift ## Run all standalone tests and checks.

test-core: ## Compile and run the portable C++ contract tests.
	@"$(SCRIPTS_DIR)/run-core-tests.sh"

check-swift: ## Compile and run the standalone Swift invariant checks.
	@"$(SCRIPTS_DIR)/run-swift-checks.sh"

test-swift: ## Run Swift XCTest; requires a full Xcode installation.
	@"$(SCRIPTS_DIR)/run-swift-tests.sh"

soak: ## Run the core soak runner; pass optional SOAK_ARGS.
	@"$(SCRIPTS_DIR)/run-core-soak.sh" $(SOAK_ARGS)

soak-smoke: ## Run the short deterministic core soak probe.
	@"$(SCRIPTS_DIR)/run-core-soak.sh" --smoke

soak-accelerated: ## Simulate the two-hour core soak at 1000x speed.
	@"$(SCRIPTS_DIR)/run-core-soak.sh" --duration-seconds 7200 --speed 1000

whisper-smoke: ## Validate real Whisper ASR with MODEL and WAV files.
	@if [[ -z "$(MODEL)" || -z "$(WAV)" ]]; then \
		echo 'usage: make whisper-smoke MODEL=/path/to/model.bin WAV=/path/to/input.wav' >&2; \
		exit 2; \
	fi
	@"$(SCRIPTS_DIR)/run-whisper-smoke.sh" "$(MODEL)" "$(WAV)"

package: ## Package the existing app bundle as a DMG; pass optional PACKAGE_ARGS.
	@"$(SCRIPTS_DIR)/package-app-dmg.sh" $(PACKAGE_ARGS)

package-overwrite: ## Package the app and replace same-version DMG artifacts.
	@"$(SCRIPTS_DIR)/package-app-dmg.sh" --overwrite

github-release: ## Verify, build the release app, and package GitHub artifacts.
	@"$(SCRIPTS_DIR)/build-github-release.sh" $(RELEASE_ARGS)

github-release-overwrite: ## Build release artifacts and replace the same version.
	@"$(SCRIPTS_DIR)/build-github-release.sh" --overwrite

github-release-skip-verify: ## Build release artifacts without the verification gate.
	@"$(SCRIPTS_DIR)/build-github-release.sh" --skip-verify

github-release-skip-verify-overwrite: ## Skip verification and replace release artifacts.
	@"$(SCRIPTS_DIR)/build-github-release.sh" --skip-verify --overwrite
