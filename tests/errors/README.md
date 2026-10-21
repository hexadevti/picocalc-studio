# Must-fail tests

Every file here must be **rejected** by the compiler or linker. The first line of each file states what
is expected:

```
// EXPECT: <stage> line=<n> match=<regex>
```

- `stage`: `compile` (preprocessor, parser or type checker) or `link` (unresolved symbol).
- `line`: the line the diagnostic must point at (`5|6` accepts either; `0` = no specific line). The IDE uses this to jump to the error, so it must be exact.
  Link errors point at the line of the first reference.
- `match`: a case-insensitive regular expression the message must match. It is deliberately loose, so the
  wording is free but the meaning is checked.

The harness also checks that the compiler **does not crash** and produces no module.
