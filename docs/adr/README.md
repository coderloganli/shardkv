# Decision records

One decision per file: what the situation was, what was decided, and why that
rather than the alternatives.

Records are written while the decision is being made, during a task's design stage
— not reconstructed afterwards, when the reasoning is already gone.

When a decision changes, the record is edited in place. This directory holds the
project's current answers, not an archaeology of previous ones.

## Finding one

Search rather than browse: `find_adr` matches on the words a decision is about and
returns the few records that bear on the question, with their summaries. Reading
the whole directory is a sign the question should have been a search.

## Writing one

Name the file `NNNN-the-decision-as-a-statement.md`, and give it a `summary:` line
under the title — that line is what search returns, so make it say the decision
rather than the topic.
