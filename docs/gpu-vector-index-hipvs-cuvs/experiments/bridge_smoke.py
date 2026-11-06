"""Small real CAGRA bridge lifecycle check; this is not a contest benchmark."""
import ctypes as C
import argparse
import json
import os
from pathlib import Path
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--bridge", type=Path, required=True)
parser.add_argument("--snapshot", type=Path, required=True)
parser.add_argument("--dtype", choices=("half", "float"), default="half")
parser.add_argument("--gemm", choices=("0", "1"), default="1")
args = parser.parse_args()
assert not args.snapshot.exists(), "Refuse to overwrite previous snapshot evidence"
os.environ.update(
    SEEKDB_CUVS_BUILD_ALGO="bfknn_sym",
    SEEKDB_CUVS_BF_GEMM=args.gemm,
    SEEKDB_CUVS_DTYPE=args.dtype,
    SEEKDB_CUVS_GEMM_TILE_Q="64",
    SEEKDB_CUVS_GEMM_TILE_B="128",
    SEEKDB_CUVS_GEMM_WMA="1",
    SEEKDB_CUVS_TIME="1",
    SEEKDB_CUVS_PIN_NODE="off",
)
os.environ.pop("SEEKDB_CUVS_GEMM_FUSED_NORM", None)  # Validate the production default.
lib = C.CDLL(str(args.bridge.resolve()))
hip = C.CDLL("/opt/rocm/lib/libamdhip64.so")
fptr, uptr = C.POINTER(C.c_float), C.POINTER(C.c_uint)
lib.seekdb_cuvs_build.argtypes = [fptr, C.c_long, C.c_long]
lib.seekdb_cuvs_build.restype = C.c_void_p
lib.seekdb_cuvs_search.argtypes = [C.c_void_p, fptr, C.c_long, C.c_long, uptr, fptr]
lib.seekdb_cuvs_search.restype = C.c_int
lib.seekdb_cuvs_free.argtypes = [C.c_void_p]
lib.seekdb_cuvs_free.restype = None
lib.seekdb_cuvs_serialize.argtypes = [C.c_void_p, C.c_char_p]
lib.seekdb_cuvs_serialize.restype = C.c_int
lib.seekdb_cuvs_deserialize.argtypes = [C.c_char_p, C.c_long, C.c_long]
lib.seekdb_cuvs_deserialize.restype = C.c_void_p

n, dim, nq, k = 512, 128, 16, 10
rows = [[float((i * 7 + c * 11) % 19 - 9) for c in range(dim)] for i in range(n)]
for i in range(n):
    rows[i][0] = float(i)
query_ids = [i * 29 + 7 for i in range(nq)]
base = (C.c_float * (n * dim))(*(x for row in rows for x in row))
queries = (C.c_float * (nq * dim))(*(x for i in query_ids for x in rows[i]))
expected = []
for qi in query_ids:
    expected.append(sorted((sum((x-y)**2 for x,y in zip(rows[qi], row)), b)
                           for b,row in enumerate(rows)))


def search(handle, stage):
    ids, distances = (C.c_uint * (nq * k))(), (C.c_float * (nq * k))()
    rc = lib.seekdb_cuvs_search(handle, queries, nq, k, ids, distances)
    assert rc == 0, (stage, "search failed", rc)
    assert hip.hipDeviceSynchronize() == 0, (stage, "HIP execution error")
    hits, self_hits = 0, 0
    for q in range(nq):
        result = list(ids[q*k:(q+1)*k])
        assert len(set(result)) == k and all(i < n for i in result), (stage, q, result)
        self_hits += result[0] == query_ids[q]
        # Accept any ID at the kth-distance tie.
        exact = dict((idx, d) for d, idx in expected[q])
        hits += sum(exact[idx] <= expected[q][k-1][0] for idx in result)
        for j,idx in enumerate(result):
            assert abs(distances[q*k+j] - exact[idx]) < 0.125, (stage, q, idx, distances[q*k+j], exact[idx])
    assert self_hits == nq, (stage, "self matches", self_hits, nq)
    print(json.dumps(dict(stage=stage, queries=nq, topk=k,
                          self_matches=self_hits, tie_aware_recall=hits/(nq*k))), flush=True)
    return [tuple(sorted(dict((idx, d) for d, idx in expected[q])[ids[q*k+j]]
                         for j in range(k))) for q in range(nq)]


handle = lib.seekdb_cuvs_build(base, n, dim)
assert handle, "build returned null"
try:
    original = search(handle, "fresh")
    assert search(handle, "reused") == original, "repeated search changed distance ranks"
    index_path = args.snapshot.resolve()
    assert lib.seekdb_cuvs_serialize(handle, os.fsencode(index_path)) == 0, "serialize failed"
finally:
    lib.seekdb_cuvs_free(handle)

handle = lib.seekdb_cuvs_deserialize(os.fsencode(index_path), n, dim)
assert handle, "deserialize returned null"
try:
    assert search(handle, "deserialized") == original, "round-trip changed distance ranks"
finally:
    lib.seekdb_cuvs_free(handle)
assert hip.hipDeviceSynchronize() == 0

for wrong_n, wrong_dim in ((n + 1, dim), (n, dim + 1)):
    wrong = lib.seekdb_cuvs_deserialize(os.fsencode(index_path), wrong_n, wrong_dim)
    if wrong:
        lib.seekdb_cuvs_free(wrong)
        raise AssertionError("snapshot shape mismatch was accepted")
print("PASS: mismatched snapshot shapes rejected", flush=True)
print("PASS: real bridge build/search/reuse/serialize/deserialize/free", flush=True)
