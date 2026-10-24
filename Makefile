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

# Each variant is NAME:MODE:ORDER:SPLIT:KIND, where MODE is the
# PWRITE_MINIMAL_B_AFTER value, ORDER and SPLIT are pwrite_minimal's
# arguments, and KIND says what an affected system should show:
#   bug      B's page leaves the cache while A runs: B's bytes are lost
#   control  one condition removed: nothing is lost
#     none, fsync      B's page stays in the cache
#     nocache-before   B finishes before A starts
#     msync-aligned    B starts on a 4 KiB boundary, so no block is shared
#                      (msync, since an aligned F_NOCACHE pwrite waits for A
#                      to finish, so B would rarely run inside A)
VARIANTS = \
	nocache:nocache:inside:$(SPLIT):bug \
	msync:msync:inside:$(SPLIT):bug \
	msync-inval:msync_inval:inside:$(SPLIT):bug \
	none:none:inside:$(SPLIT):control \
	fsync:fsync:inside:$(SPLIT):control \
	nocache-before:nocache:before:$(SPLIT):control \
	msync-aligned:msync:inside:0:control

# Each run's full output goes to test-<name>.log; only the summary line is
# shown. pwrite_minimal exits 0 when nothing was lost, 1 when data was lost
# and 2 on an error, so only an error fails the target.
test: pwrite_minimal
	@echo "macOS $$(sw_vers -productVersion 2>/dev/null), $$(uname -m)"
	@diskutil info "$$(df $(dir $(TEST_FILE)) | tail -1 | awk '{print $$NF}')" 2>/dev/null \
		| grep -i 'File System Personality' || true
	@echo
	@bugs=0; bug_lost=0; controls=0; control_lost=0; \
	for v in $(VARIANTS); do \
		old_ifs=$$IFS; IFS=:; set -- $$v; IFS=$$old_ifs; \
		name=$$1; mode=$$2; order=$$3; split=$$4; kind=$$5; \
		printf '== %s (%s) ==\n' $$name $$kind; \
		PWRITE_MINIMAL_B_AFTER=$$mode ./pwrite_minimal $(TEST_FILE) $(TRIALS) $(A_MB) \
			$$split $(B_BYTES) $$order $(DELAY_MS) > test-$$name.log 2>&1; rc=$$?; \
		grep -v '^  trial' test-$$name.log; \
		case $$rc in \
			0) echo "-> no data lost";; \
			1) echo "-> DATA LOST (per-trial details in test-$$name.log)";; \
			*) echo "-> error, exit $$rc"; exit 2;; \
		esac; \
		if [ $$kind = bug ]; then \
			bugs=$$((bugs + 1)); [ $$rc = 1 ] && bug_lost=$$((bug_lost + 1)); \
		else \
			controls=$$((controls + 1)); [ $$rc = 1 ] && control_lost=$$((control_lost + 1)); \
		fi; \
		echo; \
	done; \
	echo "RESULT: data lost in $$bug_lost of $$bugs bug variants and $$control_lost of $$controls controls."; \
	if [ $$bug_lost -gt 0 ] && [ $$control_lost = 0 ]; then \
		echo "Bug reproduced: B's bytes are lost only when its page leaves the cache while A runs."; \
	elif [ $$bug_lost = 0 ]; then \
		echo "Bug not reproduced. If few trials ran B inside A, try A_MB=1024."; \
	else \
		echo "A control lost data too, which this demo doesn't expect; see its log."; \
	fi

clean:
	rm -f pwrite_minimal test-*.log $(TEST_FILE)
