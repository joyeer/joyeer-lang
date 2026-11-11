# Design Decisions

Short records of language and compiler decisions that were contested or
non-obvious. They preserve *why* a choice was made and which alternatives were
rejected, so later work (human or AI) does not reopen settled questions
without new evidence.

A decision record is not normative. Language rules live in [the
spec](../../specs/README.md); once a decision is adopted, the rule goes in the relevant
chapter and the record links to it.

## When to write one

- A feature has more than one plausible design and you chose one.
- A choice trades off against a stated goal (zero-cost abstractions,
  deterministic destruction, AI-authored code reliability).
- A reviewer or earlier session proposed something and it was rejected.

## Workflow for a user-visible feature

1. Proposal: write a record (status `proposed`) if the design is contested.
2. Spec: change the normative chapter; mark provisional rules as such.
3. Tests first: add failing stage fixtures and focused GoogleTests that cite
   the spec section.
4. Implementation through the pipeline stages the feature crosses.
5. Review with the role checklists in [.claude/agents/](../../.claude/agents/),
   each run separately from the author.
6. `python3 scripts/validate.py --llvm-root <sdk>`.
7. Set the record to `accepted` and link the spec section.

## Template

Copy [template.md](template.md) to `NNNN-short-title.md`.
