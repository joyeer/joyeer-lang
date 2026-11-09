# 0019: Channels

- Status: not started. Phase P5b and P6 of the [concurrency plan](../plan/concurrency.md).
- Spec: none yet; the provisional text is [§2.6.3](../spec/02-types.md#263-concurrency-builtin-types-provisional).

The design is in [0013](0013-concurrency-model.md) (single sender, single receiver, bounded,
`channel(capacity:, sender: &s, receiver: &r)`, `Result<T, ChannelClosed>` from `receive`, ends
noncopyable and closing in their destroy helper, a linear element type rejected) and
[0015](0015-noncopyable-and-linear-types.md). It is written when tasks (0018) have landed, because
channel ends reuse the builtin generic type kind and the consuming builtin member that tasks add, and
because the channel runtime (a ring buffer over the P3 mutex and condition, P6) is built with it.
