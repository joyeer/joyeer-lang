# Plans

Plans track incomplete multi-step work: what is left, in what order, and how
each step is known to be done. They are not normative; language rules live in
[the spec](../../specs/README.md), and the reasons for contested choices live in
[decision records](../decisions/README.md). A plan links to both instead of
repeating them.

## When to write one

- The work spans more than one session or pull request.
- The work has phases whose order matters, or gates that must run on several
  platforms.
- A finished conversation left follow-up work that someone else will pick up.

A change that one pull request finishes needs no plan.

## Rules

1. **Every plan has a todo list.** The `Todo` section is a Markdown checklist
   (`- [ ]` / `- [x]`), grouped by phase. Each item is one verifiable step, small
   enough for one session, written so that a reader can tell whether it is done.
2. **Update the list in every iteration.** At the end of each session or pull
   request that works on the plan, mark the items it finished `- [x]`, and add
   the evidence after the item: the PR, the commit, or the test or gate that
   proves it. Leave unfinished items `- [ ]`; if an item is blocked, say so on
   the item (`blocked: <reason>`). Add new items found along the way rather than
   doing unlisted work.
3. **Log the iteration.** Add one line to `## Progress log`, newest first:
   the date, what changed in the list, and anything that changed the plan.
4. **Keep the status line current.** `Status` is `proposed`, `in progress`,
   `blocked`, or `done`, with the date of the last update.
5. **Never mark an item done that was not verified.** A step whose test or gate
   did not run stays open, with what is missing written on it.
6. **Finish by deleting.** When every item is checked, move lasting facts to the
   spec, the decision records or the [implementation notes](../impl/), then
   delete the plan and its row in the [documentation index](../README.md).

Plans written before these rules adopt them the next time they are edited.

## Template

Copy [template.md](template.md) to `short-title.md` and add a row to the Plans
table in the [documentation index](../README.md).
