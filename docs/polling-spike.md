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
