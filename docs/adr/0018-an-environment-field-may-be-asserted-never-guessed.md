# An environment field may be asserted, never guessed

summary: When the machine cannot tell the recorder what it is, the operator may assert the value through an environment variable — an assertion wins over detection, as it already does for the commit hash — and the record then says so in the field itself and warns on standard error; the recorder never substitutes a vague value of its own, and with neither an asserted nor a detected value it still refuses to record anything.

## Context

`benchmarks/environment.sh` refuses to emit a partial environment block, because
a partial one looks like a record and is not one. Every measurement script is
built on that refusal: nothing is measured without a complete record.

The refusal has now been triggered by a machine rather than by a mistake. On an
`aarch64` host under a Linux VM on macOS, the guest is not told what CPU it is
running on: `/proc/cpuinfo` carries no model line at all, and `lscpu` answers
with a placeholder. The CPU cannot be detected, so no measurement can run there.

The recorder is working correctly. But "this machine can never be measured" is
the wrong outcome when a human sitting in front of it can read the answer off
the host in one command.

There was already a field in this position. The commit hash cannot be read when
the repository is a worktree mounted into a container without its parent, and
the answer taken then was: let the operator supply it, warn about it, and say in
the record that it was supplied. That pattern is the precedent this generalises.

## Decision

**Assertion first, then detection, then refusal.** An assertion supplied through
the field's named environment variable is taken. Failing that, the field tries
every source it has. Failing those too, the script refuses as before.

Assertion comes first rather than last, and the two halves of that ordering are
one decision, not two: the operator knows what machine they are sitting at
better than a guest kernel does, and asserting is a deliberate act that cannot
be reached by accident. It is also what the commit hash already does, where the
supplied value wins over `git`. A rule that changed depending on whether
detection happened to succeed would be a rule nobody could predict.

**An asserted value is marked in the field itself**, not in a second field
beside it. The record is read by people, and a person reading `cpu_model` will
not go looking for `cpu_model_source`. The mark travels with the value into
every copy of the record.

**An assertion warns on standard error.** A real recorded run must not lose the
provenance of a field quietly, which is the same reason the commit hash warns.

**An assertion is checked before anything is printed.** The block is a list of
`name: value` lines, so a value carrying a newline or a colon can forge a field
the recorder never wrote. Such a value is refused — and refused before the first
line of the block reaches standard output, because a recorder that prints half a
block and then exits non-zero has still put a half-block somewhere a careless
caller could keep. The callers in `benchmarks/` do not keep it; the guarantee
should not depend on their all remembering that.

**The recorder never invents a value.** No "unknown", no "generic aarch64", no
architecture string standing in for a model name. That is the failure this whole
mechanism exists to prevent: a field that is present and vague reads as a
record, and the reader has no way to tell it from a real one.

**The refusal says that asserting is possible.** The old message named the field
it could not determine and stopped there, which leaves the person who hit it
with no next step.

## Reasoning

**The principle being protected is that a record is either complete or absent.**
An assertion does not weaken it: the value is real, it is attributed, and the
attribution is visible. A guess would weaken it, because a guessed value is
indistinguishable from a measured one once it is written down.

**The two failures are not symmetrical.** Refusing to measure on a machine that
cannot name itself costs a measurement. Recording a vague value costs the
reader's ability to trust any field in the block. The first is recoverable by
one environment variable; the second is not recoverable at all.

**Marking in-band survives copying.** Figures get quoted, pasted and summarised.
A mark inside the value goes with them; a separate field does not.

## Consequences

Measurement is possible on machines that cannot identify their own hardware, and
a reader of those results can see at a glance which fields were asserted.

The mark is deliberately slightly ugly in the record. A reader should notice it.

New fields added to the environment block inherit this: detect, then allow an
assertion, then refuse. They do not inherit permission to invent a default.
