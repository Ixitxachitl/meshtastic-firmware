#!/usr/bin/env python3
"""Bake a worldwide basemap into the tile format read by NicheGraphics::MapTiles
(src/graphics/niche/Map/MapTileRenderer.cpp / MapTile.h).

Fetches raster tiles from a MapTiler style (default: toner-v2, which is already
black/white line art - a good match for a 1-bit display), resizes them to the
256x256 tile size the on-device format uses, thresholds to 1-bit, and LZ4-compresses
each tile (raw block format, matching the hand-rolled decoder in MapTileRenderer.cpp).

Usage:
    python bin/generate_map_tiles.py --api-key KEY --zooms 0-4 --out world_blob.bin

The output is a standalone binary blob (tile index + compressed payloads), NOT a
compiled-in header - this data is meant for external storage (SD card, or a plain
file on a filesystem with room to spare), not the ~780KB internal app flash budget.
A companion --emit-header flag can still produce a MapTile.h-compatible header for
small test datasets.

--cache checkpoints the baked (post-threshold) tiles so an interrupted run can resume without
re-fetching. It cannot be reused across different --threshold/--invert/--style values - that's
refused via a small .meta.json sidecar next to it. To try multiple thresholds without re-hitting
the MapTiler API (and burning credits) for the same tiles, pass --raw-cache as well: it
checkpoints the raw fetched images pre-threshold, and is reused regardless of --threshold/--invert.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import struct
import sys
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

import lz4.block
import requests
from PIL import Image

TILE_SIZE = (
    512  # Overridden from --tile-size; must match NicheGraphics::MapTiles::kTileSizePx.
)
KIND_LZ4 = 0
KIND_WHITE = 1
KIND_BLACK = 2


@dataclass
class BakedTile:
    zoom: int
    tx: int
    ty: int
    kind: int
    payload: bytes  # empty for WHITE/BLACK


def fetch_tile_bytes(
    session: requests.Session,
    style: str,
    api_key: str,
    z: int,
    x: int,
    y: int,
    retries: int = 3,
) -> bytes:
    """Downloads the raw tile image bytes (PNG, as served) - kept separate from decoding so the
    raw bytes can be persisted to --raw-cache before any thresholding happens."""
    url = f"https://api.maptiler.com/maps/{style}/{z}/{x}/{y}.png?key={urllib.parse.quote(api_key)}"
    last_exc = None
    for attempt in range(retries):
        try:
            resp = session.get(url, timeout=30)
            resp.raise_for_status()
            return resp.content
        except Exception as exc:  # noqa: BLE001 - want to retry on anything transient
            last_exc = exc
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError(f"Failed to fetch tile z={z} x={x} y={y}: {last_exc}")


def decode_tile_image(raw: bytes) -> Image.Image:
    img = Image.open(io.BytesIO(raw))
    img.load()
    return img


def pack_1bpp_column_major(bits: list[list[bool]]) -> bytes:
    """bits[y][x] -> column-major packed bytes matching tile[(x//8)*TILE_SIZE + y] & (1 << (x%8))."""
    byte_cols = TILE_SIZE // 8
    out = bytearray(byte_cols * TILE_SIZE)
    for y in range(TILE_SIZE):
        row = bits[y]
        for bx in range(byte_cols):
            byte = 0
            base = bx * 8
            for k in range(8):
                if row[base + k]:
                    byte |= 1 << k
            out[bx * TILE_SIZE + y] = byte
    return bytes(out)


def threshold_tile(img: Image.Image, threshold: int, invert: bool) -> bytes:
    img = img.convert("L")
    # Only resize if the fetched size doesn't already match - avoids a needless (lossy) resize
    # when --tile-size matches what MapTiler returned natively (e.g. 512 for toner-v2).
    if img.size != (TILE_SIZE, TILE_SIZE):
        img = img.resize((TILE_SIZE, TILE_SIZE), Image.LANCZOS)
    px = img.load()
    bits = [[False] * TILE_SIZE for _ in range(TILE_SIZE)]
    any_set = False
    all_set = True
    for y in range(TILE_SIZE):
        for x in range(TILE_SIZE):
            dark = px[x, y] < threshold
            if invert:
                dark = not dark
            bits[y][x] = dark
            any_set = any_set or dark
            all_set = all_set and dark
    if not any_set:
        return b""  # caller treats as KIND_WHITE
    if all_set:
        return b"\xff"  # sentinel: caller treats as KIND_BLACK
    return pack_1bpp_column_major(bits)


def bake_tile(
    session: requests.Session,
    raw_tiles: dict[tuple[int, int, int], bytes],
    style: str,
    api_key: str,
    z: int,
    x: int,
    y: int,
    threshold: int,
    invert: bool,
) -> tuple[BakedTile, bytes, int, bool]:
    """Returns (baked_tile, raw_image_bytes, download_bytes, fetched_now). raw_image_bytes/
    fetched_now let the caller persist newly-downloaded raw bytes to --raw-cache without writing
    back bytes that were already served from that cache."""
    cached_raw = raw_tiles.get((z, x, y))
    if cached_raw is not None:
        raw_bytes, download_bytes, fetched_now = cached_raw, 0, False
    else:
        raw_bytes = fetch_tile_bytes(session, style, api_key, z, x, y)
        download_bytes, fetched_now = len(raw_bytes), True
    img = decode_tile_image(raw_bytes)
    packed = threshold_tile(img, threshold, invert)
    if packed == b"":
        return (
            BakedTile(z, x, y, KIND_WHITE, b""),
            raw_bytes,
            download_bytes,
            fetched_now,
        )
    if packed == b"\xff":
        return (
            BakedTile(z, x, y, KIND_BLACK, b""),
            raw_bytes,
            download_bytes,
            fetched_now,
        )
    compressed = lz4.block.compress(packed, store_size=False)
    # Rare: if LZ4 didn't actually shrink a busy tile, storing raw would need a
    # separate "uncompressed" kind the C++ side doesn't have yet - not expected in
    # practice for sparse line-art, so just keep the compressed form either way.
    return (
        BakedTile(z, x, y, KIND_LZ4, compressed),
        raw_bytes,
        download_bytes,
        fetched_now,
    )


def render_progress(done: int, total: int, elapsed: float, download_bytes: int) -> None:
    """Overwrites the current terminal line with a live progress bar + download rate (via \\r) -
    meant to be watched in a terminal, not parsed from a log file (redirected output will just show
    each update concatenated, which is harmless but not pretty)."""
    pct = (done / total) if total else 1.0
    bar_width = 30
    filled = int(bar_width * pct)
    bar = "#" * filled + "-" * (bar_width - filled)
    tiles_per_sec = done / elapsed if elapsed > 0 else 0.0
    bytes_per_sec = download_bytes / elapsed if elapsed > 0 else 0.0
    if bytes_per_sec >= 1024 * 1024:
        rate_str = f"{bytes_per_sec / 1024 / 1024:5.2f} MB/s"
    else:
        rate_str = f"{bytes_per_sec / 1024:5.1f} KB/s"
    sys.stdout.write(
        f"\r[{bar}] {done}/{total} ({pct * 100:5.1f}%)  {tiles_per_sec:5.1f} tiles/s  {rate_str}   "
    )
    sys.stdout.flush()


def read_cache(cache_path: Path) -> dict[tuple[int, int, int], BakedTile]:
    """Reads back tiles already baked by a prior (possibly interrupted) run - see write_cache_entry.
    Tolerates a truncated trailing record (e.g. the process was killed mid-write) by stopping at
    the first record that doesn't fully parse, rather than failing the whole resume."""
    tiles: dict[tuple[int, int, int], BakedTile] = {}
    if not cache_path.exists():
        return tiles
    data = cache_path.read_bytes()
    pos = 0
    header_size = struct.calcsize("<BHHBH")
    while pos + header_size <= len(data):
        zoom, tx, ty, kind, size = struct.unpack_from("<BHHBH", data, pos)
        pos += header_size
        if pos + size > len(data):
            break  # Truncated trailing record - discard, worker() will refetch this one.
        payload = data[pos : pos + size]
        pos += size
        tiles[(zoom, tx, ty)] = BakedTile(zoom, tx, ty, kind, payload)
    return tiles


def write_cache_entry(cache_file, t: BakedTile) -> None:
    """Appends one baked tile to the on-disk cache immediately, so a crash (rate limit, network,
    Ctrl-C) only loses in-flight tiles, not the whole run - re-running with the same --cache path
    skips everything already recorded here and only fetches what's still missing."""
    cache_file.write(struct.pack("<BHHBH", t.zoom, t.tx, t.ty, t.kind, len(t.payload)))
    cache_file.write(t.payload)
    cache_file.flush()
    os.fsync(cache_file.fileno())


def cache_meta_path(cache_path: Path) -> Path:
    return cache_path.parent / (cache_path.name + ".meta.json")


def check_and_write_cache_meta(
    cache_path: Path, style: str, threshold: int, invert: bool, tile_size: int
) -> None:
    """Baked --cache entries are stored post-threshold with no per-record record of which
    threshold/invert/style/tile-size produced them. Re-running against the same --cache path
    with different values would silently mix settings within one blob. Guard against that with a
    small sidecar recording the settings the cache was started with; a legacy cache (no sidecar,
    from before this check existed) is trusted as-is."""
    current = {
        "style": style,
        "threshold": threshold,
        "invert": invert,
        "tile_size": tile_size,
    }
    meta_path = cache_meta_path(cache_path)
    if meta_path.exists():
        try:
            existing = json.loads(meta_path.read_text())
        except (OSError, json.JSONDecodeError):
            existing = None
        if existing is not None and existing != current:
            raise SystemExit(
                f"--cache '{cache_path}' was baked with {existing}, but this run asked for "
                f"{current}. Reusing it would silently mix tiles baked at two different settings "
                f"into one blob. Pick a new --cache path (or delete the old one and its "
                f"'{meta_path.name}' sidecar) for this configuration."
            )
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(current))


def read_raw_cache(cache_path: Path | None) -> dict[tuple[int, int, int], bytes]:
    """Reads back raw (pre-threshold) tile image bytes checkpointed by a prior run - see
    write_raw_cache_entry. Same truncated-trailing-record tolerance as read_cache."""
    tiles: dict[tuple[int, int, int], bytes] = {}
    if cache_path is None or not cache_path.exists():
        return tiles
    data = cache_path.read_bytes()
    pos = 0
    header_size = struct.calcsize("<BHHI")
    while pos + header_size <= len(data):
        zoom, tx, ty, size = struct.unpack_from("<BHHI", data, pos)
        pos += header_size
        if pos + size > len(data):
            break  # Truncated trailing record - discard, worker() will refetch this one.
        tiles[(zoom, tx, ty)] = data[pos : pos + size]
        pos += size
    return tiles


def write_raw_cache_entry(
    cache_file, z: int, tx: int, ty: int, raw_bytes: bytes
) -> None:
    """Appends one tile's raw fetched image bytes (pre-threshold) to --raw-cache, so a later run
    with a different --threshold/--invert can re-bake from disk instead of re-fetching.
    """
    cache_file.write(struct.pack("<BHHI", z, tx, ty, len(raw_bytes)))
    cache_file.write(raw_bytes)
    cache_file.flush()
    os.fsync(cache_file.fileno())


def write_blob(tiles: list[BakedTile], out_path: Path) -> None:
    """Binary layout (little-endian):
    u32 magic 'MTLB', u32 tile_count
    tile_count * { u8 zoom, u16 tx, u16 ty, u8 kind, u32 offset, u16 size }
    followed by concatenated payload bytes (offset is into this payload region).
    """
    payload = bytearray()
    index = bytearray()
    for t in tiles:
        offset = len(payload)
        payload += t.payload
        index += struct.pack(
            "<BHHBIH", t.zoom, t.tx, t.ty, t.kind, offset, len(t.payload)
        )

    with open(out_path, "wb") as f:
        f.write(b"MTLB")
        f.write(
            struct.pack("<I", len(tiles))
        )  # u32: z8 alone is 65536 tiles, over u16's range
        f.write(index)
        f.write(payload)


def write_c_header(tiles: list[BakedTile], out_path: Path) -> None:
    """Emit a MapTile.h-compatible header (sparse layout) - only practical for small
    test datasets, since this data compiles into the firmware image."""
    payload = bytearray()
    offsets = []
    for t in tiles:
        offsets.append(len(payload))
        payload += t.payload

    def carr(name: str, vals: list, ctype: str) -> str:
        body = ", ".join(str(v) for v in vals)
        return f"static const {ctype} {name}[] = {{{body}}};"

    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "",
        "static const uint8_t map_tile_layout = 0;",
        "static const uint8_t map_tile_grid_cols = 0;",
        "static const uint8_t map_tile_grid_rows = 0;",
        "static const uint8_t map_tile_block_count = 0;",
        f"static const uint16_t map_tile_count = {len(tiles)};",
        carr("map_tile_zooms", [t.zoom for t in tiles], "uint8_t"),
        carr("map_tile_tx", [t.tx for t in tiles], "uint16_t"),
        carr("map_tile_ty", [t.ty for t in tiles], "uint16_t"),
        "static const uint8_t map_tile_block_zooms[] = {};",
        "static const uint16_t map_tile_block_tx[] = {};",
        "static const uint16_t map_tile_block_ty[] = {};",
        carr("map_tile_kinds", [t.kind for t in tiles], "uint8_t"),
        carr("map_tile_sizes", [len(t.payload) for t in tiles], "uint16_t"),
        carr("map_tile_offsets", offsets, "uint32_t"),
        carr("map_tile_data", list(payload), "uint8_t"),
    ]
    out_path.write_text("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--api-key", required=True, help="MapTiler API key")
    ap.add_argument(
        "--style", default="toner-v2", help="MapTiler style id (default: toner-v2)"
    )
    ap.add_argument(
        "--zooms", default="0-2", help="Zoom range, e.g. '0-4' or a single level '3'"
    )
    ap.add_argument(
        "--threshold",
        type=int,
        default=128,
        help="Grayscale threshold (0-255) for 1-bit conversion",
    )
    ap.add_argument(
        "--invert",
        action="store_true",
        help="Invert black/white (this style renders dark ocean by default)",
    )
    ap.add_argument(
        "--out", type=Path, default=Path("world_blob.bin"), help="Output blob path"
    )
    ap.add_argument(
        "--emit-header",
        type=Path,
        default=None,
        help="Also emit a MapTile.h-compatible header (small datasets only)",
    )
    ap.add_argument(
        "--max-tiles",
        type=int,
        default=None,
        help="Safety cap on total tiles fetched (for test runs)",
    )
    ap.add_argument(
        "--workers", type=int, default=16, help="Concurrent fetch workers (default 16)"
    )
    ap.add_argument(
        "--cache",
        type=Path,
        default=None,
        help="Checkpoint file, appended to as each tile is baked (default: <out> with .cache "
        "suffix). Re-running with the same --cache path resumes: tiles already recorded there are "
        "skipped instead of re-fetched, so a crash (rate limit, network, Ctrl-C) only costs the "
        "in-flight tiles, not the whole run. Records are post-threshold, so switching --threshold/"
        "--invert/--style against an existing --cache path is refused - see --raw-cache to change "
        "those without re-fetching.",
    )
    ap.add_argument(
        "--raw-cache",
        type=Path,
        default=None,
        help="Optional checkpoint of the *raw* fetched tile images, before thresholding/"
        "compression (default: none). Re-running with the same --raw-cache path, even with a "
        "different --threshold or --invert, re-bakes from these cached bytes instead of hitting "
        "the MapTiler API again. Off by default: raw tile images are much larger than the baked "
        "1-bit output, so a full world bake's raw cache can run into the GB range.",
    )
    ap.add_argument(
        "--max-consecutive-failures",
        type=int,
        default=10,
        help="Stop (without losing cached progress) after this many tile fetches fail outright in "
        "a row - a persistent failure almost always means a dead/quota-exhausted API key, not bad "
        "luck on individual tiles, so there's no point burning through the rest of the queue "
        "retrying each one only to fail the same way (default 10)",
    )
    ap.add_argument(
        "--tile-size",
        type=int,
        default=512,
        choices=(256, 512),
        help="On-device tile edge length in px - MUST match NicheGraphics::MapTiles::kTileSizePx "
        "(src/graphics/niche/Map/MapTileRenderer.h). Default 512 (MapTiler's native fetch "
        "resolution, avoiding a lossy downsample) - every current Map-capable target uses this; "
        "256 remains available only for a from-scratch flash/RAM-constrained target that doesn't "
        "exist yet",
    )
    args = ap.parse_args()

    global TILE_SIZE
    TILE_SIZE = args.tile_size

    if "-" in args.zooms:
        z_lo, z_hi = (int(v) for v in args.zooms.split("-", 1))
    else:
        z_lo = z_hi = int(args.zooms)

    coords = [
        (z, tx, ty)
        for z in range(z_lo, z_hi + 1)
        for ty in range(1 << z)
        for tx in range(1 << z)
    ]
    if args.max_tiles is not None:
        coords = coords[: args.max_tiles]
    total_planned = len(coords)

    cache_path = (
        args.cache
        if args.cache is not None
        else args.out.with_suffix(args.out.suffix + ".cache")
    )
    check_and_write_cache_meta(
        cache_path, args.style, args.threshold, args.invert, args.tile_size
    )
    tiles_by_coord = read_cache(cache_path)
    resumed = len(tiles_by_coord)
    todo = [c for c in coords if c not in tiles_by_coord]
    print(
        f"Planning {total_planned} tiles across zoom {z_lo}-{z_hi} (style={args.style}, workers={args.workers}) - "
        f"{resumed} already cached in '{cache_path}', {len(todo)} left to fetch"
    )

    raw_cache_path = args.raw_cache
    raw_tiles = read_raw_cache(raw_cache_path)
    if raw_cache_path is not None:
        print(
            f"Raw-tile cache '{raw_cache_path}': {len(raw_tiles)} tile(s) available - these won't "
            "be re-fetched even for tiles missing from --cache"
        )

    kind_counts = {KIND_LZ4: 0, KIND_WHITE: 0, KIND_BLACK: 0}
    for t in tiles_by_coord.values():
        kind_counts[t.kind] += 1
    raw_total = sum(
        TILE_SIZE * TILE_SIZE // 8
        for t in tiles_by_coord.values()
        if t.kind == KIND_LZ4
    )
    compressed_total = sum(
        len(t.payload) for t in tiles_by_coord.values() if t.kind == KIND_LZ4
    )
    fetched = 0
    download_bytes = 0
    consecutive_failures = 0
    aborted = False
    lock = threading.Lock()
    start_time = time.time()
    last_render = 0.0

    def worker(coord):
        z, tx, ty = coord
        thread_local = getattr(_thread_state, "session", None)
        if thread_local is None:
            thread_local = requests.Session()
            _thread_state.session = thread_local
        try:
            t, raw_bytes, n_bytes, fetched_now = bake_tile(
                thread_local,
                raw_tiles,
                args.style,
                args.api_key,
                z,
                tx,
                ty,
                args.threshold,
                args.invert,
            )
            return coord, t, raw_bytes, n_bytes, fetched_now, None
        except Exception as exc:  # noqa: BLE001 - report it, don't crash the whole pool
            return coord, None, None, 0, False, exc

    _thread_state = threading.local()
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    if raw_cache_path is not None:
        raw_cache_path.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.ExitStack() as stack:
        cache_file = stack.enter_context(open(cache_path, "ab"))
        raw_cache_file = (
            stack.enter_context(open(raw_cache_path, "ab"))
            if raw_cache_path is not None
            else None
        )
        pool = stack.enter_context(ThreadPoolExecutor(max_workers=args.workers))
        futures = {pool.submit(worker, c): c for c in todo}
        for future in as_completed(futures):
            if aborted:
                future.cancel()
                continue
            coord, t, raw_bytes, n_bytes, fetched_now, exc = future.result()
            if exc is not None:
                consecutive_failures += 1
                sys.stdout.write("\n")
                print(
                    f"  FAILED {coord}: {exc} ({consecutive_failures}/{args.max_consecutive_failures} consecutive)"
                )
                if consecutive_failures >= args.max_consecutive_failures:
                    print(
                        f"Aborting: {args.max_consecutive_failures} fetches failed in a row - almost certainly a "
                        f"dead/quota-exhausted API key, not bad luck. Cancelling remaining queued tiles."
                    )
                    aborted = True
                    for f in futures:
                        f.cancel()
                continue
            consecutive_failures = 0
            tiles_by_coord[coord] = t
            write_cache_entry(cache_file, t)
            if raw_cache_file is not None and fetched_now:
                write_raw_cache_entry(
                    raw_cache_file, coord[0], coord[1], coord[2], raw_bytes
                )
            with lock:
                kind_counts[t.kind] += 1
                if t.kind == KIND_LZ4:
                    raw_total += TILE_SIZE * TILE_SIZE // 8
                    compressed_total += len(t.payload)
                fetched += 1
                download_bytes += n_bytes
                now = time.time()
                if now - last_render > 0.2 or fetched == len(todo):
                    render_progress(
                        resumed + fetched,
                        total_planned,
                        now - start_time,
                        download_bytes,
                    )
                    last_render = now

    sys.stdout.write("\n")
    if aborted:
        print(
            f"Stopped early: {resumed + fetched}/{total_planned} tiles cached in '{cache_path}'. Fix the API key/quota "
            f"and re-run the exact same command (same --cache) to resume - no work already cached will be re-fetched."
        )
        return 1

    # Preserve deterministic (z, ty, tx) ordering in the output regardless of completion order.
    tiles = [tiles_by_coord[c] for c in coords]

    write_blob(tiles, args.out)
    if args.emit_header:
        write_c_header(tiles, args.emit_header)
    print_report(tiles, kind_counts, raw_total, compressed_total, args.out)
    return 0


def print_report(
    tiles, kind_counts, raw_total, compressed_total, out_path: Path
) -> None:
    blob_size = out_path.stat().st_size if out_path.exists() else 0
    print()
    print("=== Bake report ===")
    print(f"Total tiles:      {len(tiles)}")
    print(f"  LZ4-compressed: {kind_counts[KIND_LZ4]}")
    print(f"  WHITE (free):   {kind_counts[KIND_WHITE]}")
    print(f"  BLACK (free):   {kind_counts[KIND_BLACK]}")
    if raw_total:
        print(
            f"LZ4 tiles: {raw_total} bytes raw -> {compressed_total} bytes compressed "
            f"({100.0 * compressed_total / raw_total:.1f}%)"
        )
    print(
        f"Output blob: {out_path} ({blob_size} bytes, {blob_size / 1024:.1f} KiB, {blob_size / 1024 / 1024:.2f} MiB)"
    )

    print()
    print("Per-zoom breakdown:")
    by_zoom: dict[int, list] = {}
    for t in tiles:
        by_zoom.setdefault(t.zoom, []).append(t)
    for z in sorted(by_zoom):
        zt = by_zoom[z]
        free = sum(1 for t in zt if t.kind != KIND_LZ4)
        payload_bytes = sum(len(t.payload) for t in zt) + 13 * len(
            zt
        )  # + per-tile index overhead
        print(
            f"  z{z}: {len(zt)} tiles, {free} free ({100.0*free/len(zt):.0f}%), "
            f"{payload_bytes} bytes ({payload_bytes/1024:.1f} KiB)"
        )


if __name__ == "__main__":
    sys.exit(main())
