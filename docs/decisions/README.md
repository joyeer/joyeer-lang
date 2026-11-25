# Design Decisions

Short records of contested or non-obvious design decisions. They preserve
*why* a choice was made and which alternatives were rejected, so later work
(human or AI) does not reopen settled questions without new evidence.

A decision record is not normative. Language rules live in [the
spec](../../specs/README.md); tool and library contracts live in their own
documentation.

## When to write one

- A feature has more than one plausible design and you chose one.
- A choice trades off against a stated goal (zero-cost abstractions,
  deterministic destruction, AI-authored code reliability).
- A reviewer or earlier session proposed something and it was rejected.

## Workflow for a user-visible feature

1. Proposal: write a record (status `proposed`) if the design is contested.
2. Contract and design review: if language rules change, update the normative
   spec chapter, mark provisional rules as such, and have `language-designer`
   review the proposal and spec separately from the author before implementation.
   For tool and library changes, update the affected subsystem's documented
   contract when it changes.
3. Tests first: for compiler behavior changes, add failing focused GoogleTests
   and durable stage or end-to-end fixtures; cite the spec section where
   applicable. For tools and libraries, add failing tests suited to the
   affected subsystem.
4. Implementation: update every affected pipeline stage or subsystem.
5. Implementation review: run the applicable implementation reviewers
   separately from the author, using the
   [role checklists](../../.claude/agents/).
6. Run [full platform validation](../building.md#full-platform-validation).
7. If there is a decision record, mark it `accepted` and link the relevant
   spec section or subsystem documentation.

## Template

Copy [template.md](template.md) to `NNNN-short-title.md`.
