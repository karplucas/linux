# RXE CRIU kernel change guidelines

Use this checklist for Linux kernel changes implementing RXE checkpoint and
restore. The normal rules in `Documentation/process/` continue to apply.

## Scope and structure

- Keep each commit focused on one reviewable behavior.
- Separate UAPI, RXE implementation, tests, and documentation when each can
  stand and build independently.
- Avoid mixing cleanup or unrelated refactoring with functional changes.
- Prefer a short series of small commits to a single large commit.
- Keep the tree buildable after every commit.

## Kernel formatting

- Follow `Documentation/process/coding-style.rst`.
- Use tabs for indentation and spaces for alignment as required by kernel
  style.
- Keep lines at 100 columns or fewer unless readability clearly improves by
  exceeding the limit.
- Use kernel types and helpers, including checked size arithmetic for
  userspace-controlled lengths.
- Order declarations in reverse Christmas-tree form where practical.
- Place opening braces according to kernel style and do not add braces around
  a single statement unless another branch requires them.
- Use kernel-doc only for exported interfaces. Use short comments to explain
  non-obvious invariants and ordering, not a line-by-line restatement of code.
- Treat all restore input as untrusted. Validate reserved fields, enum values,
  sizes, object ownership, queue geometry, and state transitions.
- Do not serialize pointers, locks, work items, timers, or whole internal
  kernel structures as a stable interface.

## UAPI rules

- Append new uverbs method and attribute IDs; do not renumber existing IDs.
- Use fixed-width UAPI types and explicit reserved fields.
- Require reserved fields to be zero.
- Document the operation's ordering, ownership, and failure semantics beside
  the UAPI definition.
- Keep queue-page restoration outside `rxe_restore_qp()` and
  `rxe_restore_cq()`. Those callbacks create object shells and preserve mmap
  identity; queue state is synchronized only after CRIU has restored the
  mapped pages.
- RXE queue pages are copied only between the same kernel implementation.
  Validate their queue type, length, element size, index mask, mmap offset,
  and cursors, but do not introduce a cross-kernel conversion ABI.

## Commit messages

- Use a subsystem prefix such as `RDMA/rxe:` or `criu_rdma:`.
- Keep the subject imperative and at most 75 characters where practical.
- Explain the problem and observable behavior before implementation details.
- Keep commit messages concise; avoid duplicating design documents in them.
- Include the developer's normal `Signed-off-by` trailer.
- Do not add Codex, an AI tool, or an automated assistant as an author,
  co-author, or commit-message trailer.

## Verification for every commit

Run checkpatch on the commit:

```text
scripts/checkpatch.pl --strict --git HEAD
```

Build the affected RXE code using an out-of-tree build directory:

```text
make O=<build-dir> olddefconfig
make O=<build-dir> -j<number> drivers/infiniband/sw/rxe/
```

When a commit changes generic RDMA core or headers, also build the relevant
RDMA core directory or all enabled RDMA modules. Run focused KUnit or CRIU
RDMA tests when the commit adds behavior they can exercise.

Before committing, use `git diff --check`. After committing, verify the
commit rather than only the working tree so checkpatch sees the exact review
unit. Fix new warnings and rebuild before starting the next commit.
