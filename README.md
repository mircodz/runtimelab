# GC string deduplication

An experiment on top of [`feature/gc/baseline`](https://github.com/dotnet/runtimelab/tree/feature/gc/baseline):
the GC finds strings with the same contents and, opt-in, merges them, so
duplicates are reclaimed without any change to user code.

- **Queue:** strings that leave gen1 become candidates; a string is examined once.
- **Find:** after mark, the candidates are hashed and compared to find duplicates
  without touching the heap. A time budget and a table size limit bound the work.
- **Merge:** a compacting GC (or a background GC) points every reference to a
  duplicate at one canonical string; the next gen2 GC reclaims the duplicates.
  A gen1 GC that would sweep compacts instead when enough duplicates were found.
- **Measure:** a full blocking GC can report how much deduplication would save,
  without changing anything.

Strings whose identity is observable (hash code, lock or sync block in the
header) and pinned strings are left alone. With server GC, finding and
merging need regions; merging always does.

Everything is off by default. The settings (`DOTNET_` + name, values in hex):

| Setting | Default | What it does |
|---|---|---|
| `GCStringDedupMeasure` | 0 | Measure the potential in full blocking GCs (workstation GC) |
| `GCStringDedupQueue` | 0 | Queue candidates |
| `GCStringDedupFind` | 0 | Find duplicates among them; implies Queue |
| `GCStringDedupMerge` | 0 | Merge what was found; implies Find (regions only) |
| `GCStringDedupQueueMax` | 0x100000 | Most candidates a heap queues per GC |
| `GCStringDedupBudgetUs` | 0 | Most microseconds the finder spends per GC; 0 is no limit |
| `GCStringDedupFindTableBits` | 0x15 | Finder table holds 3/4 of 2^N strings (N 12 to 24) |
| `GCStringDedupCompactMinKB` | 0x40 | Duplicate KB that make a sweeping gen1 GC compact; 0 never |

Each stage reports a GC event (`StringDedupMeasure`, `StringDedupQueue`,
`StringDedupFind`, `StringDedupMerge`); the payloads are described in
`src/coreclr/gc/string_dedup.cpp`.

## Where the code is

All of the feature is in `src/coreclr/gc/string_dedup.cpp`. The rest of the GC
calls into it from a few places: `collect.cpp` (after mark and after plan),
`plan_phase.cpp` (the decision to compact), `relocate_compact.cpp` (the
relocation of references), `background.cpp` and `interface.cpp` (background GC
marking and the roots that a stack holds), `dynamic_heap_count.cpp` (DATAS) and
`diagnostics.cpp` (heap verification). The members are declared in `gcpriv.h`.

## Tests

`src/tests/GC/Features/StringDedup` runs the merge with workstation and with
server GC: 100,000 strings with 1,000 different contents have to become at most
a tenth of that many objects (1,000 in the runs so far) with their contents
intact, and strings that have a hash code have to stay separate objects.

## Known limits

- The merge needs regions, so 64-bit and not the segments GC.
- With server GC, a GC that overlaps a background GC merges nothing.
- A merged string is one object for every reference to it: code that relies on
  the identity of two equal strings, or that changes a string in place, can
  observe it. That is why the feature is opt-in.
- Built and tested on macOS arm64 only; Windows, Linux, x86 and NativeAOT are
  not tested.

The NuGet prerelease label is **`gcdedup`**. Standard runtime package IDs are
unchanged.

The rest of this file is the baseline's.

# GC baseline

A runtime-side baseline for running runtimelab GC experiments with
[dotnet/performance](https://github.com/dotnet/performance).
GC implementations are developed in runtimelab; dotnet/performance supplies
the workloads and runner that exercise those runtimes.

`feature/gc/baseline` starts from `dotnet/runtime` commit
`f92f93994a587f69d6f7510d5390e68bd5290b2b`. It includes the full runtime source
and runtimelab publishing setup, with no experimental GC changes.

## Build and publish

[`eng/pipelines/runtimelab-official.yml`](eng/pipelines/runtimelab-official.yml)
builds unsigned Release packages and uses the existing Arcade publishing stages.
MicroBuild signing is disabled for this experimental baseline; normal publishing
permissions and production-access checks still apply.
CI is manual-only; `publishToExperimentalFeed` defaults to `false`.

**Do not queue CI or publish to `dotnet-experimental` without explicit approval
after the changes have been reviewed.** Build-only approval is not publishing
approval.

### Standard-GC producer validation

Definition 895 keeps the standard-GC producer disabled by default. The `x64`
mode builds the Linux and Windows x64 runtime packages and archives and
publishes `ExternalRuntime306Validation` for exactly the six definition 306
microbenchmark rows on those RIDs across the `main`, `9.0`, and `8.0`
channels. Each row proof is bound to the producer source, build, campaign,
cohort manifest, package identity, and measured runtime binary hashes.

The `full` mode retains the complete 15-row definition 306 receipt. Neither
producer mode queues definition 306, changes its workload settings or queues,
or enables performance-result ingestion.

See [Create an experiment](https://github.com/dotnet/runtimelab/blob/docs/CreateAnExperiment.md)
for the standard runtimelab conventions.

## GC correctness and stress validation

Definition 163 exposes three `gcValidationMode` values:

- `baseline` preserves the reviewed baseline graph.
- `correctness-shard` imports one of the five closed `gcValidationRoot` graphs.
- `correctness-stress` queues and monitors all five shard roots as child runs of
  definition 163.

The parent passes its exact source ref and 40-character source version to every
child without a YAML override. `gcValidationCampaignId` can identify an
explicit campaign; when empty, the parent derives `gc-validation-<build ID>`.
`gcValidationParentBuildId` is likewise optional for the parent and is always
populated in child template parameters. `gcValidationAttempt` is restricted to
1 or 2 and is used only for the single permitted infrastructure retry.
The same campaign, parent, root, mode, and attempt identity is persisted in
non-secret run variables because the Azure Runs List/Get responses do not
return submitted template parameters. The controller hydrates List entries
with individual Get responses before matching the exact source ref and commit.
Those caller-supplied variables are discovery hints, not trusted child
evidence.
When concurrent parents create duplicate children, the lowest matching run ID
is canonical, even after it reaches a terminal state. Every shard first runs
`GCValidationAdmission`, observes exact matching children for 60 seconds, and
permits only the active canonical run to enter the unchanged authoritative root
stage. A duplicate fails before any standard build or Helix job and publishes
`GCValidationAdmission_<child build ID>/gc-validation-admission.json`.
The parent accepts a terminal child only after its native Azure timeline shows
the `GCValidationAdmission` stage and job succeeded and the authoritative
`Build` stage is present, terminal, non-skipped, and succeeded. It also lists
and downloads the exact admission artifact, then validates the receipt's
campaign, parent, root, attempt, source ref and commit, current and canonical
run IDs, admitted status, and canonical SHA-256. A baseline run, skipped Build,
missing artifact, or mismatched receipt is recorded as a rejected candidate and
does not prevent the parent from queueing or adopting a genuine shard. The
overall pipeline result alone is never accepted as proof of shard success.
Incomplete terminal timeline or artifact visibility is retried for a bounded
two-minute consistency window; evidence that remains incomplete is rejected so
it cannot indefinitely suppress a genuine replacement shard.

The parent uses only `System.AccessToken` and checks its effective
`QueueBuilds` permission on definition 163 before submitting a child. It
publishes `GCValidationChildren_<parent build ID>`, containing each canonical
queue request, its SHA-256 hash, the adopted or returned child build ID,
terminal results and failure classifications, plus
`gc-validation-children.json`. The parent succeeds only when that receipt
contains all five terminal child receipts and every final child result
succeeded. The receipt also distinguishes submitted queue requests from
confirmed new runs, records rejected candidates and their native evidence, and
records transient monitoring API errors.
An attempt 2 run is accepted only when the canonical attempt 1 is freshly
confirmed by the same native evidence as terminal and unsuccessful and its
timeline contains only proven transient agent-loss evidence. A successful POST
whose response cannot be read or decoded is treated as ambiguous and adopted
by identity without repeating the POST. A returned ID is trusted only when it
is a positive JSON integer; all other response shapes use the same
adoption-only path.
