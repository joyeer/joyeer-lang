# Markdown reference

The renderer implements a small subset. Everything else is shown as text.

## Inline

Use *emphasis*, **strong text**, `inline code` and [links](https://example.com).
Escape a marker with a backslash: \*not emphasis\*.

## Blocks

- bullet lists
- with several items

1. numbered lists
2. keep their order

```
fenced code <b>is escaped</b>
and keeps its line breaks
```

---

Raw HTML such as <script>alert(1)</script> is escaped, not executed.

## Not supported

Nested lists, tables, images, reference links and raw HTML.
