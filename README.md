# Branch Target Reuse (BTR) Artifacts

This repository contains the artifacts for the paper "Branch Target Reuse:
Practical Spectre-v2 Attacks in JIT Engines via Stale Branch Prediction Entries"
(accepted at ACM CCS 2026).

For more information about the BTR attack, see our [project-page](https://www.vusec.net/projects/btr) and our [paper](https://download.vusec.net/papers/btr_ccs26.pdf)!

```txt
btr
├─ uarch-experiments/   # uArch experiments code   (Section 5)
├─ attack-surface/      # Attack Surface Analysis  (Section 6)
│  ├─ linux-cbpf        # Linux cBPF experiments   (Section 6.1)
│  ├─ spidermonkey/     # SpiderMonkey experiments (Section 6.2)
│  ├─ graal-vm/         # GraalVM experiments      (Section 6.3)
├─ e2e-exploit/         # Linux cBPF E2E exploit   (Section 7)
├─ README.md            # You are here
```
