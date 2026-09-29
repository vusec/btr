# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

import fileinput
import re
import time
import json

REGEX = r'^\[engine\]\s*opt done\s*.*\|Tier\s*(.*?)\|.*\|CodeSize\s*(.*?)\|Addr\s*(.*?)\|.*\|Src\s*(.*?)\s.*$'
MAX_REOCCUR = 20
JIT_TIERS = 2 # assume there is only 2 tiers for simplicity

addr_last_occur = {}
distribution = {}
lineno_filtered = 0

def parse_line(line):
    m = re.match(REGEX, line)
    if m:
        compile_tier = m.group(1)
        code_size = int(m.group(2))
        addr = int(m.group(3), 16)
        src = m.group(4)
        return {
            'compile_tier': compile_tier,
            'code_size': code_size,
            'addr': addr,
            'src': src
        }
    return None

class LogLineParser:
    def __init__(self):
        self.regex = re.compile(REGEX)
        self.train_phase = False
        self.target_phase = False
        self.global_cnt_train_modules = 0
        self.global_tier_mapping = {}
        self.global_cnt_reused_inclu = 0
        self.global_cnt_reused_align = 0
        self.global_cnt_not_reused = 0
        self.iter_train_module_name = None
        self.iter_train_reused_inclu = False
        self.iter_train_reused_align = False
        self.module_sizes = {}
        self.iter_train_entry = None
        self.iter_target_entry = None

    def parse_line(self, line):
        m = self.regex.match(line)
        if m:
            compile_tier = m.group(1)
            code_size = int(m.group(2))
            addr = int(m.group(3), 16)
            src = m.group(4)
            return {
                'compile_tier': int(compile_tier),
                'code_size': code_size,
                'addr': addr,
                'src': src
            }
        return None
    
    def track_jit_size(self, entry):
        tier = entry['compile_tier']
        size = entry['code_size']
        mod = entry['src']
        
        if mod.startswith('train'):
            mod = 'train'
        elif mod.startswith('target'):
            mod = 'target'
        else:
            print(f'Warning: Unrecognized module prefix in {mod}, skipping size tracking')
            return
        label = f'{mod}_tier{tier}'
        
        if label not in self.module_sizes:
            self.module_sizes[label] = {}
        if size not in self.module_sizes[label]:
            self.module_sizes[label][size] = 0
        self.module_sizes[label][size] += 1

    def track_reuse_reg_training(self, entry):
        if entry['src'].endswith('_0:1'):
            self.iter_train_entry = entry

    def track_reuse_reg_target(self, entry):
        if entry['src'].endswith('_0:1'):
            self.iter_target_entry = entry

    def print_iter_stats(self):
        if self.iter_train_entry is None or self.iter_target_entry is None:
            print('Warning: Missing training or target entry for reuse tracking')
            return
        train_addr = hex(self.iter_train_entry['addr'])
        target_addr = hex(self.iter_target_entry['addr'])
        target_size = hex(self.iter_target_entry['code_size'])
        print(f'{self.iter_train_module_name} => [{train_addr}] ~ [{target_addr}~{target_size}]', end='')
        print(f'i={self.iter_train_reused_inclu} a={self.iter_train_reused_align}', end='\n')

    def track_reuse_on_training_start(self, entry):
        self.train_phase = True
        self.iter_train_module_name = entry['src']
        self.global_cnt_train_modules += 1
        self.track_reuse_reg_training(entry)

    def track_reuse_on_training_continue(self, entry):
        self.track_reuse_reg_training(entry)

    def track_reuse_on_training_end(self, entry):
        self.train_phase = False

    def track_reuse_on_target_start(self, entry):
        self.target_phase = True
        self.track_reuse_reg_target(entry)

    def track_reuse_on_target_continue(self, entry):
        self.track_reuse_reg_target(entry)

    def track_reuse_on_target_end(self, entry):
        self.target_phase = False
        if self.iter_train_entry is None or self.iter_target_entry is None:
            print('Warning: Missing training or target entry for reuse tracking')
            return
        train_base = self.iter_train_entry['addr']
        target_base = self.iter_target_entry['addr']
        target_size = self.iter_target_entry['code_size']
        if target_base < train_base < target_base + target_size:
            self.iter_train_reused_inclu = True
        if target_base == train_base:
            self.iter_train_reused_align = True
        self.print_iter_stats()
        self.global_cnt_reused_inclu += 1 if self.iter_train_reused_inclu else 0
        self.global_cnt_reused_align += 1 if self.iter_train_reused_align else 0
        self.global_cnt_not_reused += 1 if (not self.iter_train_reused_inclu and not self.iter_train_reused_align) else 0
        self.iter_train_reused_align = False
        self.iter_train_reused_inclu = False

    def track_reuse(self, entry):
        addr = entry['addr']
        src = entry['src']
        if src.startswith('train'):
            if not self.target_phase and not self.train_phase:
                # first time seeing a train module, start training phase
                self.track_reuse_on_training_start(entry)
            elif self.target_phase and not self.train_phase:
                # seeing a train module while in target phase, means we are starting a new training phase
                self.track_reuse_on_target_end(entry)
                self.track_reuse_on_training_start(entry)
            elif not self.target_phase and self.train_phase:
                # seeing a train module while in training phase, means we are continuing the same training phase
                self.track_reuse_on_training_continue(entry)
        elif src.startswith('target'):
            if self.train_phase and not self.target_phase:
                # seeing a target module while in training phase, means we are starting a new target phase
                self.track_reuse_on_training_end(entry)
                self.track_reuse_on_target_start(entry)
            elif not self.train_phase and self.target_phase:
                # seeing a target module while in target phase, means we are continuing the same target phase
                self.track_reuse_on_target_continue(entry)

    def cross_tier_reuse(self, entry):
        addr = entry['addr']
        compile_tier = entry['compile_tier']
        if addr in self.global_tier_mapping:
            if self.global_tier_mapping[addr] != compile_tier:
                print(f'Warning: Address {addr} compiled at different tiers: {self.global_tier_mapping[addr]} then {compile_tier}')
        self.global_tier_mapping[addr] = compile_tier

    def handle_line(self, line):
        entry = self.parse_line(line)
        if entry:
            self.track_reuse(entry)
            # self.cross_tier_reuse(entry)
        return entry

t_start = time.time()

p = LogLineParser()
for line in fileinput.input():
    p.handle_line(line)
p.track_reuse_on_target_end(None)

t_end = time.time()

print (f'Total train modules: {p.global_cnt_train_modules}, inclusive reuses: {p.global_cnt_reused_inclu}, aligned reuses: {p.global_cnt_reused_align}, not reused: {p.global_cnt_not_reused}')
if p.global_cnt_train_modules == 0:
    print('No training modules found.')
print (f'Time taken: {t_end - t_start} seconds')
incl_rate = (p.global_cnt_reused_inclu / (t_end - t_start))
print (f'Reuse rate: {incl_rate:.2f} b/sec')

summary = {
    "total_train_modules": p.global_cnt_train_modules,
    "inclusive_reuses": p.global_cnt_reused_inclu,
    "aligned_reuses": p.global_cnt_reused_align,
    "not_reused": p.global_cnt_not_reused,
    "time_seconds": t_end - t_start,
    "reuse_rate_b_per_sec": incl_rate,
    # "module_sizes": p.module_sizes
}

print(json.dumps(summary, indent=2, sort_keys=True))