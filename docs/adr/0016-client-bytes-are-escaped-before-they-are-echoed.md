# Client bytes are escaped before they are echoed

summary: An error reply that quotes back a client-supplied command name escapes every byte outside printable ASCII as \xHH and caps the echo at 128 bytes; more generally, client bytes may be returned inside a length-framed bulk string but never inside a simple string or a simple error.

## Context

An unknown command is answered with `-ERR unknown command 'X'`, where `X` is the
name the client sent. That name arrives as a bulk string, which is length-framed,
so it may contain any byte at all — including CRLF.

A simple error is not length-framed. It is terminated by the first CRLF. Echoing
the name raw therefore lets the sender end the error line early and dictate the
bytes the client reads as its next reply:

    *1\r\n$14\r\nBAD\r\n+INJECTED\r\n

That is a legal request. Against a server that echoes raw, the client reads
`-ERR unknown command 'BAD` as one reply and `+INJECTED` as the next — a reply
the server never sent, chosen by whoever opened the connection.

This is response-line injection, and it is the same shape as header injection in
HTTP: a framing layer that trusts a length, wrapped by one that trusts a
delimiter.

## Decision

**Escape, then quote.** Every byte outside printable ASCII becomes `\xHH`, and
the echo is capped at 128 bytes.

**The general rule the escape is an instance of:** client bytes may be returned
inside a bulk string, which is length-framed, but never inside a simple string or
a simple error, which are terminated by the first CRLF.

## Reasoning

**The cap and the escape answer different attacks.** The escape removes the
delimiter the injection depends on. The cap keeps a client from choosing the size
of a reply it did not pay to receive, which is a cheap amplification otherwise.

**Escaping at the reply, not at the parse.** The parser stays a pure function
over bytes and does not know what will be done with what it returns. Sanitising
there would put a protocol concern in a layer that has none, and would corrupt
the name for every other use — a name that is echoed is not a name that is
wrong.

**Not "reject weird names".** A name with strange bytes in it is a legal request
and its answer is a correct error reply, not a disconnect. Refusing correctly is
the point: the RESP downgrade path has clients detect a RESP2-only server by
receiving `-ERR unknown command 'HELLO'`, so a server that closes or hangs on a
name it does not implement fails the handshake instead of declining it.

## Alternatives

**Quote with a length-framed bulk string instead.** The error would stop being a
simple error, which is what clients expect for an unknown command, and every
client's error path would have to be re-read to see whether it copes. Escaping
keeps the reply the shape the protocol says it is.

**Drop the name from the message.** `-ERR unknown command` alone is safe and much
less useful: the name is what tells a caller which of its calls was wrong.
