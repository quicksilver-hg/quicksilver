# GPU Solver

`qsgpusolve-opencl`, Quicksilver's graphics helper, accelerates Cuckatoo
proof-of-work for transfers and mining on any card with an OpenCL driver: AMD,
NVIDIA and Intel. It is not part of consensus validation: every proof the
helper returns is checked by the node before use.

The helper comes with the release packages (on Ubuntu, the `quicksilver-gpu`
package) and is built with the project; see [Building Quicksilver](../INSTALL.md).
The desktop, the daemon and the agent find it on their own, in
`../libexec/quicksilver` from their own directory or beside their own
executable. It loads the OpenCL driver when it runs, so it needs no OpenCL SDK
to build, and a computer with no OpenCL driver simply reports no usable card.

## When it is needed

- The desktop vault can fall back to the built-in CPU solver on `main` and
  `publictest`, but the measured reference workload takes about 16 minutes on
  an 8-thread desktop. An external GPU is strongly recommended.
- An agent spend falls back to the built-in CPU solver on `main` or `publictest`
  only when the operator opts in with the command-line agent's
  `-allowcputxpow` flag. The desktop has no setting for it. The default is off
  because an agent starts the work on its own schedule, while the computer may
  be in use, and the grind uses every core for many minutes. Configure the
  helper before proving an agent spend unless that opt-in is deliberate.
- `sandbox` uses a small built-in graph and does not need GPU acceleration.
- Running or synchronizing a node, validating blocks, and receiving funds do
  not require a GPU.

`main` and `publictest` use one graph size for a transfer and for a block.
CPU solving is allowed for a transfer because it is one bounded search the
sender waits out once. An agent spend is that same bounded search, but the
agent starts it on its own schedule, so the processor is refused unless the
operator opts in. The command-line opt-in is `-allowcputxpow`. The desktop
has no setting for it, and the command-line agent does not read desktop
settings. It is opt-in for blocks
because block mining is a continuous race against GPU cards: a CPU can run
the graph, but an unbounded grind at near-zero odds would look like a broken
miner. The block opt-in is `-allowcpumining`, and the same switch is the
checkbox in the desktop's Settings > Options > Hardware that allows this
computer's processor to mine blocks.

The measured times are calibration results, not performance guarantees. GPU,
CPU, driver, and current network work requirements all affect completion time.

## Checking the cards

List the cards the helper can use:

```bash
qsgpusolve-opencl --list-devices
```

Each line names a card by an id such as `pci:0000:04:00.0`, with its status.
By default every graphics card except built-in graphics is used. Choose cards
with `-cuckatoodevice=<id>` (repeat it for several), or use
`-cuckatoodevice=cpu` to prepare proofs on the processor only.

The helper's exit status says what happened:

| Status | Meaning |
| --- | --- |
| 0 | Ran to completion. A cycle was found, or the nonce window closed without one — the two are told apart by the presence of `nonce=` and `cycle=` on stdout, not by the status. For `--list-devices`, the OpenCL driver loaded, even if it lists no card |
| 2 | Bad invocation: wrong argument count, or a malformed preimage |
| 3 | The helper was built for a different graph size than the one requested |
| 4 | No usable card: no OpenCL driver, a `--device` that is not present, a kernel that does not compile, or a failed start-up check |
| 5 | The card faulted mid-solve: an enqueue, read or finish failed. The helper stops at the first fault and prints the OpenCL error and a platform-specific first thing to check |

The project's own `gpu_parity_tests` is skipped whenever `CUCKATOO_GPU_SOLVER`
is unset, whether or not the machine has a card. With it set but no usable
card, the helper exits 4 and the test fails; it does not skip.

## AMD cards on Linux kernel 7.0

On Linux kernel 7.0 the `amdgpu` driver's default limit for a compute job is 2000 ms (kernel 6.17 uses 10000 ms). A Cuckatoo solve
job can run longer than that. When it does, the kernel logs a line like `ring comp_1.3.0 timeout` and resets the GPU, and
mining stalls. On one tested machine (an RX 580 on kernel 7.0.0-42) this repeated about every two seconds while mining, and the
machine later froze.

To see whether you are affected, run `sudo dmesg | grep 'ring comp'` while mining, and read the current limit:

```
cat /sys/module/amdgpu/parameters/lockup_timeout
modinfo -p amdgpu | grep lockup
```

The first command prints a value only if one was set. It is four numbers in milliseconds, in the order GFX, compute, SDMA, video. A blank line means none was set, and the driver uses the default that the second command shows in `(default: ...)`. On kernel 7.0 that default is 2000 for all jobs. If the compute limit is 2000, raise it:

1. Create `/etc/modprobe.d/amdgpu-lockup.conf` containing `options amdgpu lockup_timeout=2000,60000,2000,2000`.
2. Run `sudo update-initramfs -u` (Debian, Ubuntu, Linux Mint) and reboot.
3. Read the parameter again. It must show `2000,60000,2000,2000`.

On the tested machine, this removed the timeout lines (none in a 2 hour 24 minute mining run). This has been checked on one machine and one card
family; other cards and kernels may behave differently. Nvidia cards are not affected.

## Windows: raise the GPU watchdog before solving

Windows runs a **Timeout Detection and Recovery** watchdog that resets the
display driver when a GPU operation runs longer than `TdrDelay`, which defaults
to **2 seconds**. A single Cuckatoo graph at the shipped size takes **several
seconds**, so the card can be reset mid-solve.

The helper detects a fault mid-solve and stops: it returns **exit status 5**,
names the OpenCL error, and points at `TdrDelay`. It also warns at startup,
before any solving, when `TdrDelay` is below 10 seconds or is unset — unset
means Windows applies its 2-second default. That warning is advisory: TDR
bounds how long a single kernel may occupy the adapter, not total graph time,
so a low value is a strong suspicion rather than a proof.

Neither signal replaces the event log. A reset that lands between kernels can
leave nothing behind, so confirm against Event ID 4101 as below rather than
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

Event ID 4101 from source `Display` — for example *"Display driver nvlddmkm
stopped responding and has successfully recovered"* — timestamped during a solve is the
signature of this problem. A solve that appears to succeed is not sufficient
evidence on its own, because the reset does not occur on every graph.

## Using a helper you built yourself

To use a different helper instead of `qsgpusolve-opencl`, such as a CUDA or
ROCm build of your own, pass its absolute path at startup:

```bash
quicksilver -cuckatoosolver=/absolute/path/to/helper
quicksilver-daemon -cuckatoosolver=/absolute/path/to/helper
quicksilver-agent -cuckatoosolver=/absolute/path/to/helper <command>
```

It must keep the bundled helper's contract: the same arguments, the
`--list-devices` lines, `progress=`, `nonce=` and `cycle=` on stdout, and the
exit statuses above. Quicksilver does not ship or build any other helper.

The desktop also exposes the helper path in Settings → Options → Hardware and probes
the selected executable before reporting acceleration as ready. Restart after
changing the configured path.

The path is handed to the operating system as-is: it is never passed through a
shell, so spaces and characters such as `&`, `;` or `$` in it need no quoting and
carry no special meaning. It may be a wrapper script rather than the solver
binary — `test/testrun/qs-solver-dispatch.sh` is one — provided the script is
executable and takes the same arguments.

Use `-cuckatoosolvertimeout=<seconds>`, from 1 to 86400, only when the default
30-second no-progress watchdog is unsuitable for the target hardware. A missing, failed, malformed,
or timed-out helper result is never trusted.
