# Test-only root entry point exercising project.mk's controlled sub-make.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
include $(ROOT)/mk/project.mk

.PHONY: canary-contract-probe
canary-contract-probe:
	@printf '%s\n' 'ROOT_KERNEL_BUILD_DIR=$(KERNEL_BUILD_DIR)'
	@$(call os01_submake,kernel,ARCH=x86_64 print-canary-contract $(OS01_SUBMAKE_ARGS))
