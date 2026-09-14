# Dead-File Census

The dead-file census identified inherited files that no longer served the
Quicksilver product tree.

## Method

Files were grouped by whether they were mechanically unreachable, premise-dead
for Quicksilver, still live despite old terminology, or part of vendored and
legal attribution surfaces that should remain intact.

## Removed Classes

The cleanup removed obsolete release, verification, seed, and build scaffolding
that belonged to inherited distribution workflows rather than current
Quicksilver operation.

Low-risk dead files were removed only after checking build references, test
references, and documentation references.

## Preserved Classes

Vendored dependencies and upstream legal notices were preserved. Live vault,
RPC, and test code was not deleted merely because it contained inherited
terminology; those areas were handled by separate semantic cleanup passes.

## Verification

The pass used tracked-file searches, build-list checks, and targeted residue
scans. Follow-up work handled source and user-facing residue that required
behavioral changes rather than file deletion.
