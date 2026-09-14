# Logging

Quicksilver's log is a machine-searchable operational interface as well as a
human-readable diagnostic stream. Every node log call uses a category from
`HgLog`; uncategorized `LogPrintf` calls are not permitted.

Messages use a short event description followed by stable `key=value` fields
for identifiers and state. The product name is omitted because every entry in
`quicksilver.log` already has that context. Validation reject tokens are emitted
through a `reason=` field instead of being embedded in prose, so operators and
tests can search them without parsing sentences.

The register deliberately does not impose a closed verb vocabulary. That would
make legitimate diagnostics conform to a regex instead of improving their
meaning. `test/lint/lint-quicksilver-log-register.py` enforces only the
mechanically decidable boundaries: categorized calls, no redundant product
name, and structured reject reasons.
