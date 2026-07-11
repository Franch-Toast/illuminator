# BPF probe compilation rules — organized by subsystem
# Usage: bazel build //src/ebpf/probes:all

load(":bpf_probe.bzl", "bpf_probe", "bpf_skeleton")

# --- CPU ---
bpf_probe(name = "cpu_profiler", src = "cpu/cpu_profiler.bpf.c")
bpf_skeleton(name = "cpu_profiler_sk", src = "cpu/cpu_profiler.bpf.c")
bpf_probe(name = "cpu_sampler", src = "cpu/cpu_sampler.bpf.c")

# --- Scheduler ---
bpf_probe(name = "sched_tracer", src = "sched/sched_tracer.bpf.c")
bpf_skeleton(name = "sched_tracer_sk", src = "sched/sched_tracer.bpf.c")
bpf_probe(name = "sched_analyzer", src = "sched/sched_analyzer.bpf.c")
bpf_skeleton(name = "sched_analyzer_sk", src = "sched/sched_analyzer.bpf.c")
bpf_probe(name = "offcpu_profiler", src = "sched/offcpu_profiler.bpf.c")
bpf_skeleton(name = "offcpu_profiler_sk", src = "sched/offcpu_profiler.bpf.c")

# --- I/O ---
bpf_probe(name = "bio_latency", src = "io/bio_latency.bpf.c")
bpf_skeleton(name = "bio_latency_sk", src = "io/bio_latency.bpf.c")

# --- Network ---
bpf_probe(name = "net_tracer", src = "net/net_tracer.bpf.c")
bpf_skeleton(name = "net_tracer_sk", src = "net/net_tracer.bpf.c")
