# Security Policy

## Supported Versions

Quicksilver is pre-1.0. The current public development series is 0.1.x; only
the most recent published 0.1.x source state receives security fixes. Older
development snapshots are not supported and are not back-ported. Support is
best-effort, with no fixed response-time commitment.

## Reporting a Vulnerability

Do not open public GitHub issues for vulnerability reports.

Report security-sensitive issues **privately** through GitHub's Private
Vulnerability Reporting:

https://github.com/quicksilver-hg/quicksilver/security/advisories/new

If you cannot use GitHub, you may instead email
`quicksilver.maintainers@gmail.com`. That mailbox is plaintext, so prefer the
GitHub channel for anything sensitive.

Quicksilver deliberately publishes no maintainer encryption keys: report
transport is already protected by GitHub's private advisory channel, and a
long-lived key would add rotation and impersonation risk without improving on
that.

We practise coordinated disclosure. We will acknowledge your report, work with
you on a fix, and agree a public-disclosure timeline together; with your
permission we will credit you in the published advisory.
