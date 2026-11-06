int main(int argc, char** argv)
{
  const bool shape = argc > 1 && std::strcmp(argv[1], "shape") == 0;
  const long n = shape ? std::atol(argv[2]) : 128;
  const long gd = shape ? std::atol(argv[3]) : 96;
  const long tq = shape ? std::atol(argv[4]) : 32;
  const long tb = shape ? std::atol(argv[5]) : 128;
  const long dim = 16;
  std::vector<__half> base(n * dim);
  // Distinct row norms, including a large change between consecutive tiles.
  for (long i = 0; i < n; ++i) {
    for (long c = 0; c < dim; ++c) {
      base[i * dim + c] = __half((i * 7 + c * 11) % 67 - 33);
    }
    base[i * dim] = __half(i);
  }
  if (!shape) {
    if (std::strcmp(argv[1], "allocation") == 0) { fail_allocation = std::atoi(argv[2]); }
    if (std::strcmp(argv[1], "copy") == 0) { fail_copy = true; }
    if (std::strcmp(argv[1], "selection") == 0) { fail_selection = true; }
  }
  setenv("SEEKDB_CUVS_TIME", "1", 1);
  raft::resources res;
  std::vector<float> distances(n * gd);
  std::vector<unsigned> ids(n * gd);
  const int rc = sk_bf_gemm_topk128(&res, base.data(), n, dim, gd,
                                   distances.data(), ids.data(), tq, tb);
  if (!allocations.empty() || live_handles != 0 || live_events != 0) {
    std::fprintf(stderr, "leaked allocations=%zu handles=%d events=%d\n",
                 allocations.size(), live_handles, live_events);
    return 1;
  }
  if (!shape) {
    if (rc != 1) { std::fprintf(stderr, "failure was not propagated: %d\n", rc); return 1; }
    return 0;
  }
  if (rc != 0) { std::fprintf(stderr, "valid tiled input rejected: %d\n", rc); return 1; }
  for (long q = 0; q < n; ++q) {
    std::vector<float> oracle;
    for (long b = 0; b < n; ++b) {
      float d = 0;
      for (long c = 0; c < dim; ++c) {
        const float delta = float(base[q * dim + c]) - float(base[b * dim + c]);
        d += delta * delta;
      }
      oracle.push_back(d);
    }
    auto sorted = oracle;
    std::sort(sorted.begin(), sorted.end());
    std::unordered_set<unsigned> seen;
    for (long k = 0; k < gd; ++k) {
      const unsigned id = ids[q * gd + k];
      if (id >= n || !seen.insert(id).second || oracle[id] != sorted[k] ||
          distances[q * gd + k] != sorted[k]) {
        std::fprintf(stderr, "wrong neighbor/distance at query=%ld rank=%ld id=%u\n", q, k, id);
        return 1;
      }
    }
  }
  return 0;
}
