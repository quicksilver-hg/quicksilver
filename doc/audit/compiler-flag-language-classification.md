# Compiler Flag Language Classification

`try_append_cxx_flags` performs a C++ capability probe, but target options added
by the helper are consumed by every source language in that target. The helper
therefore keeps shared compiler-driver options shared and offers `CXX_ONLY` as
an explicit call-site choice for diagnostics whose semantics require C++.

This inventory covers all 53 executable calls. The five calls classified as
probe-only do not attach target options. The five examples in the helper's
documentation block are not executable and are excluded from the count.

## Evidence Rules

- **Shared** means the option controls the compiler driver, preprocessor,
  assembler, instrumentation, hardening, or a diagnostic that applies to C as
  well as C++. Microsoft `/W3` and `/wd` are language-neutral controls even
  when a particular suppressed warning is more likely in one language.
- **C++ only** requires `CXX_ONLY`, and the bar is that the diagnostic *cannot
  fire* in C, not that it is usually about C++. Clang's diagnostic reference
  describes shadowed fields and unused member functions in terms of the C++
  object model, neither of which C has. GCC documents overloaded virtuals as
  C++/Objective-C++ only and describes suggested overrides in terms of virtual
  functions and the `override` keyword. **Thread-safety analysis fails that bar
  and is classified shared** -- see R18.
- **Probe only** means the call has no `TARGET`. It either updates a C++
  configuration variable or returns a result used by explicit follow-up code,
  so there is no target compile option to guard.
- **Shared + link** identifies calls that intentionally add the accepted option
  to both compilation and the compiler-driver link command. `CXX_ONLY` may
  guard only `target_compile_options`; neither `target_link_options` branch may
  use `COMPILE_LANGUAGE`.

As a direct compiler check, GCC 13 accepts `-Woverloaded-virtual` and
`-Wsuggest-override` in the helper's C++ probe, but `gcc-13 -x c -Werror`
rejects each as valid only for C++/Objective-C++. That is the concrete F-271
failure mode. The other C++-only rows are classified by diagnostic semantics,
not by whether one current C frontend happens to accept and ignore the switch.

## Root Calls (47)

| ID | Site | Option | Classification | Named evidence |
|---|---|---|---|---|
| R01 | `CMakeLists.txt:304` | `-Wa,-muse-unaligned-vector-move` | Shared | GNU driver `-Wa` forwards an assembler option; it is independent of source grammar. |
| R02 | `CMakeLists.txt:316` | `/bigobj` | Shared | MSVC `/bigobj` changes COFF section capacity for compiler-produced objects. |
| R03 | `CMakeLists.txt:318` | `-Wa,-mbig-obj` | Shared | GNU driver `-Wa` forwards the COFF large-object assembler option. |
| R04 | `CMakeLists.txt:354` | `-fsanitize=${SANITIZERS}` | Shared | GCC/Clang instrumentation option for C and C++; C objects must match the sanitizer link runtime. |
| R05 | `CMakeLists.txt:412` | `/W3` | Shared | MSVC warning-level compiler control for C and C++. |
| R06 | `CMakeLists.txt:413` | `/wd4018` | Shared | MSVC language-neutral disable control; C4018 is signed/unsigned mismatch. |
| R07 | `CMakeLists.txt:414` | `/wd4146` | Shared | MSVC language-neutral disable control; C4146 is unary minus on an unsigned type. |
| R08 | `CMakeLists.txt:415` | `/wd4244` | Shared | MSVC language-neutral disable control; C4244 is a conversion with possible data loss. |
| R09 | `CMakeLists.txt:416` | `/wd4267` | Shared | MSVC language-neutral disable control; C4267 is `size_t` conversion with possible data loss. |
| R10 | `CMakeLists.txt:417` | `/wd4715` | Shared | MSVC language-neutral disable control; C4715 is a missing return path. |
| R11 | `CMakeLists.txt:418` | `/wd4805` | Shared | MSVC language-neutral disable control; C4805 is an unsafe mixed-type operation. |
| R12 | `CMakeLists.txt:424` | `-Wall` | Shared | GCC/Clang warning umbrella covers both C and C++. |
| R13 | `CMakeLists.txt:425` | `-Wextra` | Shared | GCC/Clang extra-warning umbrella covers both C and C++. |
| R14 | `CMakeLists.txt:426` | `-Wgnu` | Shared | Clang GNU-extension umbrella includes C constructs such as case ranges and anonymous structs. |
| R15 | `CMakeLists.txt:428` | `-Wformat -Wformat-security` | Shared | Format-string checking applies to C and C++. |
| R16 | `CMakeLists.txt:429` | `-Wvla` | Shared | Variable-length-array diagnostics apply to C and C++. |
| R17 | `CMakeLists.txt:430` | `-Wshadow-field` | **C++ only** | Clang diagnoses parameters or non-static data members shadowing inherited members. |
| R18 | `CMakeLists.txt:431` | `-Wthread-safety` | Shared | 🔎 **Measured, not read.** Clang 20 runs the full analysis on a C translation unit: a `guarded_by` global read without the capability held emits `-Wthread-safety-analysis` from `clang-20 -x c`. The attributes are GNU attributes, not a C++ extension, so guarding this flag would delete a working diagnostic from every C translation unit. |
| R19 | `CMakeLists.txt:432` | `-Wloop-analysis` | Shared | Clang loop diagnostics inspect loop statements available in both languages. |
| R20 | `CMakeLists.txt:433` | `-Wredundant-decls` | Shared | Redundant declarations exist in both C and C++. |
| R21 | `CMakeLists.txt:434` | `-Wunused-member-function` | **C++ only** | Clang's diagnostic is specifically an unused member function. |
| R22 | `CMakeLists.txt:435` | `-Wdate-time` | Shared | Preprocessor date/time macro checking applies to both languages. |
| R23 | `CMakeLists.txt:436` | `-Wconditional-uninitialized` | Shared | Clang diagnoses conditionally uninitialized variables in both C and C++. |
| R24 | `CMakeLists.txt:437` | `-Wduplicated-branches` | Shared | GCC checks duplicated conditional branches in both languages. |
| R25 | `CMakeLists.txt:438` | `-Wduplicated-cond` | Shared | GCC checks repeated conditions in both languages. |
| R26 | `CMakeLists.txt:439` | `-Wlogical-op` | Shared | GCC checks suspicious logical operations in both languages. |
| R27 | `CMakeLists.txt:440` | `-Woverloaded-virtual` | **C++ only** | GCC documents this option as C++/Objective-C++ only. |
| R28 | `CMakeLists.txt:441` | `-Wsuggest-override` | **C++ only** | GCC defines it around virtual functions and the C++ `override` keyword. |
| R29 | `CMakeLists.txt:442` | `-Wimplicit-fallthrough` | Shared | Switch fallthrough diagnostics apply to both C and C++. |
| R30 | `CMakeLists.txt:443` | `-Wunreachable-code` | Shared | Unreachable-code analysis applies to both C and C++. |
| R31 | `CMakeLists.txt:444` | `-Wdocumentation` | Shared | Clang documentation-comment diagnostics apply to declarations in both languages. |
| R32 | `CMakeLists.txt:445` | `-Wself-assign` | Shared | Clang diagnoses self-assignment expressions in both languages. |
| R33 | `CMakeLists.txt:446` | `-Wbidi-chars=any` | Shared | GCC checks source characters before C/C++ grammar differs. |
| R34 | `CMakeLists.txt:447` | `-Wundef` | Shared | Preprocessor undefined-macro diagnostics apply to both languages. |
| R35 | `CMakeLists.txt:452` | `-Wunused-parameter` / `-Wno-unused-parameter` | Shared | Function parameters and the diagnostic exist in both C and C++. |
| R36 | `CMakeLists.txt:464` | `-fno-extended-identifiers` | Shared | Identifier character handling applies to both C and C++. |
| R37 | `CMakeLists.txt:471` | `-fdebug-prefix-map` | Shared | Debug-path rewriting is language-independent. |
| R38 | `CMakeLists.txt:474` | `-fmacro-prefix-map` | Shared | Preprocessor macro-path rewriting is language-independent. |
| R39 | `CMakeLists.txt:482` | `-fstack-reuse=none` | Shared + link | GCC stack-slot reuse is a code-generation option; the existing helper also validates and forwards it on the driver link command. |
| R40 | `CMakeLists.txt:495` | `_FORTIFY_SOURCE=3` probe | Probe only | The result controls explicit, language-shared fortify compile definitions below the probe. |
| R41 | `CMakeLists.txt:511` | `-Wstack-protector` | Shared | GCC stack-protector coverage diagnostic applies to generated functions in both languages. |
| R42 | `CMakeLists.txt:512` | `-fstack-protector-all` | Shared + link | Stack-protector instrumentation applies to C and C++; the driver link option remains unguarded. |
| R43 | `CMakeLists.txt:513` | `-fcf-protection=full` | Shared + link | Control-flow protection is a target code-generation and link property, not C++ syntax. |
| R44 | `CMakeLists.txt:519` | `-fstack-clash-protection` | Shared + link | Stack-clash instrumentation applies to generated functions in both languages. |
| R45 | `CMakeLists.txt:524` | `-mbranch-protection=bti` | Shared | AArch64 branch protection is a target code-generation option for C and C++. |
| R46 | `CMakeLists.txt:526` | `-mbranch-protection=standard` | Shared | AArch64 branch protection is a target code-generation option for C and C++. |
| R47 | `CMakeLists.txt:555` | `-Werror` or `/WX` | Shared | Both compiler families define warnings-as-errors for C and C++; Win64 WERROR must cover C translation units. |

## LevelDB Calls (2)

| ID | Site | Option | Classification | Named evidence |
|---|---|---|---|---|
| L01 | `cmake/leveldb.cmake:91` | `-Wconditional-uninitialized` / negative form | Shared | The underlying variable-flow diagnostic is valid in C and C++; the interface stays reusable. |
| L02 | `cmake/leveldb.cmake:94` | `-Wsuggest-override` / negative form | **C++ only** | Both the positive probe and attached negative form describe C++ virtual overrides. |

## Configuration Calls (2)

| ID | Site | Option | Classification | Named evidence |
|---|---|---|---|---|
| P01 | `cmake/module/ProcessConfigurations.cmake:135` | `-g3` | Probe only | A successful result changes `CMAKE_CXX_FLAGS_DEBUG`; no target option is attached. |
| P02 | `cmake/module/ProcessConfigurations.cmake:141` | `-ftrapv` | Probe only | A successful result changes `CMAKE_CXX_FLAGS_DEBUG`; no target option is attached. |

## Helper Bootstrap Calls (2)

| ID | Site | Option | Classification | Named evidence |
|---|---|---|---|---|
| H01 | `cmake/module/TryAppendCXXFlags.cmake:148` | `/WX /options:strict` | Probe only | Populates the strict MSVC flags used by later C++ capability probes; it attaches no target option. |
| H02 | `cmake/module/TryAppendCXXFlags.cmake:150` | `-Werror` | Probe only | Populates the warnings-as-errors flag used by later C++ capability probes; it attaches no target option. |

## Compile Database Gate

The pre-workaround mixed C/C++ target and the fixed target were configured with
the same Debug options, `WERROR=ON`, `SANITIZERS=address`, and exported compile
databases. Build-directory, include-path, output-path, and prefix-map values were
normalized before comparing commands.

For GCC 13, the C command lost exactly the two C++ diagnostics that GCC's C++
probe accepts but its C frontend rejects:

```diff
 -Wlogical-op -Woverloaded-virtual -Wsuggest-override -Wimplicit-fallthrough
+-Wlogical-op -Wimplicit-fallthrough
```

For Clang 20, the C command lost the four root warning options classified as
C++ only, while retaining every shared option -- `-Wthread-safety` among them:

```diff
 -Wvla -Wshadow-field -Wthread-safety -Wloop-analysis -Wredundant-decls -Wunused-member-function -Wdate-time -Wconditional-uninitialized -Woverloaded-virtual -Wsuggest-override -Wimplicit-fallthrough
+-Wvla -Wthread-safety -Wloop-analysis -Wredundant-decls -Wdate-time -Wconditional-uninitialized -Wimplicit-fallthrough
```

⚖ Note what this half of the gate does **not** show. Clang 20 accepts all five
of those options for C and ignores the four it cannot apply, silently -- checked
directly with `clang-20 -Werror -fsyntax-only <flag> -x c /dev/null`, which is
clean for every one of them. So no Clang build has ever emitted a diagnostic
about these flags, and removing them from C commands buys correctness of
intent, not a warning. **The only measured F-271 symptom is GCC's two**, and
`-Wthread-safety` was the one removal that would have cost something real.

The normalized C++ command was identical before and after for both compilers.
A mixed-language contract fixture additionally produced these compile database
entries:

```text
C:   -Werror -fsanitize=address
C++: -Werror -fsanitize=address -Wsuggest-override
```

Its executable link command contained the unguarded `-fsanitize=address`. This
proves that `CXX_ONLY` is opt-in, WERROR and sanitizer instrumentation still
reach C translation units, and compile-language generator expressions have not
leaked into either link-option path.

## References

- [CMake `COMPILE_LANGUAGE` generator expression](https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html#genex:COMPILE_LANGUAGE)
- [GCC warning options](https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html)
- [GCC instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html)
- [Clang diagnostic flags](https://clang.llvm.org/docs/DiagnosticsReference.html)
- [Clang thread-safety analysis](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html)
- [MSVC compiler options](https://learn.microsoft.com/en-us/cpp/build/reference/compiler-options-listed-alphabetically)
- [MSVC warnings C4000 through C4199](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4000-through-c4199)
- [MSVC warnings C4200 through C4399](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4200-through-c4399)
- [MSVC warnings C4600 through C4799](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4600-through-c4799)
- [MSVC warnings C4800 through C4999](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4800-through-c4999)
