# Shader cache

Every shader Community Shaders builds, with FXC or DXC, goes through one store: `org::services::ShaderStore`
(`extern/ORGModuleServices`). It lives in `Data/ShaderCache/v3`. The rule it follows is simple: a saved shader is used
only when every file it read last time still has the same contents.

## How one shader is resolved

1. **Look up the variant** by its key: the root name, entry point, target, defines in order, final compiler
   arguments and compiler identity.
2. **Check its files.** The record lists each file the compiler opened, and each path it tried that did not exist.
   If they all still match and the root is unchanged, read the artifact. This is a hit, and it does not preprocess.
3. **Otherwise preprocess** and hash the text. If an artifact exists for that text, use it and update the record. This
   is "absorbed". It covers comment edits, inactive code, reverted edits, and variants that compile to the same thing.
4. **Otherwise compile** and save.

Debug builds skip step 3's sharing. They embed source text, so any source change must rebuild them.

File times are never used. Each file is read and hashed once per generation, and a stat costs about as much as
the read. A hot reload or a shader clear starts a new generation.

## Reading the log

Each batch logs one line:

```
Shader store after shader compilation: 4719 requests: 4549 hits, 170 absorbed, 0 compiled, 0 failed (0 remembered),
0 not saved. 170 preprocessed, 190 source files read. Misses: 0 no record, 0 root changed, 170 dependency changed,
0 artifact invalid. First changed file: shaders/watereffects/waterparallax.hlsli
```

| Field | Meaning |
|---|---|
| `no record` | The variant is new: a new define set, flag or compiler, or the index was lost. |
| `root changed` | A generated root's bytes differ, such as a DCLF rewrite. |
| `dependency changed` | A file it read changed, or a path it tried now exists. The first such file is named. |
| `artifact invalid` | The artifact is missing, corrupt or evicted. |
| `not saved` | The compile worked, but its file could not be written. |
| `remembered` | The same inputs failed before, so nothing ran. Fixing any file it read clears this. |

A start with no changes should show all hits and `0 preprocessed`. If you see `compiled` on every start, this line
says why.

## On disk

- `index.a`, `index.b`: the variant records, and where each artifact is. Saved in turn, each with a hash, so a torn
  write falls back to the other. Losing both costs preprocessing, never compiling: a start then reads every pack.
- `packs/<8 hex digits>.pack`: the artifacts, named by what they contain, as records appended to a few large files.
  A pack starts with a 16-byte header (magic, schema, its id). Each record is a 48-byte header (magic, format, codec,
  the compile key, the stored and raw sizes, and a hash over the header and the stored bytes), then the stored bytes.
- `engine-draw-programs.txt`: DCLF's manifest. The store leaves it alone.

Formats in the record header: DXIL 0, SPIR-V 1, DXBC 2. `tools/aftermath-decode --spirv` reads this folder through the
store, read-only. `tools/shader-compare/dxc_survey.py` reads the packs itself.

### Appending

- Only the process that owns the folder writes. Its compile workers compress outside any lock, then append under one
  lock, at the pack's end, with a positional write. The artifact enters the table only after the write, so a reader
  never finds a record still being written. Readers read records with positional reads, many at once.
- A pack takes records until it would pass 64 MB (`ShaderStoreOptions::packBytes`), then a new pack starts. A record
  larger than that gets a pack of its own.
- Nothing is renamed or truncated, because renames are what fail under MO2's usvfs. A failed write leaves the pack's
  end where it was, and the next record writes over it.

### Starting

- The index holds each pack's end as of its save. A start reads the index, then only what each pack has past that
  end: what a killed process appended after its last save. Each whole record found there is added; the first torn
  one ends the read, and the next append writes over it. A pack the index does not know is read whole.
- So a killed game loses only the record it was writing. The variant records it made since the last save are lost
  too, as before, and those variants are "absorbed" on the next start: preprocessed, not compiled.
- About 7,000 artifacts make an index of about 700 KB. The library test (`TestIndexLoadTimeAtGameSize`) reads it in
  about 4 ms and opens the packs in under 1 ms. The game logs its own numbers in the "Shader store opened" line.

### Compression

- zstd level 9, with no dictionary (`ShaderStoreOptions::compressionLevel`; 0 stores raw). A record that zstd does not
  shrink is stored raw. The level only affects new records, so changing it needs no rebuild.
- On the repository's SPIR-V and DXBC samples (162 files, 11 MB): level 3 gives 3.5x, level 9 3.8x, level 19 4.4x.
  Compression at level 9 runs at about 60 MB/s, under 2 ms for a typical shader, small beside its compile.
  Decompression runs at about 1 GB/s at any level: about 0.07 ms for a typical 70 KB shader, less than reading the
  bytes it saves. A trained dictionary gave about 10% more, not worth a dictionary to version and keep.

## Eviction and compaction

At startup, if the stored records are over the size limit (`Shader Store Max MB`, 4 GB by default): artifacts no
record uses go first, then the longest unused. Anything used in the last three starts stays even over the limit, with a
warning, because evicting live shaders would rebuild them on every start. Eviction only drops entries from the table.

Compaction then frees the bytes. A pack more than half dead (evicted, removed or replaced records, or a torn tail) has
its live records copied, still compressed, to the newest pack, and is deleted. Over the size limit, the packs with the
most dead bytes go too, until what is left fits. It runs at startup, before any request; it copies at most the live
half of each pack it rewrites.

A start killed during compaction is safe: the copies are past the index's end of the pack they went to, so the next
start reads them, and the old pack, now with no live records, is compacted again. A pack that could not be deleted is
kept in the index as retired and deleted on a later start.

## The v2 layout

v3 replaced v2's one file per artifact (`cas/xx/<key>.bin`). The old folder is not migrated: the first v3 start deletes
it with the rest of `Data/ShaderCache`'s old entries (logging "Deleted N files (M MB) of the old shader cache") and
compiles everything once.

## Disk cache off

Nothing is read, written or deleted. As before, the game also does not precompile at startup in this mode, so loads do
not wait and shaders compile as they are drawn.

## Tests

`extern/ORGModuleServices/tests/ShaderStore` covers the engine with a fake compiler that counts its calls: among them,
round trips through zstd, pack rollover, a process killed between saves and in the middle of a record, many threads
appending at once, compaction (and a start killed during it), eviction with compaction, and read-only opening.
`extern/ORGModuleServices/tests/Services` covers the same behaviour through `ShaderCompiler` with real DXC.
Both build standalone, with zstd on the prefix path: `cmake -S tests/ShaderStore -B build/store && cmake --build build/store`.

## Open questions

Decided by the agent, to confirm:

- **Level 9, no dictionary.** Level 19 is 15% smaller but compresses about 12 times slower, which a cold start would feel.
- **No migration from v2.** One full compile on the first start is simpler than reading the old files, and it is stated.
- **A new folder, v3.** A v2 build deletes every `Data/ShaderCache` entry but `v2`, and a v3 build every entry but
  `v3`. On one shared store (cs-test's), each switch between a v2 and a v3 build compiles everything, until every
  stage has v3. Putting the packs inside `v2` under new file names would avoid that, at the cost of a misleading name.
- **Another process does not write.** A second game on the same folder reads what the owner saved but saves no
  artifacts; its compiles count as "not saved". Two processes appending to one pack would need a cross-process lock.
- **Compaction runs at startup, in the store's constructor.** It is bounded by the live half of the packs it rewrites,
  usually nothing. A background compaction would need moving records while readers use them.
- **64 MB packs.** Small enough that compacting one is quick, large enough that the game has a handful of files.

