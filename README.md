# pggaf

> **Development status:** `pggaf` is still under active development and should not be used yet for production or relied on for stable results.

`pggaf` annotates GAF (Graph Alignment Format) files with two complementary pieces of information derived from a pangenome graph index:

- **Haplotype set tags** (`hs`/`hb`/`he`) — which donor haplotype paths in the graph each alignment is consistent with, stored as compact set IDs that index into a sidecar file mapping them back to named samples.
- **Reference coordinate tags** (`rc`/`rb`/`re`) — the chromosome name and 0-based interval on the linear reference genome, projected from the graph walk by following reference-sample haplotype paths embedded in the GBZ index.

Both outputs are derived from a GBZ file, which bundles the haplotype index together with the graph topology needed to compute node lengths and reference path positions.

The typical workflow is:
1. Align reads to a pangenome graph (e.g. with `vg giraffe`) → produces a GAF file.
2. Run `pggaf annotate-gaf` to produce an annotated GAF and a `.pgs` sidecar. With `--gbz` and `--ref-sample`, each record also gains `rc`/`rb`/`re` reference coordinates derived from the named sample's haplotype paths in the graph.
3. Optionally run `pggaf index-gaf` to coordinate-sort and tabix-index the output for region queries (requires `rc`/`rb`/`re`).
4. Optionally run `pggaf decode` to export the sidecar as a human-readable TSV.

---

## Contents

- [End-to-end example](#end-to-end-example)
- [Dependencies](#dependencies)
- [Building](#building)
- [Installing](#installing)
- [Usage](#usage)
  - [`pggaf annotate-gaf`](#pggaf-annotate-gaf)
  - [`pggaf index-gaf`](#pggaf-index-gaf)
  - [`pggaf decode`](#pggaf-decode)
- [Using pggaf as a library](#using-pggaf-as-a-library)

---

## End-to-end example

**Scenario:** HiFi reads from sample HG002 aligned to the HPRC pangenome graph with `vg giraffe`. The goal is to annotate each read with the donor haplotypes it is most consistent with, and export a TSV mapping those annotations to named samples.

**Inputs:**

```
HG002.gaf                        # read-to-graph alignments from vg giraffe
hprc-v2.1-mc-chm13-eval.gbz     # GBZ pangenome graph+index
hprc-v2.1-mc-chm13-eval.ri      # r-index (optional, speeds up annotation)
```

> **GAM input:** `pggaf` accepts GAF only. If your aligner produced a GAM file (vg's binary protobuf format), convert it first:
> ```bash
> vg convert -G alignments.gam graph.gbz > alignments.gaf
> ```

**Step 1 — Annotate the GAF:**

```bash
pggaf annotate-gaf \
    --gaf        HG002.gaf \
    --gbz        hprc-v2.1-mc-chm13-eval.gbz \
    --r-index    hprc-v2.1-mc-chm13-eval.ri \
    --ref-sample CHM13 \
    --out-gaf    HG002.annotated.gaf \
    --out-sets   HG002.pgs
```

Each record in `HG002.annotated.gaf` now carries up to six extra tags:

```
hs:B:I,4,7       <- set IDs: two subpath matches
hb:B:I,0,391     <- start node offset for each match
he:B:I,391,520   <- end node offset (exclusive) for each match
rc:Z:chr9        <- reference contig
rb:i:68000000    <- 0-based start on rc
re:i:68050000    <- 0-based end (exclusive) on rc
```

`hs` indexes into `HG002.pgs`. A read with a single `hs` entry maps entirely within one group of haplotype paths; multiple entries mean the read spans graph regions where different haplotypes diverge.

`HG002.pgs` stores, for each set ID, the list of haplotype thread IDs that pass through that subpath. The sidecar keeps the GAF lean; decoding is done on demand with `pggaf decode`.

**Step 2 — Index for region queries** (optional, requires `rc`/`rb`/`re` tags from step 1):

```bash
pggaf index-gaf \
    --in  HG002.annotated.gaf \
    --out HG002.annotated.coord.gaf.gz
```

Query by region:

```bash
tabix HG002.annotated.coord.gaf.gz chr9:68000000-69000000
```

**Step 3 — Decode to TSV** (optional):

```bash
pggaf decode \
    --sets HG002.pgs \
    --gbz  hprc-v2.1-mc-chm13-eval.gbz \
    --out  HG002.threads.tsv
```

`HG002.threads.tsv` maps every set ID to named haplotypes:

```
set_id  thread_id  path_id  sample   haplotype  locus         path_name
4       54312      54312    GRCh38   0          chr9          GRCh38#0#chr9[68220832]
4       54372      54372    HG00140  1          CM087114.1    HG00140#1#CM087114.1#59589124
7       61204      61204    HG00513  2          CM088003.1    HG00513#2#CM088003.1#71304981
```

Each row is one haplotype thread that passes through the graph subpath a read was aligned to. Joining on `hs` from the annotated GAF to `set_id` in this TSV gives you the full haplotype identity for every read.

---

## Dependencies

### Required

| Dependency | Version | Install |
|---|---|---|
| CMake | ≥ 3.20 | `brew install cmake` / `apt install cmake` |
| C++20 compiler | GCC ≥ 11 or Clang ≥ 13 | system toolchain |
| GBZ index | — | produced by `vg gbwt` or downloaded from HPRC |
| OpenSSL | any recent | `brew install openssl` / `apt install libssl-dev` |
| Zstandard | any recent | `brew install zstd` / `apt install libzstd-dev` |
| zlib | any recent | system default on macOS; `apt install zlib1g-dev` on Linux |
| autoconf + automake | any recent | `brew install autoconf automake` / `apt install autoconf automake` |

### Optional

| Dependency | Purpose | Install |
|---|---|---|
| patchelf | **Linux only** — self-contained install (see [Installing](#installing)) | auto-built if not found on `PATH` |

### Bundled (via git submodules)

- [sdsl-lite](https://github.com/vgteam/sdsl-lite) — compressed data structures
- [gbwt](https://github.com/jltsiren/gbwt) — graph BWT and r-index
- [gbwtgraph](https://github.com/jltsiren/gbwtgraph) — graph query layer
- [libhandlegraph](https://github.com/vgteam/libhandlegraph) — pangenome graph interface
- [htslib](https://github.com/samtools/htslib) — bgzip compression and tabix indexing for `index-gaf` (built from source; no separate install needed)

---

## Building

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/kokyriakidis/pggaf.git
cd pggaf
```

Configure and build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The `pggaf` binary is at `build/pggaf`. If you only want to run the tool locally, this is all you need — installing is optional.

---

## Installing

Installing is only necessary if you want to put `pggaf` on your `PATH`, distribute it to a machine without the build dependencies, or use it as a library in another CMake project. The install step bundles all runtime dependencies (zstd, OpenSSL) alongside the binary so the result is self-contained.

```bash
cmake --install build --prefix /path/to/install
```

This installs:

```
/path/to/install/
  bin/pggaf
  lib/libpggaf.dylib   (macOS) / libpggaf.so  (Linux)
  lib/libzstd.*        ← bundled at install time
  include/pggaf/
  lib/cmake/pggaf/
```

Runtime libraries are copied into `lib/` and their load paths are rewritten — no Homebrew, conda, or package manager prefix needs to be present on the target machine.

**Linux note:** `patchelf` is required for the bundling step. If it is not on `PATH`, it is automatically built from source during `cmake --build` and used transparently — no manual install needed.

---

## Usage

### `pggaf annotate-gaf`

Annotate a GAF file with haplotype set information.

```
pggaf annotate-gaf
    --gaf      <in.gaf>
    --gbz      <graph.gbz>
    --out-gaf  <out.gaf>
    --out-sets <out.pgs>
  [ --r-index    <graph.ri> ]
  [ --ref-sample <name> ]
  [ --primary-only ]
```

| Flag | Required | Description |
|---|---|---|
| `--gaf` | yes | Input GAF file |
| `--gbz` | yes | GBZ pangenome graph index |
| `--out-gaf` | yes | Output annotated GAF file |
| `--out-sets` | yes | Output sidecar metadata file (`.pgs`) |
| `--r-index` | no | R-index (`.ri`) — enables faster haplotype locate queries |
| `--ref-sample` | no | Reference sample name for coordinate projection (e.g. `CHM13`); if omitted, the sample(s) listed in the GBZ `RS:Z:` tag are used |
| `--primary-only` | no | Use the highest-MAPQ alignment's walk for haplotype annotation of each `qname` block; reference coordinate tags are still written for every record |

`annotate-gaf` groups **contiguous** GAF records with the same `qname` into a block. No sorting is required. When `--primary-only` is used, the alignment with the highest MAPQ in each contiguous block determines the haplotype annotation (`hs`/`hb`/`he`) for all records in that block; reference coordinate tags (`rc`/`rb`/`re`) are always written per record based on that record's own graph walk.

> **Note:** if supplementary alignments for the same read are not contiguous in the file (i.e. other reads appear between them), each contiguous run is treated as a separate block and annotated independently. Most aligners (`vg giraffe`) output all alignments for a read together, so this is rarely an issue. If needed, sort by qname first: `LC_ALL=C sort -k1,1V reads.gaf > reads.sorted.gaf`

**Memory model:**

- The GAF input is streamed; it is not loaded into RAM in full.
- The GBZ index is loaded into RAM before annotation starts.
- If an r-index is provided, it is also loaded into RAM.

**Reference coordinate tags** (`rc`/`rb`/`re`): `annotate-gaf` projects each record's graph walk onto linear reference coordinates:

1. Walk the nodes in GAF column 6 and find those overlapping the aligned region (columns 7–8).
2. Look up each node in the reference-sense haplotype paths (sample named by `--ref-sample`, or auto-detected from the GBZ `RS:Z:` tag).
3. Convert node-local offsets to absolute reference coordinates, accounting for strand.
4. If all anchoring nodes agree on the same chromosome and orientation, emit the union interval as `rc`/`rb`/`re`. Otherwise omit the tags for that record.

**Tags written to each record:**

`hs`, `hb`, and `he` are parallel arrays — entry `i` across all three describes one subpath match:

| Tag | Type | Content |
|---|---|---|
| `hs` | `B:I` | Set ID for match `i` — index into the sidecar to retrieve matching thread IDs |
| `hb` | `B:I` | Begin node offset for match `i` within the GAF walk |
| `he` | `B:I` | End-exclusive node offset for match `i` within the GAF walk |
| `rc` | `Z` | Reference contig name (e.g. `chr20`); only with `--gbz` |
| `rb` | `i` | 0-based begin coordinate on `rc`; only with `--gbz` |
| `re` | `i` | 0-based end-exclusive coordinate on `rc`; only with `--gbz` |

Tags are omitted on records with no valid graph mapping. The original 12 GAF columns are preserved unchanged.

**Example:**

```bash
pggaf annotate-gaf \
    --gaf        reads.gaf \
    --gbz        hprc.gbz \
    --r-index    hprc.ri \
    --ref-sample CHM13 \
    --out-gaf    reads.annotated.gaf \
    --out-sets   reads.pgs
```

---

### `pggaf index-gaf`

Coordinate-sort an annotated GAF and build a tabix index for region queries.

```
pggaf index-gaf
    --in  <annotated.gaf>
    --out <out.gaf.gz>
```

| Flag | Required | Description |
|---|---|---|
| `--in` | yes | Annotated GAF from `annotate-gaf` (must contain `rc`/`rb`/`re` tags) |
| `--out` | yes | Output bgzip-compressed, coordinate-sorted GAF (use `.gaf.gz` suffix) |

Records without `rc`/`rb`/`re` are omitted from the output. Coordinate sorting uses the system `sort` (available on all Unix/macOS systems); bgzip compression and tabix indexing are handled internally via the bundled htslib — no separate bgzip or tabix install needed. `tabix` from your system is still needed to query the resulting index.

**Example:**

```bash
pggaf index-gaf \
    --in  reads.annotated.gaf \
    --out reads.annotated.coord.gaf.gz
```

**Query by region:**

```bash
tabix reads.annotated.coord.gaf.gz chr20:60452687-60469389
```

Each result line has three prepended coordinate columns followed by the original annotated GAF record:

```
chr20   60452687   60469389   <qname>   ...   rc:Z:chr20   rb:i:60452686   re:i:60469389   hs:B:I,...
```

---

### `pggaf decode`

Decode a sidecar file into a TSV of thread identities.

```
pggaf decode
    --sets <in.pgs>
    --gbz  <graph.gbz>
    --out  <out.tsv>
```

| Flag | Required | Description |
|---|---|---|
| `--sets` | yes | Sidecar file produced by `annotate-gaf` |
| `--gbz` | yes | GBZ used at annotation time |
| `--out` | yes | Output TSV file |

The GBZ fingerprint is checked against the sidecar — passing a different index than the one used at annotation time is an error.

**Example:**

```bash
pggaf decode \
    --sets reads.pgs \
    --gbz  hprc-v2.1-mc-chm13-eval.gbz \
    --out  threads.tsv
```

**Output columns:**

```
set_id  thread_id  path_id  sample  haplotype  locus  path_name
```

---

## Using pggaf as a library

If you installed with `cmake --install`, downstream CMake projects can find pggaf via:

```cmake
find_package(pggaf REQUIRED)
target_link_libraries(my_target PRIVATE pggaf::pggaf)
```

Pass `-Dpggaf_DIR=/path/to/install/lib/cmake/pggaf` if the install prefix is not in CMake's default search path.
