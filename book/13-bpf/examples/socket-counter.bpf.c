// SPDX-License-Identifier: GPL-2.0-only
/* 本章实验程序，不是内核源码节选。 */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} calls SEC(".maps");

SEC("socket")
int socket_counter(struct __sk_buff *skb)
{
	__u32 key = 0;
	__u64 *value;

	value = bpf_map_lookup_elem(&calls, &key);
	if (value)
		__sync_fetch_and_add(value, 1);

	return 0xffffffffU;
}

char LICENSE[] SEC("license") = "GPL";
