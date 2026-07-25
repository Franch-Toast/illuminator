load("//src/ebpf_common:bpf_probe.bzl", "bpf_probe")

# Orphan probes — BPF sources without corresponding userspace plugins
# TODO: mem_tracer needs a corresponding Source plugin or should be removed.

bpf_probe(name = "mem_tracer", src = "memory/mem_tracer.bpf.c")
