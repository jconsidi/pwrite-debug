# Demonstrates APFS losing one pwrite's bytes in a 4 KiB block shared with a
# concurrent pwrite. See the header of pwrite_minimal.cpp for the details.
#
#   make test                          run the demo on ./pwrite_test.tmp
#   make test TEST_FILE=/Volumes/x/t   use another APFS path
#   make test TRIALS=10                fewer trials, less disk (~0.26 GB each)
#   make test CXX=g++-16               build with another compiler

ifeq ($(origin CXX),default)
CXX = clang++
endif
CXXFLAGS ?= -O2 -std=c++20 -pthread

TEST_FILE ?= ./pwrite_test.tmp
TRIALS ?= 50
A_MB ?= 256
SPLIT ?= 2048
B_BYTES ?= 2048
DELAY_MS ?= 5

ARGS = $(TEST_FILE) $(TRIALS) $(A_MB) $(SPLIT) $(B_BYTES)

.PHONY: all test clean

all: pwrite_minimal

pwrite_minimal: pwrite_minimal.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# Each run's full output goes to test-<variant>.log; only the summary line is
# shown. pwrite_minimal exits 0 when nothing was lost, 1 when data was lost
# and 2 on an error, so only an error fails the target.
#
# Expected on an affected system: the two "bug" runs lose B's bytes in nearly
# every trial with B inside A, and the two controls lose nothing.
test: pwrite_minimal
	@echo "macOS $$(sw_vers -productVersion 2>/dev/null), $$(uname -m)"
	@diskutil info "$$(df $(dir $(TEST_FILE)) | tail -1 | awk '{print $$NF}')" 2>/dev/null \
		| grep -i 'File System Personality' || true
	@echo
	@lost=0; \
	for v in nocache:bug msync:bug none:control fsync:control; do \
		mode=$${v%%:*}; kind=$${v##*:}; \
		printf '== %-7s (%s) ==\n' $$mode $$kind; \
		PWRITE_MINIMAL_B_AFTER=$$mode ./pwrite_minimal $(ARGS) inside $(DELAY_MS) \
			> test-$$mode.log 2>&1; rc=$$?; \
		grep -v '^  trial' test-$$mode.log; \
		case $$rc in \
			0) echo "-> no data lost";; \
			1) echo "-> DATA LOST (per-trial details in test-$$mode.log)"; \
			   [ $$kind = bug ] && lost=$$((lost + 1));; \
			*) echo "-> error, exit $$rc"; exit 2;; \
		esac; \
		echo; \
	done; \
	if [ $$lost -gt 0 ]; then \
		echo "RESULT: bug reproduced in $$lost of 2 variants."; \
	else \
		echo "RESULT: bug not reproduced. If few trials ran B inside A, try A_MB=1024."; \
	fi

clean:
	rm -f pwrite_minimal test-*.log $(TEST_FILE)
