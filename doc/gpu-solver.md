# GPU Solver

Quicksilver's external `qsgpusolve` helper accelerates Cuckatoo proof-of-work
for transfers and mining. It is not part of consensus validation: every proof
returned by the helper is checked by the node before use.

The helper currently uses CUDA and therefore requires a supported NVIDIA GPU
and CUDA toolkit. It is built separately so CUDA does not become a dependency
of the node or vault build.

## When it is needed

- The desktop vault can fall back to the built-in CPU solver on `main` and
  `publictest`, but the measured reference workload takes about 16 minutes on
  an 8-thread desktop. An external GPU is strongly recommended.
- An agent spend falls back to the built-in CPU solver on `main` or
  `publictest` only when the operator opts in. The command-line agent takes
  `-allowcputxpow`. The desktop stores the same choice as a checkbox under
  Controls > Options > Main. That checkbox does not reach the command-line
  agent, which does not read desktop settings, so the sign command the desktop
  copies includes the flag when the checkbox is on. The default is off because
  an agent starts the work on its own schedule, while the computer may be in
  use, and the grind uses every core for many minutes. Configure the helper
  before proving an agent spend unless that opt-in is deliberate.
- `sandbox` uses a small built-in graph and does not need GPU acceleration.
- Running or synchronizing a node, validating blocks, and receiving funds do
  not require a GPU.

`main` and `publictest` use one graph size for a transfer and for a block.
CPU solving is allowed for a transfer because it is one bounded search the
sender waits out once. An agent spend is that same bounded search, but the
agent starts it on its own schedule, so the processor is refused unless the
operator opts in. The command-line opt-in is `-allowcputxpow`. The desktop
checkbox next to the mining one is a separate setting, because the
command-line agent does not read desktop settings. It is opt-in for blocks
because block mining is a continuous race against GPU cards: a CPU can run
the graph, but an unbounded grind at near-zero odds would look like a broken
miner. The block opt-in is `-allowcpumining`, and the same switch is the
checkbox in the desktop's Controls > Options > Main that allows this
computer's processor to mine blocks.

The measured times are calibration results, not performance guarantees. GPU,
CPU, driver, and current network work requirements all affect completion time.

## Choosing the compute capability

`GPU_ARCH` (Linux) and `-GpuArch` (Windows) must match the card the helper will
run on. The examples below use `sm_61`; that is a value to replace, not a
default to copy. A helper built for the wrong architecture fails to load and
reports **exit status 4**, which is the same status as "no CUDA device found" —
so an architecture mismatch looks exactly like missing hardware.

| Card family | Example cards | Value |
| --- | --- | --- |
| Maxwell | GTX 950, GTX 960, GTX 970, GTX 980 | `sm_52` |
| Pascal | GTX 1050 Ti, GTX 1060, GTX 1080, P104-100 | `sm_61` |
| Turing | GTX 1660, RTX 2060, RTX 2080 | `sm_75` |
| Ampere | RTX 3060, RTX 3090, A100 | `sm_86` (`sm_80` for A100) |
| Ada | RTX 4070, RTX 4090, L40S | `sm_89` |

Read the value straight off the card rather than inferring it:

```bash
nvidia-smi --query-gpu=name,compute_cap --format=csv
```

A reported compute capability of `5.2` means `sm_52`, `6.1` means `sm_61`, and
so on.

**CUDA 13.0 removed support for Maxwell, Pascal and Volta.** Building for
`sm_52`, `sm_61` or `sm_70` requires a **CUDA 12.x** toolkit; CUDA 13.x cannot
target those cards at all. CUDA 12.x emits a deprecation warning for these
architectures, which is expected and not an error. Install a 12.x toolkit when
the target card predates Turing.

## Linux and other Unix-like systems

### Getting a CUDA toolkit

The helper needs `nvcc`, from NVIDIA's CUDA toolkit. It is not part of the
graphics driver and is not in this project's dependency list, because CUDA is
deliberately not a build dependency of Quicksilver itself. Two ways to get it:

- **Your distribution's package**, if it has one — `nvidia-cuda-toolkit` on
  Debian, Ubuntu and their derivatives, `cuda` on Arch. This is the least
  trouble, but you get whatever version the distribution shipped, and some
  distributions ship none.
- **NVIDIA's own repository**, from
  <https://developer.nvidia.com/cuda-downloads>, which covers every supported
  distribution and lets you choose the version — in particular a 12.x toolkit
  for a pre-Turing card, per the note above.

Check what you ended up with before building, because the version decides which
cards you can target at all:

```bash
nvcc --version
```

A `release 12.x` line can build for every architecture in the table above. A
`release 13.x` line cannot build for `sm_52`, `sm_61` or `sm_70`; `nvcc`
rejects those targets at build time.

### Building

Choose the compute capability for the target GPU (see above), and build the
helper with its own Makefile:

```bash
make -C src/crypto/cuckatoo/gpu GPU_ARCH=sm_61   # replace sm_61 with your card's value
```

The output is `src/crypto/cuckatoo/gpu/qsgpusolve`. The default graph size is
28, which matches `main` and `publictest`. A helper built for another graph size
is rejected at runtime.

Probe the executable and CUDA device without starting a solve:

```bash
src/crypto/cuckatoo/gpu/qsgpusolve 28 00000000 0 0; echo "exit=$?"
```

**A successful probe prints nothing.** Silence is the success signal, not a
missing CUDA toolkit or a broken build, so read the exit status rather than the
output: `exit=0` means the helper ran and found a usable CUDA device. Any other
status is explained in the table below.

**Building on a machine with no GPU works; probing it does not.** `nvcc` needs
no device to compile, so a helper can be built on one machine for a card in
another. Two consequences:

- `nvidia-smi` cannot tell you the compute capability of a card that is not
  there. Read it on the target machine, or take the value from the table above
  and confirm it once the helper is in place.
- The probe above returns **exit status 4** on a machine with no CUDA device.
  That is the correct answer, not a build failure — and it is the same status
  an architecture mismatch gives, so it proves nothing about whether you picked
  `GPU_ARCH` correctly. Only a probe on the target card does that.

The project's own `gpu_parity_tests` is skipped whenever `CUCKATOO_GPU_SOLVER`
is unset, whether or not the machine has a card. With it set but no usable
device, the solver exits 4 and the test fails; it does not skip.

| Status | Meaning |
| --- | --- |
| 0 | Ran to completion. A cycle was found, or the nonce window closed without one — the two are told apart by the presence of `nonce=` and `cycle=` on stdout, not by the status |
| 2 | Bad invocation: wrong argument count, or a malformed preimage |
| 3 | The helper was built for a different graph size than the one requested |
| 4 | No usable CUDA device — no device at all, or a binary built for the wrong compute capability |
| 5 | The device faulted mid-solve. The helper stops at the first fault and prints the CUDA error and a platform-specific first thing to check |

Status 5 exists because a faulted card is otherwise silent: the vendored
`gpuAssert()` consumes the CUDA error and resets the device, so the remaining
graphs in the run search stale host memory and find nothing, which is
indistinguishable from an unlucky nonce window. The helper therefore arms
before each call and tests independent signals after it, rather than trusting
the solver's return value.

## Windows

### Getting a CUDA 12.x toolkit

The helper needs `nvcc` from a **CUDA 12.x** toolkit. CUDA 13.x cannot target
`sm_52`, `sm_61` or `sm_70`, as above. NVIDIA's current download page offers a
13.x toolkit, so take a 12.x release from the toolkit archive:

<https://developer.nvidia.com/cuda-toolkit-archive>

Choose a 12.x Windows installer. When a newer display driver is already
installed, install the toolkit components only and leave the bundled display
driver unselected, so the installer does not replace that driver. The toolkit
is installed under

`C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.<n>`

which is the path `-CudaPath` takes. Check the compiler before building:

```powershell
& "$CudaPath\bin\nvcc.exe" --version
```

A `release 12.x` line can build for every architecture in the table above.

### Building

Use the maintained PowerShell wrapper from a Developer PowerShell prompt. Pass
paths and versions that match the installed Visual Studio and CUDA toolchains:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\contrib\windows\build-qsgpusolve-windows.ps1 `
  -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" `
  -VcVarsVersion "14.39" `
  -CudaPath "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9" `
  -GpuArch sm_61
```

The default output is
`src\crypto\cuckatoo\gpu\qsgpusolve.exe`. The script's default
`-VsInstallPath` is Visual Studio 2022 Community, the same path as the example
and the one the Windows build guide requires. Omit `-VcVarsVersion` to enter
the developer shell with Visual Studio's own default toolset. The example
passes `14.39` because the fleet builds with that toolset; it is not a default
to copy. When you do pass `-VcVarsVersion`, that version must exist under
`<VsInstallPath>\VC\Tools\MSVC` or the wrapper stops. Set `-CudaPath` to the
12.x toolkit you installed and `-GpuArch` to the target card.

`-Target qsgpucalibrate` builds the calibration twin instead, the Windows
equivalent of `make -C src/crypto/cuckatoo/gpu qsgpucalibrate`. That binary is a
measurement tool, not a miner: it times every graph rather than stopping at the
first cycle, and it is never on a validation path.

Probe it before configuring Quicksilver:

```powershell
.\src\crypto\cuckatoo\gpu\qsgpusolve.exe 28 00000000 0 0
```

### Raise the GPU watchdog before solving

Windows runs a **Timeout Detection and Recovery** watchdog that resets the
display driver when a GPU operation runs longer than `TdrDelay`, which defaults
to **2 seconds**. A single Cuckatoo graph at the shipped size takes **several
seconds**, so a device on which that limit applies can be reset mid-solve.

The helper detects a fault mid-solve and stops: it returns **exit status 5**,
names the CUDA error, and points at `TdrDelay`. It also warns at startup, before
any solving, when the device it is about to use reports that a kernel run-time
limit applies and `TdrDelay` is below 10 seconds or is unset — unset means
Windows applies its 2-second default. CUDA reports that as
`cudaDevAttrKernelExecTimeout`: 1 when a limit applies, 0 when it does not. A
reported 0 prints nothing. If the query fails, the helper still warns on a
short or unset value, so a failed query does not hide the warning. That warning
is advisory: TDR bounds how long a single kernel may occupy the adapter, not
total graph time, so a low value is a strong suspicion rather than a proof.

Neither signal replaces the event log. The exit-5 path has not been exercised
on genuinely faulting hardware, and a reset that lands between kernels can leave
nothing sticky behind, so confirm against Event ID 4101 as below rather than
concluding from a clean exit that the watchdog is not firing.

Raise the limit from an **elevated** PowerShell, then **reboot**:

```powershell
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" `
  -Name TdrDelay -PropertyType DWord -Value 60 -Force
```

The value has no effect until the machine restarts. It must be a `REG_DWORD`;
written as any other type it is ignored silently, which reproduces the original
symptom exactly.

Do not set `TdrLevel` to `0`. That disables the watchdog altogether, so a
genuinely hung GPU locks the machine with no recovery path. Raising `TdrDelay`
keeps the protection and only widens the window.

To confirm the fix, run a real solve and then check that no new reset events
were logged:

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; Id=4101} -MaxEvents 5 |
  Select-Object TimeCreated, Message
```

Event ID 4101 from source `Display` — *"Display driver nvlddmkm stopped
responding and has successfully recovered"* — timestamped during a solve is the
signature of this problem. A solve that appears to succeed is not sufficient
evidence on its own, because the reset does not occur on every graph.

Whether the startup warning was printed is the helper's own answer for the
device it is using. To see the driver model of the card:

```text
nvidia-smi --query-gpu=name,driver_model.current --format=csv
```

`nvidia-smi -q` prints the same fact under **Driver Model** / **Current**. On
Windows that value is TCC, WDDM, or MCDM. On Linux it is `N/A`, and the helper
does not print this warning. The warning follows the device query above. It
does not follow this column, and it does not follow whether a monitor is
attached.

## Configure Quicksilver

Pass an absolute path at startup:

```bash
quicksilver -cuckatoosolver=/absolute/path/to/qsgpusolve
quicksilver-daemon -cuckatoosolver=/absolute/path/to/qsgpusolve
quicksilver-agent -cuckatoosolver=/absolute/path/to/qsgpusolve <command>
```

The desktop also exposes the helper path in Controls → Options → Main and probes
the selected executable before reporting acceleration as ready. Restart after
changing the configured path.

The path is handed to the operating system as-is: it is never passed through a
shell, so spaces and characters such as `&`, `;` or `$` in it need no quoting and
carry no special meaning. It may be a wrapper script rather than the solver
binary — `test/testrun/qs-solver-dispatch.sh` is one — provided the script is
executable and takes the same arguments.

Use `-cuckatoosolvertimeout=<seconds>` only when the default no-progress
watchdog is unsuitable for the target hardware. A missing, failed, malformed,
or timed-out helper result is never trusted.
