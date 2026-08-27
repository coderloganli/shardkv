# Values are byte strings

summary: A stored value is a byte string and nothing else; `INCR` parses the text and formats the result back, rather than a cached integer representation being kept alongside.

## Context

The counter commands (`INCR`, `DECR`, `INCRBY`, `DECRBY`) read a value, treat it
as a number, and write it back. The obvious optimisation is to keep the integer
form in the value so that a counter under repeated `INCR` is not parsed and
formatted on every operation, which is what Redis does with its int encoding.

The design carried that optimisation until it was reviewed, at which point the
edge cases became the point of interest rather than a detail.

## Decision

`Value` holds a byte string and an optional expiry deadline. There is no integer
variant.

`INCR` and its relatives parse the stored bytes as a signed 64-bit decimal,
reject anything that is not one, apply the delta with overflow checked, and store
the formatted result.

## Reasoning

**The optimisation has a correctness trap that is easy to walk into and hard to
find.** A value is a byte string on the wire, so whatever is stored must come
back byte-identical. `SET k 001` followed by `GET k` must return `001`, not `1`.
So must `+1`, and a value with leading whitespace, and one too long for an
`int64`. An integer representation is only safe when the text is the canonical
decimal form of the number it encodes, which means every write path needs a
canonicality check and every read path needs a formatter that provably inverts
it. Get that wrong and the bug does not surface at the counter commands where the
optimisation lives — it surfaces at `GET`, `APPEND`, `GETSET` and `STRLEN`, as
values that are subtly not what was stored.

**Nothing has been measured, so there is no case for paying that.** This is the
same argument as the decision to keep `std::unordered_map` for v1: an
optimisation adopted before there is a profile is a guess, and this one is a
guess that costs correctness surface. The parse-and-format is a handful of
nanoseconds against a request that also does a syscall.

**It is cheap to add later and the interface does not change.** Command
implementations reach values only through the shard, and the external contract —
values are byte strings — is exactly what makes the internal representation free
to change afterwards.

**The alternative considered was keeping the integer variant with an explicit
canonicality rule:** store the integer form only when the text round-trips
exactly, and convert to string form before any string command operates. That is
what Redis does and it is correct, but it means the first week's code carries a
representation invariant that every command has to respect, in a codebase where
the event loop and the protocol parser are also new. Deferring it removes one of
the three places a wrong answer could be coming from.

## Consequence

The string commands become trivial: `APPEND`, `GETSET`, `STRLEN` and `GET` all
operate on the stored bytes with no conversion and no special case for
integer-backed values.
