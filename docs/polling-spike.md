# Hardware Polling Spike

## Purpose

This branch exists to answer one narrow question:

Can the wired Xbox `02FF` controller path support real hardware-visible polling control, or is feeder-side injection cadence the only practical timing control on this machine?

This is an experimental research track. It is not the supported production architecture.

The supported product line remains the hybrid HID-child stack:

```text
GaYmXInputFilter -> xinputhid -> GaYmFilter -> HidUsb
```

## What Problem This Spike Is Trying To Solve

The supported hybrid stack now exposes:

- feeder-side injection cadence control
- best-effort HID-class poll-frequency control

The current live stack rejects HID-class poll-frequency control with `ERROR_INVALID_FUNCTION`, which means the supported stack does not currently expose a true hardware polling knob.

That leaves an open technical question:

- Is there a lower USB/composite attachment point where polling can be controlled in a `hidusbf`-style way?

## Non-Goals

This spike is not trying to:

- replace the supported hybrid stack
- rewrite the product architecture
- broaden supported device coverage
- ship a production-ready new filter path in one pass

If the result is “not viable,” that is still a successful spike.

## Hypotheses To Test

1. The controller’s observable polling behavior is owned below the current HID-child stack.
2. Real poll-rate control, if possible, will require a USB/composite-side attachment point rather than the current HID-child pair.
3. HID-class poll-frequency control may exist in theory, but the active `xinputhid` path on this machine does not expose it.

## Success Criteria

Any one of these outcomes is sufficient:

1. Positive proof:
   - a prototype can change hardware-visible polling behavior on the target controller path
   - the effect is observable in captured timing or request cadence

2. Negative proof:
   - the relevant lower path is identified
   - the controller/driver stack refuses or ignores polling control there
   - we can defend the conclusion that feeder cadence is the only practical control on this machine

## Investigation Order

1. Re-capture the live parent/composite and USB/HID-child stack layout for the controller.
2. Identify where `hidusbf`-style logic would actually have to sit to affect endpoint cadence.
3. Separate three possible timing layers:
   - USB endpoint polling interval
   - HID-class poll frequency
   - user-mode/kernel override injection cadence
4. Build a minimal probe for the candidate lower path.
5. Measure whether request cadence or observable input timing changes under that probe.

## Evidence We Already Have

- The supported hybrid HID-child stack works for override, XInput, `joy.cpl`, and transition handling.
- Feeder-side cadence control is real and now operator-visible.
- HID-class poll-frequency control is exposed in tooling, but the active stack rejects it on this machine.
- Earlier `hidusbf` testing changed enumeration/visibility characteristics when attached lower on the parent/composite side.

That last point is why this spike exists at all.

## Expected Artifacts

If the spike is successful, produce:

- one capture note for the live parent/USB/HID stack
- one short design note explaining the actual timing owner on this hardware path
- one minimal prototype or capture probe
- one conclusion:
  - viable next step for real polling control
  - or explicit closure that the supported product should stay with feeder cadence only

Current capture note:

- [polling-capture-note.md](./polling-capture-note.md)

Current measurement tool:

- `CadenceProbe.exe` in debug/full tool builds

Current experimental package paths:

- `scripts\build-usb-child-probe-package.ps1`
- `GaYmFilter\GaYmUsbChildProbe.inf`
- `scripts\install-usb-child-probe.ps1`
- `scripts\uninstall-usb-child-probe.ps1`
- `scripts\build-parent-composite-probe-package.ps1`
- `GaYmFilter\GaYmCompositeProbe.inf`
- `scripts\install-parent-composite-probe.ps1`
- `scripts\uninstall-parent-composite-probe.ps1`

## Current Result

The USB-child probe is now a completed measurement step, and the composite-parent probe is now the first positive cadence-observation result.

Observed with the HID-child lower extension removed:

- HID child:
  - `GaYmXInputFilter -> xinputhid -> HidUsb`
- USB child:
  - `HidUsb -> GaYmFilter -> xboxgip`

Measured result:

- upper path remained steady at roughly `240-242 Hz`
- lower USB-child path exposed a stable semantic packet source
- lower USB-child path did not expose a comparable periodic cadence signal

Interpretation:

- the USB `02FF` child is useful for semantic observation
- it is not sufficient, by itself, to prove or control hardware-visible polling cadence
- the next lower-path candidate is the composite parent `USB\VID_045E&PID_0B12...`

Observed on the isolated composite-parent probe:

- parent stack:
  - `xboxgip -> dc1-controller -> GaYmFilter -> USBPcap -> ACPI -> USBHUB3`
- USB child:
  - `HidUsb -> xboxgip`
- HID child:
  - `GaYmXInputFilter -> xinputhid -> HidUsb`

Measured result:

- upper path still runs at roughly `250 Hz`
- lower parent path now shows a matching live cadence on internal control traffic
- observed lower cadence is approximately `124-127` internal-control requests per `500 ms`
- active semantic source remains:
  - `ioctl=0x00220003`
  - `len=17`

Updated interpretation:

- the composite parent is the first lower path that exposes a live repeating cadence signal comparable to the upper path
- this is the first credible candidate for real hardware-visible polling experimentation on this machine
- the next spike step is no longer discovery of the timing owner
- the next spike step is controlled perturbation of this parent-path cadence to see whether upper-visible polling behavior changes

## Controlled Perturbation Result

That perturbation experiment is now complete.

Method:

- enabled a fixed `5000 us` dev-only jitter on the lower parent probe
- targeted via:
  - `GAYM_CONTROL_TARGET=lower`
  - `GaYmCLI jitter 5000 5000`
- measured before and after with:
  - `CadenceProbe.exe 3 500 lower 0`
  - `CadenceProbe.exe 3 500 upper 0`

Observed baseline:

- lower parent cadence:
  - approximately `124-127` internal-control completions per `500 ms`
- upper cadence:
  - approximately `119-137` `0x8000E00C` device-control completions per `500 ms`

Observed with parent jitter enabled:

- lower parent cadence dropped to roughly `99-104` per `500 ms`
- upper cadence dropped to roughly `48-64` per `500 ms`

Conclusion:

- parent-path completion timing is not just observable
- parent-path completion timing is causal for the upper visible polling behavior on this machine
- this is the first successful below-HID timing intervention in the spike

That does not yet prove direct USB endpoint `bInterval` control.

It does prove that the composite parent path is the first viable place to modulate polling-visible behavior below the supported HID-child stack.

## Parent Jitter Sweep

The next step was to check whether that perturbation scaled smoothly or only appeared at the high end.

Measured with a fixed lower-target jitter sweep:

- `0 us`
- `1000 us`
- `2000 us`
- `3000 us`
- `5000 us`

Method:

- targeted via:
  - `GAYM_CONTROL_TARGET=lower`
  - `GaYmCLI jitter <value> <value>`
- sampled after each change with:
  - `CadenceProbe.exe 3 500 lower 0`
  - `CadenceProbe.exe 3 500 upper 0`

Observed averages per `500 ms` window:

| Lower jitter | Lower parent cadence | Upper cadence |
| --- | ---: | ---: |
| `0 us` | `125.5` | `121.0` |
| `1000 us` | `125.7` | `123.0` |
| `2000 us` | `125.5` | `122.8` |
| `3000 us` | `125.2` | `121.5` |
| `5000 us` | `113.0` | `71.2` |

Interpretation:

- the parent-path timing effect is real
- it is not approximately linear over this range
- `1000-3000 us` produced little or no stable change in upper visible cadence
- `5000 us` still produced a strong drop in both lower parent cadence and upper visible cadence

Current best read:

- the parent path is definitely causal
- the observable transfer function has a threshold or bursty behavior instead of a smooth proportional response
- if real hardware-visible poll control exists here, the next spike should focus on shaping or scheduling the delay more precisely rather than just increasing fixed stall time

## Shaped Scheduler Result

That next scheduling experiment is now complete.

Implementation change:

- replaced the crude per-completion busy stall with a per-device due-time scheduler on the parent probe
- the lower parent path now reserves the next completion slot under a spin lock
- at passive IRQL it sleeps the coarse portion with `KeDelayExecutionThread`
- it only uses `KeStallExecutionProcessor` for the short tail

Measured with the same lower-target sweep:

- `0 us`
- `1000 us`
- `2000 us`
- `3000 us`
- `4000 us`
- `5000 us`

Observed averages per `500 ms` window:

| Lower pacing interval | Lower parent cadence | Upper cadence |
| --- | ---: | ---: |
| `0 us` | `125.3` | `128.7` |
| `1000 us` | `125.5` | `126.7` |
| `2000 us` | `125.8` | `98.2` |
| `3000 us` | `126.5` | `85.7` |
| `4000 us` | `122.8` | `74.8` |
| `5000 us` | `98.3` | `49.5` |

Interpretation:

- the upper-visible effect is now materially smoother than the old fixed-stall experiment
- `2000-5000 us` produces a progressive drop in upper cadence instead of a near-flat response followed by a sudden cliff
- the lower parent cadence still resists change until the higher end of the range, which suggests the parent path is gating or batching requests before the effect fully shows up in its own counters

Current best read:

- shaping the parent-path delay is a better experimental lever than adding a blind fixed stall
- the composite parent is still the only lower path that has shown causal control over upper-visible polling behavior
- this is now strong evidence that any real hardware-polling research should stay on the composite-parent side, not the USB `02FF` child or the supported HID-child product line

## Explicit Target-Rate Experiment

The next step was to move one level above raw microsecond intervals and ask for a desired rate directly.

Implementation:

- kept the kernel surface unchanged
- added a user-mode `GaYmCLI rate ...` experiment on top of the shaped lower pacing knob
- the CLI now:
  - applies a fixed lower interval candidate through `IOCTL_GAYM_SET_JITTER`
  - measures the resulting lower and upper cadence from the live runtime counters
  - searches for the closest match to a requested parent or upper target rate
  - leaves the best interval applied at the end

Supported commands in maintainer builds:

- `GaYmCLI rate parent <hz>`
- `GaYmCLI rate upper <hz>`
- `GaYmCLI rate off`
- `GaYmCLI ratecurve [device_index] [sample_ms] [output_csv]`
- `GaYmCLI ratecurve-stats [runs] [device_index] [sample_ms] [output_csv]`

Observed validation results:

- `GaYmCLI rate parent 245`
  - best interval: `4000 us`
  - measured during sweep: parent `243.3 Hz`, upper `262.0 Hz`
- `GaYmCLI rate upper 150`
  - best interval: `3700 us`
  - measured during sweep: parent `249.3 Hz`, upper `151.3 Hz`

Direct probe after the `upper 150` run:

- lower parent cadence stayed close to baseline:
  - roughly `124-128` internal-control requests per `500 ms`
- upper cadence stayed near the requested target:
  - roughly `70-89` device-control completions per `500 ms`
  - approximately `140-178 Hz`

Interpretation:

- the explicit target-rate layer works better as a user-mode experiment than as another kernel ABI change
- asking for an upper target is now more practical than guessing a raw interval manually
- the lower parent counters still do not fully explain the upper-path response, which reinforces that the parent-side pacing effect propagates upstream in a non-trivial way

For machine-readable captures, `ratecurve` emits:

```text
interval_us,parent_hz,upper_hz
```

and disables the lower pacing experiment again when the sweep finishes.

The next layer now exists too:

- `ratecurve-stats` reruns the same sweep multiple times
- it emits mean/min/max per interval instead of a single noisy sample
- it also restores the lower pacing experiment to `off` when complete

First validation capture:

```text
GaYmCLI ratecurve-stats 2 0 500
```

Observed aggregated output:

| Interval | Parent mean | Parent min-max | Upper mean | Upper min-max |
| --- | ---: | ---: | ---: | ---: |
| `3500 us` | `250.0` | `248.0-252.0` | `224.0` | `168.0-280.0` |
| `4000 us` | `244.0` | `242.0-246.0` | `206.0` | `150.0-262.0` |
| `4500 us` | `217.0` | `212.0-222.0` | `175.0` | `112.0-238.0` |
| `5000 us` | `212.0` | `204.0-220.0` | `178.0` | `132.0-224.0` |

Interpretation:

- the parent-side cadence is comparatively stable once the interval gets large enough to matter
- the upper-visible cadence remains much noisier than the parent counters
- repeated sweeps are therefore necessary before claiming a stable transfer curve at the high end

## Guardrails

- Keep the stable product line untouched.
- Do not merge experimental attach-path changes into the supported branch without separate validation.
- Prefer measurement and proof over large speculative rewrites.

## Safe Cap Result

The next follow-up was to harden the parent probe after an unsafe run triggered a watchdog bugcheck during an aggressive `rate-stable` sweep.

Safety changes:

- parent probe pacing cap reduced from `5000 us` to `2500 us`
- non-passive completion-path stall capped to `100 us`
- `rate-stable` search narrowed to the conservative `0-2500 us` set

Observed result from a guarded live pass:

- `GaYmCLI rate-stable 100 2 0 500`
  - best interval: `2500 us`
  - parent mean: `252.0 Hz`
  - upper mean: `257.0 Hz`
  - upper range: `256.0-258.0 Hz`

Interpretation:

- the hardened version no longer reproduced the watchdog crash in the guarded test
- but it also no longer produced useful upper-rate control
- that strongly suggests the earlier effect depended on a level of inline completion delay that is not acceptable on the parent hot path

Current best read:

- inline parent-path delay shaping is a useful causality probe
- it is not yet a credible production or even semi-safe experimental rate-control mechanism
- the next serious research direction should move closer to the host-controller scheduling layer instead of pushing further on the current completion-delay design

## Host-Stack Direction

Independent review of the public `hidusbf` package points to a different architecture from the current spike:

- a per-device lower filter is used for targeting and deployment
- the real overclocking effect is tied to patching or altering the USB host stack path
  - `USBPORT.SYS`
  - `USBXHCI.SYS`
  - `IUSB3XHC.SYS`

That means the closest analogue to `hidusbf` is not "delay parent completions more carefully." It is:

- identify the active host-controller path for this machine
- fingerprint the exact controller, hub, and driver versions involved
- decide whether a host-stack patch or equivalent interception layer is even technically plausible on this target

To support that next step, the spike now includes:

- `scripts\capture-host-polling-context.ps1`

which captures:

- the current composite parent / USB child / HID child stack state
- the parent ancestry chain
- service image paths for the parent chain
- host-driver version information for key USB and controller binaries

First capture on this machine identified the live host path as:

- `dc1-controller -> USBHUB3 -> USBXHCI -> pci -> ACPI`

So the next host-stack research target on this box is the `USBXHCI` path, not `USBPORT.SYS`.

Follow-up recon narrowed that further:

- there are two present `USBXHCI` controllers on this machine
- the Xbox path is attached to:
  - `PCI\VEN_8086&DEV_7AE0&SUBSYS_86941043&REV_11\3&11583659&0&A0`
  - `Intel(R) USB 3.20 eXtensible Host Controller - 1.20 (Microsoft)`
- current active image:
  - `C:\Windows\System32\drivers\USBXHCI.SYS`
  - version `10.0.26100.2454`

The spike now includes a dedicated read-only capture for that layer:

- `scripts\capture-usbxhci-recon.ps1`

The next read-only step now exists too:

- `scripts\assess-usbxhci-patchability.ps1`

Current assessment on this machine:

- active host image:
  - `C:\Windows\System32\drivers\USBXHCI.SYS`
  - version `10.0.26100.2454`
  - SHA256 `B010BFE5944E1C339D0216537137A29F7BE8391B4F0A3729490E44B08D06AF55`
- target controller:
  - `PCI\VEN_8086&DEV_7AE0&SUBSYS_86941043&REV_11\3&11583659&0&A0`
  - `Intel(R) USB 3.20 eXtensible Host Controller - 1.20 (Microsoft)`
- documented `hidusbfn` Win11 `USBXHCI` support range from the local package readme:
  - `10.0.22000.1` through `10.0.22621.608`

Interpretation:

- host-stack work is still the right architectural layer if true `hidusbf`-style polling control is the goal
- but direct reuse of `hidusbf`/`hidusbfn` era `USBXHCI` assumptions is not validated on this box
- this machine is running a newer `USBXHCI.SYS` than the documented public patch range
- any future host-stack experiment therefore starts as a fresh reverse-engineering/recon task, not a straight reuse exercise

That recon task now has a first read-only implementation too:

- `scripts\capture-usbxhci-symbol-recon.ps1`

Current `USBXHCI.SYS` symbol/pattern reconnaissance on this machine found:

- PE identity:
  - `AMD64`
  - `PE32+`
  - subsystem `Native`
  - entrypoint RVA `0x00055B30`
- CodeView debug anchor:
  - format `RSDS`
  - PDB path `usbxhci.pdb`
- stable section anchors:
  - `.text` SHA256 `F25F49E605032AB34F10C735733E65F86E1572E97E0476FB647AD26AFB3BF524`
  - `PAGE` SHA256 `BAB60678385351CE4D232D30EED1F4C541A50FD7737D6220E90512C9CE5D15D6`
  - `.idata` SHA256 `316B925A83B5593141864F5364984D0799A05C79D985514CEF860B0E1A9C996D`
- import surface:
  - `HAL.dll`
  - `ntoskrnl.exe`
  - `WDFLDR.SYS`
  - `WppRecorder.sys`
- keyword-bearing internal strings and source-path anchors:
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\endpoint.c`
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\controller.c`
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\command.c`
  - `onecore\drivers\wdm\usb\usb3\usbxhci\sys\tr.c`
  - `INTERRUPTER_DATA`
  - `ENDPOINT_DATA`
  - `Transfer Ring Tag`

Interpretation:

- a future host-stack experiment is still high-risk, but it is no longer blind
- this image exposes enough internal naming and structural anchors to support deeper offline reverse engineering if we choose to go there
- the next stage, if pursued, should stay read-only and build function/feature maps from these anchors before any attempt to intercept or patch host-controller behavior

That next read-only step now exists too:

- `scripts\capture-usbxhci-feature-map.ps1`

Current feature-map result on this machine:

- runtime function count:
  - `1294`
- anchor categories:
  - controller/slot/root-hub anchors in `.rdata` from `0x0005BA7F` through `0x0005E530`
  - endpoint anchors from `0x0005B6F7` through `0x000618C8`
  - ring anchors from `0x0005B5C0` through `0x00060700`
  - transfer anchors from `0x0005B5C0` through `0x00061530`
  - interrupter anchors from `0x0005BF18` through `0x00070E74`
- candidate function bands:
  - controller: `0x00001348 .. 0x00078BE0`
  - endpoint: `0x00007D60 .. 0x00054F74`
  - ring: `0x0000A398 .. 0x0005346C`
  - transfer: `0x00002964 .. 0x00054E60`
  - interrupter: `0x000410B4 .. 0x00081E4D`

Notable clusters from the heuristic pass:

- controller/slot/root-hub:
  - `0x0000A640-0x0000A7F4`
  - `0x0000B2A0-0x0000B472`
  - `0x00018AD4-0x00018DCB`
  - `0x00026440-0x000277A7`
- endpoint:
  - `0x00007D60-0x0000813A`
  - `0x00008250-0x0000844D`
  - `0x000318C4-0x00031987`
  - `0x00035038-0x000352F6`
- ring/transfer:
  - `0x0000A398-0x0000A62F`
  - `0x00019F40-0x00019FE8`
  - `0x0003D690-0x0003DF6D`
  - `0x00052A74-0x00052C46`
- interrupter:
  - `0x000410B4-0x00041382`
  - `0x0007B5D0-0x0007BAF3`
  - `0x00081980-0x00081E4D`

Interpretation:

- we now have a defensible image-specific shortlist of likely code regions for controller, endpoint, ring/transfer, and interrupter behavior
- that is enough to support a deeper offline mapping pass without touching live host-stack behavior
- the next read-only stage, if pursued, should focus on correlating these candidate functions with unwind boundaries, section placement, and import usage to separate control-plane logic from hot interrupt/transfer paths

That next read-only stage now exists too:

- `scripts\capture-usbxhci-cluster-profile.ps1`

Current cluster-profile result on this machine:

- candidate functions profiled from the current feature-map shortlist:
  - `70`
- direct RIP-relative IAT call/jump hits:
  - `1333`
- IAT entries parsed:
  - `146`

Key separation signals:

- controller/slot/root-hub shortlist:
  - top imports include `KeGetCurrentIrql`, `KeQueryUnbiasedInterruptTime`, timer APIs, and `KeDelayExecutionThread`
  - strongest control-plane candidates include:
    - `0x0001B1F0-0x0001B6D9`
    - `0x0003634C-0x000366BC`
    - `0x0000DC30-0x0000DE2D`
- transfer shortlist:
  - strongest import pattern is `KeAcquireSpinLockRaiseToDpc` / `KeReleaseSpinLock`
  - strongest hot-path transfer candidates include:
    - `0x0000320D-0x000038AE`
    - `0x0001144D-0x00011916`
    - `0x000077FC-0x00007B61`
- ring shortlist:
  - strongest import pattern is `DbgPrintEx`, with some `KeStallExecutionProcessor`
  - strongest ring/control transition candidates include:
    - `0x00052A74-0x00052C46`
    - `0x000528DC-0x00052A6B`
    - `0x0000A398-0x0000A62F`
    - `0x0003D690-0x0003DF6D`
- interrupter shortlist:
  - two strongest candidates are pageable `PAGE` functions
  - imports include `IoAllocateWorkItem`, `ExAllocatePool2`, `KeInitializeEvent`, and `WppRecorder`
  - strongest interrupter candidates include:
    - `0x0007B5D0-0x0007BAF3`
    - `0x00081980-0x00081E4D`
    - with one nonpageable `.text` candidate at `0x000410B4-0x00041382`

Interpretation:

- the read-only map is now good enough to distinguish likely hot transfer logic from controller-side orchestration and pageable interrupter/work-item code
- if a deeper offline reverse-engineering pass is pursued, the highest-value next targets are the transfer and ring clusters above, not the entire image
- the host-stack experiment is still not justified yet, but the candidate scope is now small enough to make targeted offline study realistic

That next read-only stage now exists too:

- `scripts\capture-usbxhci-targeted-callmap.ps1`

Current targeted call-map result on this machine:

- selected features:
  - `Transfer`
  - `Ring`
- selected top candidates:
  - `4` per feature
- captured direct call sites:
  - `92`

Key targeted findings:

- top transfer-event clusters are not isolated islands
  - `0x0000320D-0x000038AE` fans into shared `.text` helpers, especially:
    - `0x00058B00`
    - `0x00006BA0`
    - `0x00004124`
    - `0x000042A0`
  - `0x0001144D-0x00011916` similarly fans into:
    - `0x00010440`
    - `0x00012400`
    - `0x00011B00`
    - `0x00058B00`
  - direct imported edges remain dominated by:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
- the selected ring clusters are much more self-contained and debug-oriented
  - `0x000528DC-0x00052A6B`, `0x00052A74-0x00052C46`, and `0x0005346C-0x000535A4` each have one direct internal edge back into the nearby `0x00052254` helper
  - `0x00052F78-0x00053280` has the only same-feature edge in the narrowed set:
    - `Ring:0x0005346C`
  - imported edges are overwhelmingly:
    - `DbgPrintEx`
  - with one notable direct `HAL.dll!KeStallExecutionProcessor` site in:
    - `0x00052A74-0x00052C46`

Interpretation:

- the transfer-event regions remain the strongest offline study targets if the goal is timing or cadence control, because they connect to shared nonpageable helper code and hot spinlock paths
- the top ring/TRB regions look more like localized command/diagnostic machinery than the best first place to search for a host-level polling lever
- if a future offline reverse-engineering pass goes deeper, the next narrow target set should start with:
  - `0x0000320D-0x000038AE`
  - `0x000077FC-0x00007B61`
  - `0x0001144D-0x00011916`
  - plus the shared helper anchors they call most often

That next read-only stage now exists too:

- `scripts\capture-usbxhci-helper-callmap.ps1`

Current helper-centric result on this machine:

- transfer-derived helper seeds discovered from the targeted map:
  - `27`
- selected helper functions mapped:
  - `6`
- captured helper call sites:
  - `45`

Top helper convergence findings:

- the transfer side really does converge into a shared helper tier
  - strongest seed:
    - `0x00058B00`
    - total inbound transfer calls: `7`
    - unique transfer callers: `2`
  - next strongest workhorse helpers:
    - `0x00006BA0`
    - `0x00010440`
    - both with `3` inbound calls from transfer-event regions
- the most important helper-to-helper edges are:
  - `0x00006BA0 -> 0x00058B00` (`3` calls)
  - `0x00010440 -> 0x00058B00` (`3` calls)
  - `0x00004124 -> 0x00058B00` (`1` call)

What that means structurally:

- `0x00058B00` is not a rich standalone subsystem
  - it is only `6` bytes long in the current image
  - no direct outgoing internal or IAT calls were captured there
  - so it looks more like a tiny shared thunk or dispatch shim than the real timing-control body
- the higher-value helper bodies are the callers that collapse into it
  - `0x00006BA0` carries:
    - repeated `KeAcquireSpinLockRaiseToDpc`
    - repeated `KeReleaseSpinLock`
    - `IoQueueWorkItem`
    - `KeGetCurrentIrql`
  - `0x00010440` carries:
    - repeated `KeAcquireSpinLockRaiseToDpc`
    - repeated `KeReleaseSpinLock`
    - `KfRaiseIrql`
    - `KeLowerIrql`
    - `IoFreeMdl`

Interpretation:

- the transfer-event regions do not immediately fan back out into unrelated code
- they converge first into a small helper tier, and that tier itself converges on a tiny shared thunk at `0x00058B00`
- so the next serious offline target set is no longer the original transfer functions alone
- it is:
  - `0x00006BA0`
  - `0x00010440`
  - `0x00004124`
  - with `0x00058B00` treated as a likely shared dispatch endpoint rather than the main body of interest

That next read-only stage now exists too:

- `scripts\capture-usbxhci-helper-micromap.ps1`

Current micro-map result on this machine:

- requested helpers:
  - `0x00006BA0`
  - `0x00010440`
  - `0x00004124`
- resolved helpers:
  - `3`
- neighbor radius:
  - `2`
- second-hop targets per helper:
  - `4`

Key structural findings:

- `0x00006BA0` is a real local body, not just a jump point
  - sits inside a contiguous local band with:
    - `0x00006A44`
    - `0x00006E74`
    - `0x00007160`
  - still imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `IoQueueWorkItem`
    - `KeGetCurrentIrql`
  - second-hop shows two distinct directions:
    - local continuation into `0x00006E74` and `0x00007160`
    - separate control/error branch into `0x00040D38`
- `0x00010440` is the other substantial inner body
  - sits in a larger local band with:
    - `0x000103CF`
    - `0x00010CD8`
    - `0x00010D60`
  - still imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `KfRaiseIrql`
    - `KeLowerIrql`
    - `IoFreeMdl`
  - second-hop splits into:
    - local continuation through `0x00011240`
    - a different control-plane looking branch through `0x00022E7C`
- `0x00004124` is much thinner than the other two
  - one direct internal edge to `0x00058B00`
  - one `WppAutoLogTrace` import
  - nearby neighbors include `0x000042A0`, which was already visible from the transfer-event layer

Most important second-hop details:

- `0x00006E74`
  - nonpageable
  - no direct IAT edges in this pass
  - pushes further into:
    - `0x00008454`
    - `0x00054F74`
- `0x00040D38`
  - looks more like an assert/control path
  - imports:
    - `KeBugCheckEx`
    - `KeGetCurrentProcessorNumberEx`
- `0x00011240`
  - collapses back into `0x00058B00`
  - plus `WppAutoLogTrace`
- `0x00022E7C`
  - branches into:
    - `0x0001A7FC`
    - `0x0000DA20`
    - `0x0000DC30`
  - imports:
    - `KeGetCurrentIrql`
    - `VslDeleteSecureSection`

Interpretation:

- the helper tier does not fan back out randomly
- it narrows to two real bodies of interest:
  - `0x00006BA0`
  - `0x00010440`
- `0x00004124` is better treated as a thin wrapper than a primary reverse-engineering target
- the most valuable next offline targets are now:
  - `0x00006E74`
  - `0x00011240`
  - `0x00022E7C`
  - and the shared thunk endpoint `0x00058B00` only as a routing landmark

That next read-only stage now exists too:

- `scripts\capture-usbxhci-branch-split.ps1`

Current branch-split result on this machine:

- assessed second-hop targets:
  - `0x00006E74`
  - `0x00011240`
  - `0x00022E7C`

Resulting split:

- `0x00006E74`
  - classification:
    - `likely hot-path continuation`
  - basis:
    - stays in nonpageable `.text`
    - no direct IAT edges in this pass
    - internal targets stay inside `.text`:
      - `0x00008454`
      - `0x00054F74`
- `0x00011240`
  - classification:
    - `instrumented wrapper`
  - basis:
    - one direct internal edge back into `0x00058B00`
    - one direct import:
      - `WppAutoLogTrace`
- `0x00022E7C`
  - classification:
    - `control/assert drift`
  - basis:
    - reaches controller candidate:
      - `Controller:0x0000DC30`
    - imports:
      - `KeGetCurrentIrql`
      - `VslDeleteSecureSection`

Interpretation:

- the branch split is now specific enough to prioritize deeper study
- `0x00006E74` is the only one of the three that still looks like a credible continuation of a hot transfer-side path
- `0x00011240` is likely useful for instrumentation context, but not the main timing lever
- `0x00022E7C` now looks clearly off the main timing path and into control/security/assert territory

So the next clean offline target is no longer a set of three.
It is primarily:

- `0x00006E74`

with secondary context only from:

- `0x00011240`

That next read-only stage now exists too:

- `scripts\capture-usbxhci-single-target-deep.ps1`

Current single-target result on this machine:

- primary target:
  - `0x00006E74`
- direct internal follow-on targets:
  - `0x00008454`
  - `0x00054F74`

What that deeper pass shows:

- `0x00006E74`
  - sits in a tight local nonpageable neighborhood with:
    - `0x00006A44`
    - `0x00006BA0`
    - `0x00007160`
    - `0x000071C8`
  - still has no direct IAT edges
  - only calls:
    - `0x00008454` twice
    - `0x00054F74` once
- `0x00008454`
  - lands directly inside the endpoint neighborhood
  - nearest known endpoint candidates are:
    - `0x00008250-0x0000844D` (`Reset Endpoint`)
    - `0x000085E0-0x000087BB` (`Stop Endpoint`)
  - only fans into:
    - `0x00058B00`
  - carries one:
    - `WppAutoLogTrace`
- `0x00054F74`
  - lands at the upper edge of the endpoint function band and immediately above a transfer candidate:
    - nearest transfer candidate:
      - `0x00054E60-0x00054F1D`
  - but its own direct imports are:
    - `DbgPrint`
    - `KdRefreshDebuggerNotPresent`
  - and its direct internal calls go to:
    - `0x00018934`
    - `0x0002C7F8`

Interpretation:

- `0x00006E74` still looks like the right hot-path continuation to study
- but its two exits are now split cleanly:
  - `0x00008454` is the stronger endpoint-side continuation
  - `0x00054F74` looks more like a debug-heavy boundary/helper near the transfer/endpoint seam
- so the next best offline target is no longer generic `0x00006E74`
- it is specifically:
  - `0x00008454`

with `0x00054F74` kept only as boundary context.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-endpoint-target-deep.ps1`

Current endpoint-target result on this machine:

- primary target:
  - `0x00008454`
- direct handoff target:
  - `0x00058B00`

What that deeper pass shows:

- `0x00008454`
  - still sits inside a tight endpoint neighborhood:
    - `0x00008250-0x0000844D`
    - `0x000085E0-0x000087BB`
  - has only:
    - one direct internal call to `0x00058B00`
    - one direct `WppAutoLogTrace` import
- `0x00058B00`
  - is a `6`-byte `.text` function
  - has no direct internal calls
  - has no direct IAT calls
  - sits far away from the current transfer/endpoint candidate cluster set

Interpretation:

- `0x00008454` still looks endpoint-adjacent, not like an isolated pure tracing stub
- but the visible direct handoff from it is only a tiny shared thunk at `0x00058B00`
- that makes `0x00058B00` less interesting as a next study target than the neighboring endpoint bodies around `0x00008454`

So the next clean offline targets are now:

- `0x00008250-0x0000844D`
- `0x000085E0-0x000087BB`

with `0x00058B00` kept only as a routing marker.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-endpoint-neighbor-compare.ps1`

Current endpoint-neighbor comparison on this machine:

- compared bodies:
  - `0x00008250-0x0000844D`
  - `0x000085E0-0x000087BB`
- both are similarly sized
- both have:
  - `8` direct internal calls
  - `0` direct IAT calls

Shared direct internal targets:

- `0x000049B4`
- `0x00005BC0`
- `0x00006A08`
- `0x00006A44`
- `0x0001F9A4`

Unique split:

- `0x00008250` only:
  - `0x0001BF58`
  - `0x000331C8`
- `0x000085E0` only:
  - `0x000087C4`
  - `0x0002BED0`

Interpretation:

- the two neighboring endpoint bodies are both substantive
- they are not separate isolated side paths
- they converge into a shared helper set, which is now more interesting than treating either endpoint body alone as the next reverse-engineering target
- two of those shared targets also line up with earlier hot/helper context:
  - `0x000049B4`
  - `0x00006A44`

So the next clean offline targets are now the shared helper tier, starting with:

- `0x0001F9A4`
- `0x00006A44`
- `0x000049B4`

That next read-only stage now exists too:

- `scripts\capture-usbxhci-shared-helper-assessment.ps1`

Current shared-helper assessment on this machine:

- assessed helpers:
  - `0x0001F9A4`
  - `0x00006A44`
  - `0x000049B4`
- recommended next:
  - `0x00006A44`

Why:

- `0x0001F9A4`
  - behaves like an instrumented wrapper
  - one internal handoff to:
    - `0x00058B00`
  - one direct import:
    - `WppAutoLogTrace`
- `0x000049B4`
  - behaves like a debug-heavy seam
  - direct imports:
    - `DbgPrint`
    - `KdRefreshDebuggerNotPresent`
- `0x00006A44`
  - still behaves like a hot helper continuation
  - direct internal edges:
    - `0x00006BA0`
    - `0x00007160`
    - `0x00058B00`
  - direct imports:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`

Interpretation:

- the shared-helper tier now resolves cleanly into one remaining hot candidate:
  - `0x00006A44`
- and that candidate feeds directly back into the earlier hot helper band:
  - `0x00006BA0`
  - `0x00007160`

So the next clean offline targets are now:

- `0x00006A44`

with immediate follow-on context in:

- `0x00006BA0`
- `0x00007160`

That next read-only stage now exists too:

- `scripts\capture-usbxhci-hot-helper-band-assessment.ps1`

Current hot-helper-band assessment on this machine:

- assessed band:
  - `0x00006A44`
  - `0x00006BA0`
  - `0x00007160`
- recommended next:
  - `0x00006BA0`

Role split:

- `0x00006A44`
  - entry helper
  - points into:
    - `0x00006BA0`
    - `0x00007160`
- `0x00006BA0`
  - primary hot body
  - strongest import profile in the band:
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeReleaseSpinLock`
    - `IoQueueWorkItem`
    - `KeGetCurrentIrql`
  - internal edges into:
    - `0x00006E74`
    - `0x00007160`
    - `0x0000757C`
    - `0x00040D38`
- `0x00007160`
  - thin tail/thunk path
  - only direct internal edge:
    - `0x00058B00`

Interpretation:

- the surviving hot-helper band no longer needs to be treated as three equal candidates
- `0x00006A44` is the feeder into the band
- `0x00007160` is a thin exit
- `0x00006BA0` is the actual body worth deeper offline study

So the next clean offline target is now:

- `0x00006BA0`

with immediate follow-on context in:

- `0x00006E74`
- `0x0000757C`

and `0x00040D38` kept only as the likely control/assert side branch.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-hot-body-branch-assessment.ps1`

Current hot-body branch assessment on this machine:

- assessed branches:
  - `0x00006E74`
  - `0x0000757C`
  - `0x00040D38`
- recommended next:
  - `0x00006E74`

Role split:

- `0x00006E74`
  - endpoint-side hot continuation
  - keeps real internal fan-out into:
    - `0x00008454`
    - `0x00054F74`
- `0x0000757C`
  - quiet transfer bridge
  - no direct internal or IAT edges in this pass
  - but its immediate neighbors advance into:
    - `0x000076A0`
    - `0x000077FC-0x00007B61`
- `0x00040D38`
  - control/assert branch
  - imports:
    - `KeBugCheckEx`
    - `KeGetCurrentProcessorNumberEx`

Interpretation:

- the outward branches from `0x00006BA0` are no longer ambiguous
- `0x00006E74` is still the only branch with substantive hot-path structure
- `0x0000757C` is worth keeping only as a transfer-side bridge marker
- `0x00040D38` remains demoted to control/assert context

So the next clean offline target is now:

- `0x00006E74`

with immediate follow-on context in:

- `0x00008454`
- `0x00054F74`

and `0x0000757C` kept only as a bridge marker into the nearby transfer-event region.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-endpoint-vs-transfer-assessment.ps1`

Current endpoint-vs-transfer comparison on this machine:

- endpoint-side continuation:
  - `0x00008454`
- transfer-side cluster:
  - `0x000077FC`
- recommended next:
  - `0x000077FC`

Why:

- `0x00008454`
  - is thin and instrumented
  - one direct internal handoff to:
    - `0x00058B00`
  - one direct import:
    - `WppAutoLogTrace`
- `0x000077FC`
  - is a much richer transfer-side body
  - direct internal fan-out includes:
    - `0x00003FA0`
    - `0x00003C70`
    - `0x00004124`
    - `0x000049B4`
    - `0x00005BC0`
    - `0x00007B70`
    - `0x00008878`
  - direct imports include:
    - `KeReleaseSpinLock`
    - `KeAcquireSpinLockRaiseToDpc`

Interpretation:

- `0x00006E74` is still the important split point out of `0x00006BA0`
- but if the goal is the richer next body for polling/scheduling recon, the bridge-side transfer cluster is now more valuable than the endpoint-side wrapper leg

So the next clean offline target is now:

- `0x000077FC`

with immediate follow-on context in:

- `0x00007B70`
- `0x00008878`

and `0x00008454` kept only as the thinner endpoint-side continuation.

That larger read-only stage now exists too:

- `scripts\capture-usbxhci-transfer-cluster-batch.ps1`

Current transfer-cluster batch result on this machine:

- tested neighborhood:
  - `0x000076A0`
  - `0x000077FC`
  - `0x00007B70`
  - `0x00007D60`
  - `0x00008878`
- recommended next:
  - `0x00007D60`

Ranking:

- `0x00007D60`
  - score `2286`
  - `13` direct internal
  - `0` direct IAT
  - size `986`
- `0x000077FC`
  - score `1994`
  - `10` direct internal
  - `5` direct IAT
  - size `869`
- `0x00008878`
  - score `866`
- `0x00007B70`
  - score `612`
- `0x000076A0`
  - score `465`

Why `0x00007D60` won:

- it is the densest body in the tested transfer-side neighborhood
- it reconnects directly into the shared helper tier:
  - `0x00006A08`
  - `0x00006A44`
  - `0x0001F9A4`
  - `0x0001BF58`
  - `0x000049B4`
  - `0x00005BC0`
- it also advances locally into:
  - `0x00008140`
  - `0x00008180`

Interpretation:

- the larger batch test did exactly what it was supposed to do
- it showed that `0x000077FC` is rich, but not the best next target in its own neighborhood
- `0x000076A0` and `0x00007B70` look more like thin WPP/thunk-style edge bodies
- `0x00008878` is substantial but still secondary

So the next clean offline target is now:

- `0x00007D60`

with immediate follow-on context in:

- `0x00008140`
- `0x00008180`

and `0x000077FC` kept as the upstream transfer-side feeder body.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-transfer-follow-assessment.ps1`

Current `0x00007D60` follow-on assessment on this machine:

- compared follow-ons:
  - `0x00008140`
  - `0x00008180`
- recommended next:
  - `0x00008180`

Why:

- `0x00008140`
  - is tiny:
    - size `55`
  - only one direct internal edge:
    - `0x00008DA0`
- `0x00008180`
  - is materially denser:
    - size `196`
    - `3` direct internal edges
  - fan-out includes:
    - `0x00008E74` (`2` calls)
    - `0x00046530`

Interpretation:

- the `0x00007D60` follow-on split does not produce two equally interesting branches
- `0x00008140` looks like a thin edge helper
- `0x00008180` is the only one that still has enough structure to justify deeper offline work

So the next clean offline target is now:

- `0x00008180`

with immediate follow-on context in:

- `0x00008E74`
- `0x00046530`

and `0x00008140` kept only as the thinner sibling edge.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-transfer-next-hop-assessment.ps1`

Current `0x00008180` next-hop assessment on this machine:

- compared next hops:
  - `0x00008E74`
  - `0x00046530`
- recommended next:
  - `0x00046530`

Why:

- `0x00008E74`
  - is effectively a stub:
    - size `17`
    - `0` direct internal
    - `0` direct IAT
- `0x00046530`
  - is the only substantive continuation:
    - size `333`
    - `5` direct internal
  - fan-out includes:
    - `0x0001A7FC`
    - `0x00019AC8`
    - `0x0001AD7C`
    - `0x00058AC0`

Interpretation:

- the `0x00008180` split is not ambiguous
- `0x00008E74` does not justify deeper follow-up
- `0x00046530` is the first real continuation beyond the `0x00007D60 -> 0x00008180` path

So the next clean offline target is now:

- `0x00046530`

with immediate follow-on context in:

- `0x0001A7FC`
- `0x00019AC8`
- `0x0001AD7C`

and `0x00008E74` kept only as the tiny sibling stub.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-transfer-body-follow-assessment.ps1`

Current `0x00046530` follow-on assessment on this machine:

- compared next hops:
  - `0x0001A7FC`
  - `0x00019AC8`
  - `0x0001AD7C`
- recommended next:
  - `0x0001AD7C`

Why:

- `0x0001A7FC`
  - is a thin instrumented leg
  - one direct internal handoff to:
    - `0x00058B00`
  - one direct import:
    - `WppAutoLogTrace`
- `0x00019AC8`
  - is a small debug-leaning helper
  - one direct internal handoff to:
    - `0x00045A8C`
  - one direct import:
    - `KdRefreshDebuggerNotPresent`
- `0x0001AD7C`
  - is the only substantive continuation:
    - size `852`
    - `10` direct internal
    - `5` direct IAT
  - internal fan-out includes:
    - `0x0001B0D8`
    - `0x0001B158`
    - `0x00055790`
    - `0x00055864`
    - `0x0000DA20`
    - `0x0000DC30`

Interpretation:

- the `0x00046530` split is no longer ambiguous
- `0x0001A7FC` and `0x00019AC8` should both be demoted to side-context
- `0x0001AD7C` is the only branch with enough structure to keep following

So the next clean offline target is now:

- `0x0001AD7C`

with immediate follow-on context in:

- `0x0001B0D8`
- `0x0001B158`
- `0x00055790`
- `0x00055864`

and `0x0000DA20` / `0x0000DC30` kept as the likely control-side hooks.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-transfer-tail-band-assessment.ps1`

Current `0x0001AD7C` tail-band assessment on this machine:

- compared tails:
  - `0x0001B0D8`
  - `0x0001B158`
- compared trace siblings:
  - `0x00055790`
  - `0x00055864`
- compared shared direct targets:
  - `0x00058AC0`
  - `0x0000C8C0`
- compared local substantive neighbor:
  - `0x0001B1F0`
- recommended next:
  - `0x0001B1F0`

Why:

- `0x0001B0D8`
  - stays a thin non-instrumented tail
  - only meaningful direct targets:
    - `0x00058AC0`
    - `0x0000C8C0`
- `0x0001B158`
  - is the same kind of thin tail sibling
  - same direct targets:
    - `0x00058AC0`
    - `0x0000C8C0`
- `0x00055790`
  - is trace-heavy side-context
  - one direct internal handoff to:
    - `0x00058B00`
  - one direct import:
    - `WppAutoLogTrace`
- `0x00055864`
  - is the same trace-heavy sibling pattern
  - one direct internal handoff to:
    - `0x00058B00`
  - one direct import:
    - `WppAutoLogTrace`
- `0x00058AC0`
  - collapses into a tiny stub:
    - size `30`
    - `0` direct internal
    - `0` direct IAT
- `0x0000C8C0`
  - collapses into ETW-side helper context:
    - size `91`
    - `0` direct internal
    - one direct import:
      - `EtwWriteTransfer`
- `0x0001B1F0`
  - is the first substantive neighboring body:
    - size `1257`
    - `24` direct internal
    - `11` direct IAT
  - carries timing/orchestration imports:
    - `KeQueryUnbiasedInterruptTime`
    - `KeStallExecutionProcessor`
    - `ExAllocateTimer`
    - `ExSetTimer`
    - `ExDeleteTimer`
    - `KeWaitForSingleObject`

Interpretation:

- the `0x0001AD7C` line does not keep getting more interesting through the direct tail-call pair
- those tails mostly collapse into stub and ETW-side helpers
- the only remaining substantive continuation near that band is the adjacent timer/orchestration body at `0x0001B1F0`

So the next clean offline target is now:

- `0x0001B1F0`

with immediate follow-on context in:

- `0x0000D210`
- `0x0001BA28`
- `0x0001BA64`
- `0x0001BA8C`

and `0x00058AC0` / `0x0000C8C0` demoted to stub-and-trace side context.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-timer-body-follow-assessment.ps1`

Current `0x0001B1F0` follow-on assessment on this machine:

- direct bridge:
  - `0x0000D210`
- alternate local wrapper ladder:
  - `0x0001BA28`
  - `0x0001BA64`
  - `0x0001BA8C`
- first substantive bodies:
  - `0x0000D258`
  - `0x0001BAC0`
- recommended next:
  - `0x0000D258`
- secondary track:
  - `0x0001BAC0`

Why:

- `0x0000D210`
  - is only a bridge:
    - size `65`
    - one direct internal handoff to:
      - `0x0000D258`
- `0x0001BA28`
  - is a tiny wrapper:
    - size `54`
    - one direct internal handoff to:
      - `0x0001CBB4`
- `0x0001BA64`
  - is a tiny local bridge:
    - size `31`
    - one direct internal handoff to:
      - `0x0001BA8C`
- `0x0001BA8C`
  - collapses immediately:
    - size `34`
    - self-loop only
- `0x0000D258`
  - is the direct substantive continuation:
    - size `664`
    - `5` direct internal
    - `2` direct IAT
  - reconnects into the earlier transfer-side line:
    - `0x0001A7FC`
    - `0x0001AD7C`
  - and carries pool-management imports:
    - `ExAllocatePool2`
    - `ExFreePoolWithTag`
- `0x0001BAC0`
  - is the first real body on the local alternate side:
    - size `363`
    - `6` direct internal
    - `0` direct IAT
  - fan-out includes:
    - `0x0001BF58`
    - `0x0001BC34`
    - `0x0001F9A4`
    - `0x00005BC0`
    - `0x00006A08`

Interpretation:

- the immediate `0x0001B1F0` split is no longer ambiguous
- the direct bridge side through `0x0000D210` reaches substantive code faster
- the `0x0001BA28` side only becomes interesting again after a tiny wrapper ladder, so it should be treated as secondary

So the next clean offline target is now:

- `0x0000D258`

with immediate follow-on context in:

- `0x0001A7FC`
- `0x0001AD7C`
- `0x00058AC0`
- `0x00058BC0`

and `0x0001BAC0` kept as the secondary local branch.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-next-five-assessment.ps1`

Current five-target assessment on this machine:

- primary line:
  - `0x0000D258`
  - `0x0000D59C`
- secondary line:
  - `0x0001BAC0`
  - `0x0001BF58`
- opaque side candidate:
  - `0x00058BC0`
- recommended next:
  - `0x0000D59C`
- secondary track:
  - `0x0001BAC0`

Why:

- `0x0000D258`
  - remains the primary body that was worth following
  - but the next richer unresolved step behind it is now:
    - `0x0000D59C`
- `0x0000D59C`
  - is the strongest unresolved continuation in this set:
    - size `621`
    - `7` direct internal
    - `1` direct IAT
  - keeps timing-sensitive context:
    - `KeStallExecutionProcessor`
  - fan-out includes:
    - `0x0001BF58`
    - `0x0000D210`
    - `0x0001A7FC`
    - `0x00056D8C`
- `0x0001BAC0`
  - remains the best alternate branch body:
    - size `363`
    - `6` direct internal
    - `0` direct IAT
  - fan-out includes:
    - `0x0001BF58`
    - `0x0001BC34`
    - `0x0001F9A4`
    - `0x00005BC0`
    - `0x00006A08`
- `0x0001BF58`
  - demotes to trace-heavy follow-on:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00058BC0`
  - stays opaque:
    - size `682`
    - `0` direct internal
    - `0` direct IAT
  - so it is not the best next deep target even though it is large

Interpretation:

- the primary line through `0x0000D258` is still the right place to push deeper
- the immediate next best target is `0x0000D59C`
- `0x0001BAC0` should stay alive as the secondary branch
- `0x0001BF58` and `0x00058BC0` should both be deprioritized for now

So the next clean offline target is now:

- `0x0000D59C`

with immediate follow-on context in:

- `0x0001BF58`
- `0x0000D210`
- `0x0001A7FC`
- `0x00056D8C`

and `0x0001BAC0` kept as the secondary branch body.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-d59c-follow-assessment.ps1`

Current `0x0000D59C` follow-on assessment on this machine:

- compared follow-ons:
  - `0x0001BF58`
  - `0x0000D210`
  - `0x0001A7FC`
  - `0x00056D8C`
  - `0x00056DBC`
- recommended next:
  - `0x00056DBC`

Why:

- `0x0001BF58`
  - demotes to trace-heavy side context:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x0000D210`
  - stays only a bridge:
    - one direct internal handoff to:
      - `0x0000D258`
- `0x0001A7FC`
  - stays an instrumented side leg:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00056D8C`
  - is only a short bridge:
    - one direct internal handoff to:
      - `0x00056DBC`
- `0x00056DBC`
  - is the first substantive body left on this branch:
    - size `2125`
    - `20` direct internal
    - `11` direct IAT
  - direct imports include:
    - `KeGetCurrentIrql`
    - `KeAcquireSpinLockRaiseToDpc`
    - `KeQueryTimeIncrement`
    - `KeReleaseSpinLock`
    - `DbgkWerCaptureLiveKernelDump`
    - `ExAllocatePool2`

Interpretation:

- the `0x0000D59C` split is no longer ambiguous
- three of the four old follow-ons are now clearly demoted:
  - trace-heavy
  - instrumented
  - or bridge-only
- the next real unresolved body is `0x00056DBC`

So the next clean offline target is now:

- `0x00056DBC`

with immediate follow-on context in:

- `0x00001008`
- `0x0000103C`
- `0x00001068`
- `0x00056B50`
- `0x00058AC0`

and `0x0001BAC0` kept as the secondary branch body.

That next read-only stage now exists too:

- `scripts\capture-usbxhci-56dbc-band-assessment.ps1`

Current `0x00056DBC` band assessment on this machine:

- stub band:
  - `0x00001008`
  - `0x0000103C`
  - `0x00001068`
  - `0x00058AC0`
- helper band:
  - `0x00056B50`
  - `0x00056BA0`
  - `0x00056CA4`
  - `0x00079C58`
- adjacent bodies:
  - `0x00057610`
  - `0x00057748`
  - `0x000585E4`
- recommended next:
  - `0x0001BAC0`
- primary band state:
  - registration/debug drift

Why:

- `0x00001008` and `0x0000103C`
  - are tiny stubs:
    - no direct internal
    - no direct IAT
- `0x00001068`
  - is ETW-side stub context:
    - one direct import:
      - `EtwWriteTransfer`
- `0x00058AC0`
  - remains a tiny stub:
    - no direct internal
    - no direct IAT
- `0x00056B50`
  - is only a tiny bridge
- `0x00056BA0`
  - is a path/string helper:
    - `IoQueryFullDriverPath`
    - `RtlInitUnicodeString`
    - `RtlUnicodeStringToAnsiString`
    - `RtlFreeAnsiString`
- `0x00056CA4`
  - mixes allocation/spinlock setup with side-context fan-out:
    - `0x00079C58`
    - `0x00058BC0`
- `0x00079C58`
  - is ETW registration context:
    - `EtwRegister`
    - `EtwSetInformation`
- `0x00057610`
  - is ETW unregister / cleanup context:
    - `EtwUnregister`
    - `ExFreePoolWithTag`
- `0x00057748`
  - is debug/registry-heavy context:
    - `DbgPrintEx`
    - `ZwQueryKey`
    - `ExAllocatePoolWithTag`
- `0x000585E4`
  - is only a tiny cleanup/free helper:
    - `ExFreePoolWithTag`

Interpretation:

- the `0x00056DBC` line no longer looks like a productive hot transfer path
- its immediate ecosystem has drifted into:
  - ETW registration
  - ETW unregister
  - path/string handling
  - registry/debug
  - cleanup/free helpers
- that means the clean next offline move is to pivot back to the unresolved secondary branch instead of pushing deeper into this band

So the next clean offline target is now:

- `0x0001BAC0`

with immediate follow-on context in:

- `0x0001BC34`
- `0x0001BF58`
- `0x0001F9A4`
- `0x00005BC0`
- `0x00006A08`

That next read-only stage now exists too:

- `scripts\capture-usbxhci-secondary-branch-assessment.ps1`

Current `0x0001BAC0` secondary-branch assessment on this machine:

- compared targets:
  - `0x0001BC34`
  - `0x0001BF58`
  - `0x0001F9A4`
  - `0x00005BC0`
  - `0x00006A08`
- recommended next:
  - `0x0001BC34`

Why:

- `0x0001BC34`
  - is the only substantive IRP/completion-style body in this set:
    - size `798`
    - `7` direct internal
    - `2` direct IAT
  - direct imports:
    - `IoReuseIrp`
    - `IoSetCompletionRoutineEx`
  - internal fan-out includes:
    - `0x00001BE8`
    - `0x00058EC0`
    - `0x00058B00`
- `0x0001BF58`
  - demotes to trace-heavy side context:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x0001F9A4`
  - also demotes to trace-heavy side context:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00005BC0`
  - is only a tiny reconnecting bridge:
    - `0x00006A44`
- `0x00006A08`
  - is another small reconnecting bridge:
    - `0x00041EC0`

Interpretation:

- the pivot away from the `0x00056DBC` band was correct
- the cleanest remaining body on the secondary branch is `0x0001BC34`
- the other four candidates should be treated as:
  - trace-heavy context
  - or small reconnectors into older helper machinery

So the next clean offline target is now:

- `0x0001BC34`

with immediate follow-on context in:

- `0x00001BE8`
- `0x00058EC0`
- `0x00058B00`

That next read-only stage now exists too:

- `scripts\capture-usbxhci-1bc34-follow-assessment.ps1`

Current `0x0001BC34` follow-on assessment on this machine:

- compared follow-ons:
  - `0x00001BE8`
  - `0x00058EC0`
  - `0x00058B00`
- recommended next:
  - `0x0001C090`

Why:

- `0x00001BE8`
  - is another instrumented leg:
    - `0x00058B00`
    - `WppAutoLogTrace`
- `0x00058EC0`
  - is an opaque slab:
    - size `263`
    - `0` direct internal
    - `0` direct IAT
- `0x00058B00`
  - is only a tiny thunk:
    - size `6`
    - `0` direct internal
    - `0` direct IAT

Interpretation:

- the direct `0x0001BC34` callee set does not contain a better next deep target
- one leg is instrumented
- one leg is opaque
- one leg is just a thunk
- so the next sensible offline move is to step sideways to the next substantial same-band body instead of following those direct callees further

So the next clean offline target is now:

- `0x0001C090`

The spike now also includes a bounded exhaustive walk over the current read-only traversal model:

- `scripts\capture-usbxhci-exhaustive-walk.ps1`

Current exhaustive result on this machine:

- seed set:
  - `0x0001BC34`
- output:
  - `out\dev\usbxhci-exhaustive-walk.txt`
- visited targets:
  - `927`
- class breakdown:
  - `substantive=335`
  - `mixed=294`
  - `trace=141`
  - `bridge=61`
  - `stub=44`
  - `opaque=22`
  - `path-string=16`
  - `thunk=10`
  - `etw=4`
- traversal mode:
  - direct internal targets from substantive and bridge nodes
  - plus substantial same-band neighbors

Coverage check:

- earlier hot-path anchors are included in the exhaustive walk:
  - `0x00006BA0`
  - `0x00006E74`
  - `0x000077FC`
  - `0x00007D60`
  - `0x0001AD7C`
  - `0x0001B1F0`
  - `0x00056DBC`

Interpretation:

- the current read-only traversal frontier is exhausted under the present rules
- there is no longer a single manually selected "next target" inside this reachable subgraph
- further progress now means either:
  - changing the traversal rules
  - seeding a different subgraph
  - or leaving read-only recon and moving into a different kind of experiment

The spike now also includes feature-candidate closure:

- `scripts\capture-usbxhci-feature-closure.ps1`

Current feature-candidate closure result on this machine:

- feature candidates:
  - `70`
- baseline visited:
  - `927`
- missing feature seeds before closure:
  - `15`
- closure output:
  - `out\dev\usbxhci-feature-closure.txt`
  - `out\dev\usbxhci-feature-closure-walk.txt`
- combined visited after closure:
  - `963`
- still missing feature candidates:
  - `0`

Interpretation:

- all current feature-map and cluster-profile candidates are now covered
- the remaining uncovered space after that point was general runtime-function space, not unresolved feature candidates

The spike now also includes whole-runtime closure:

- `scripts\capture-usbxhci-runtime-closure.ps1`

Current whole-runtime closure result on this machine:

- runtime functions:
  - `1294`
- baseline walks:
  - `2`
- baseline visited:
  - `963`
- missing runtime seeds before closure:
  - `331`
- closure output:
  - `out\dev\usbxhci-runtime-closure.txt`
  - `out\dev\usbxhci-runtime-closure-walk.txt`
- combined visited after closure:
  - `1294`
- still missing runtime functions:
  - `0`

Interpretation:

- the read-only runtime census is now complete for `USBXHCI.SYS` `10.0.26100.2454`
- every runtime function in the image has now been entered at least once under the current deep-pass model
- there is no remaining untested target set inside this image for the present read-only workflow

The spike now also includes an intervention shortlist derived from the full runtime census:

- `scripts\capture-usbxhci-intervention-shortlist.ps1`

Current shortlist result on this machine:

- parsed targets:
  - `1294`
- output:
  - `out\dev\usbxhci-intervention-shortlist.txt`

Current top candidates:

1. `0x0001B1F0`
   - controller timing/orchestration body
   - imports:
     - `KeQueryUnbiasedInterruptTime`
     - `KeStallExecutionProcessor`
     - `ExAllocateTimer`
     - `ExSetTimer`
     - `ExDeleteTimer`
     - `KeWaitForSingleObject`
2. `0x0001144D`
   - transfer hot-path body
   - imports:
     - `KeAcquireSpinLockRaiseToDpc`
     - `KeReleaseSpinLock`
3. `0x000038CC`
   - transfer hot-path body
   - imports:
     - `KeAcquireSpinLockRaiseToDpc`
     - `KeReleaseSpinLock`
4. `0x00011E20`
   - transfer hot-path body
   - imports:
     - `KeAcquireSpinLockRaiseToDpc`
     - `KeReleaseSpinLock`
5. `0x0003634C`
   - controller timing/orchestration body
   - imports:
     - `KeQueryUnbiasedInterruptTime`
     - `KeDelayExecutionThread`
     - `KeGetCurrentIrql`

Interpretation:

- the branch now has a ranked intervention set instead of just coverage
- the best candidates split cleanly into:
  - controller timing/orchestration bodies
  - transfer hot-path spinlock bodies
- if this spike moves beyond read-only recon, these are the first bodies worth deeper offline study or controlled patchability analysis

The spike now also includes a controller-family assessment:

- `scripts\capture-usbxhci-controller-timing-family-assessment.ps1`

Current controller-family result on this machine:

- output:
  - `out\dev\usbxhci-controller-timing-family-assessment.txt`
- primary bodies:
  - `0x0001B1F0`
  - `0x0003634C`
- shared direct callees:
  - `0x0000D210`
  - `0x00019AC8`
  - `0x0001A724`
  - `0x0001BA28`
  - `0x0002E390`

Interpretation:

- the two controller timing bodies do share a callee spine
- but that shared spine mostly demotes to:
  - wrappers and bridges
  - trace legs
  - debug-side context
- it does not expose a better deeper timing body than the parents themselves
- the controller-family intervention order should stay:
  - `0x0001B1F0`
  - `0x0003634C`

The spike now also includes a `0x0001B1F0` exclusive-branch assessment:

- `scripts\capture-usbxhci-1b1f0-exclusive-assessment.ps1`

Current `0x0001B1F0` exclusive-branch result on this machine:

- output:
  - `out\dev\usbxhci-1b1f0-exclusive-assessment.txt`
- exclusive direct callees reviewed:
  - `0x0000BE64`
  - `0x0000BF40`
  - `0x0000C970`
  - `0x0001BA64`
  - `0x0001BA8C`
  - `0x0003FC38`
  - `0x00058B00`

Interpretation:

- the unique `0x0001B1F0` branch mostly collapses to:
  - wrapper ladders
  - debug/IRQL side legs
  - a thunk
- `0x0003FC38` is the only exclusive descendant that still looks like real timing-side machinery
- the next controller-family target should therefore be:
  - `0x0003FC38`

The spike now also includes a `0x0003FC38` branch assessment:

- `scripts\capture-usbxhci-3fc38-branch-assessment.ps1`

Current `0x0003FC38` branch result on this machine:

- output:
  - `out\dev\usbxhci-3fc38-branch-assessment.txt`
- reviewed branch nodes:
  - `0x00012BF0`
  - `0x0001BA28`
  - `0x0000BF40`
  - `0x0000C970`
  - `0x0000D210`
  - `0x0003C400`
  - `0x0003FE84`
  - `0x0003FF40`
  - `0x00058B00`

Interpretation:

- `0x0003FC38` remains the leaf timing body in the primary controller family
- the descendants demote into:
  - side bodies
  - flush and completion service legs
  - wrappers
  - debug/IRQL legs
  - a thunk

The spike now also includes a `0x0003634C` exclusive assessment:

- `scripts\capture-usbxhci-3634c-exclusive-assessment.ps1`

Current `0x0003634C` exclusive-branch result on this machine:

- output:
  - `out\dev\usbxhci-3634c-exclusive-assessment.txt`
- reviewed exclusive nodes:
  - `0x0001A7FC`
  - `0x0001BA00`
  - `0x0002F834`
  - `0x000303B4`
  - `0x00041388`
  - `0x00056D58`

Interpretation:

- the secondary controller family does not reveal a better deeper timing body either
- `0x0003634C` should be treated as the leaf timing body for that side

Current controller-family leaf set is now:

- primary:
  - `0x0001B1F0`
  - leaf timing descendant `0x0003FC38`
- secondary:
  - `0x0003634C`

The spike now also includes a transfer-family assessment:

- `scripts\capture-usbxhci-transfer-family-assessment.ps1`

Current transfer-family result on this machine:

- helper-heavy pair:
  - `0x0001144D`
  - `0x00011E20`
- event-dispatch pair:
  - `0x000038CC`
  - `0x000077FC`

Interpretation:

- helper-heavy shared spine:
  - `0x00010440`
  - `0x000049B4`
  - `0x00058B00`
- only `0x00010440` remains substantive on that side
- event-dispatch shared spine:
  - `0x00003C70`
  - `0x00003FA0`
  - `0x00004124`
  - `0x000049B4`
  - `0x00005BC0`
- that side does not expose a better shared deeper body
- keep `0x000038CC` and `0x000077FC` as the current event-side leaves

The spike now also includes a `0x00010440` helper-branch assessment:

- `scripts\capture-usbxhci-10440-helper-branch-assessment.ps1`

Current `0x00010440` branch result on this machine:

- output:
  - `out\dev\usbxhci-10440-helper-branch-assessment.txt`
- reviewed branch nodes:
  - `0x00010D60`
  - `0x00058B00`
  - `0x00058EC0`
  - `0x00011240`
  - `0x00022E7C`
  - `0x000076A0`
  - `0x00010CD8`
  - `0x000111C4`
  - `0x00012700`
  - `0x000148B4`
  - `0x0002F21C`
  - `0x0003C8C4`

Interpretation:

- `0x00010D60` is the only substantive transfer-side descendant under `0x00010440`
- the other direct descendants demote into:
  - trace legs
  - control/assert context
  - stubs
  - opaque slabs
  - side bridges
- the helper-heavy transfer subfamily leaf set is now:
  - `0x0001144D`
  - `0x00011E20`
  - leaf body `0x00010D60`
