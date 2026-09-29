# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# SpiderMonkey PoC flow.
# One front door for the whole PoC. Each stage is a target you can run standalone
# or via `make all`; every stage prints PASS / FAIL / NO-SIGNAL and exits so it is
# CI-friendly. Stages map to the five PoC requirements:
#
#   1. abi      trainer/callee stack ABI == gadget stack reads   (abi_check.sh)
#   2. tune     auto-probe nr_loads for a page-aligned, max-pool  (tools/tune_target.py)
#      target  ... same, then DEPLOY to wasm/                    (--deploy)
#   3. churn    no mid-loop JIT compiles                          (churn_check.sh)
#      bailouts no steady-state Ion bailouts                      (bailouts_check.sh)
#   4. reuse    trainers GC'd, target reuses their pages          (reuse_check.sh)
#   5. btb      end-to-end BTB control -> F+R signal              (btb_check.sh)
#
# Alongside them, run standalone before stage 5 on a new host:
#   frcalib     measure this host's F+R hit/miss reload times     (fr_calib.sh)
#               and report the --fr-threshold they support
#
# Requires $JS_CMD (the Firefox 143 debug js shell) in the environment or on the
# command line, gdb, wat2wasm, nasm, python3.
#
# Usage:
#   export JS_CMD=/path/to/obj-debug-x86_64-.../dist/bin/js
#   make abi
#   make tune PAGES=8
#   make target PAGES=8                 # tune + deploy to wasm/
#   make churn UARCH=zen4
#   make all UARCH=zen4 PAGES=8
#
# UARCH is one of the common.sh keys: a76 x3 zen4 raptorcove lioncove.

# ---- knobs ------------------------------------------------------------------
UARCH       ?=
PAGES       ?= 2
ARCH        ?= x86_64
NRFUNC      ?= 1
CHURN_REPS  ?= 20
BAIL_REPS   ?= 50
WARMUP      ?= 10
BTB_REPS    ?= 100
CNT_MIN     ?= 1
SAMPLES     ?= 20000
MIN_GAP     ?= 20
export JS_CMD

# The stages `all` runs, in requirement order. `all` uses `tune` (non-mutating),
# not `target`, so a full run never rewrites wasm/. frcalib stays out of it: it
# is a per-host measurement you run and read yourself.
STAGES = abi tune churn bailouts reuse btb

.PHONY: help all abi tune target churn bailouts reuse btb frcalib check-uarch

help:
	@echo "BTR SpiderMonkey PoC flow -- targets:"
	@echo "  make abi      UARCH=<u>      stage 1: stack ABI (callee == gadget)"
	@echo "  make tune   [PAGES=2 ARCH=x86_64 NRFUNC=1]  stage 2: auto-probe geometry (verify)"
	@echo "  make target [PAGES=2 ARCH=x86_64 NRFUNC=1]  stage 2: auto-probe + DEPLOY to wasm/"
	@echo "  make churn    UARCH=<u>      stage 3a: no mid-loop JIT churn"
	@echo "  make bailouts UARCH=<u>      stage 3b: no steady-state bailouts"
	@echo "  make reuse    UARCH=<u>      stage 4: GC reclaim + page reuse"
	@echo "  make btb      UARCH=<u>      stage 5: BTB control -> F+R signal"
	@echo "  make all      UARCH=<u>      run every stage, print a summary"
	@echo "  make frcalib  UARCH=<u>      standalone: calibrate F+R hit/miss timing"
	@echo "  UARCH in: a76 x3 zen4 raptorcove lioncove   (needs \$$JS_CMD set)"

check-uarch:
	@[ -n "$(UARCH)" ] || { echo "error: set UARCH=<a76|x3|zen4|raptorcove|lioncove>"; exit 2; }

# ---- stage 1: ABI -----------------------------------------------------------
abi: check-uarch
	@./abi_check.sh $(UARCH)

# ---- stage 2: build target --------------------------------------------------
tune:
	@python tools/tune_target.py --arch $(ARCH) --pages $(PAGES) $(if $(NRFUNC),--nr-func $(NRFUNC))

target:
	@python tools/tune_target.py --arch $(ARCH) --pages $(PAGES) $(if $(NRFUNC),--nr-func $(NRFUNC)) --deploy

# ---- stage 3: churn + bailouts ----------------------------------------------
churn: check-uarch
	@NR_REPEATS=$(CHURN_REPS) ./churn_check.sh $(UARCH)

bailouts: check-uarch
	@NR_REPEATS=$(BAIL_REPS) WARMUP=$(WARMUP) ./bailouts_check.sh $(UARCH)

# ---- stage 4: GC reclaim / reuse --------------------------------------------
reuse: check-uarch
	@./inspect_reuse.sh $(UARCH)

# ---- stage 5: BTB control ---------------------------------------------------
btb: check-uarch
	@NR_REPEATS=$(BTB_REPS) CNT_MIN=$(CNT_MIN) ./btb_check.sh $(UARCH)

# ---- F+R timing calibration (standalone, run before stage 5) ----------------
frcalib: check-uarch
	@SAMPLES=$(SAMPLES) MIN_GAP=$(MIN_GAP) ./fr_calib.sh $(UARCH)

# ---- everything, with a summary (never aborts mid-way) ----------------------
all: check-uarch
	@bash -c 'set -u; \
	  stages="$(STAGES)"; \
	  declare -A res; overall=0; \
	  for s in $$stages; do \
	    echo; echo "######## stage: $$s ########"; \
	    if $(MAKE) --no-print-directory $$s; then res[$$s]=PASS; else res[$$s]=FAIL; overall=1; fi; \
	  done; \
	  echo; echo "######## PoC flow summary (UARCH=$(UARCH) PAGES=$(PAGES)) ########"; \
	  for s in $$stages; do printf "  %-9s %s\n" "$$s" "$${res[$$s]}"; done; \
	  echo "  (btb FAIL on a debug host = NO-SIGNAL, expected; needs real hw + opt build)"; \
	  exit $$overall'
