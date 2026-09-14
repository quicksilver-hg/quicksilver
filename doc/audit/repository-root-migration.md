# Repository Root Migration

Quicksilver was moved from a nested product layout to a repository root layout.

## Outcome

The public source tree now exposes the node, build files, documentation,
contrib tooling, tests, and project metadata from the repository root. Stale
nested-path references were removed from public docs and automation.

## Removed Bulk

The migration deleted obsolete outer scaffolding, stale nested project files,
and duplicated root-level metadata. Files that belonged to the product remained
at their canonical root-relative paths.

## Verification

Public docs and tooling were scanned for stale nested paths and local checkout
names. The cleanup also verified that root-level build and lint entry points
continued to point at the current tree.

Generated and archive material that is local-only should stay ignored rather
than reintroduced into the public source tree.
