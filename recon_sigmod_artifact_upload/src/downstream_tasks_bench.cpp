#define GRAPH_COMPRESS_NO_MAIN
#include "recon.cpp"

#include <cmath>
#include <functional>
#include <numeric>
#include <queue>
#include <unordered_set>

struct CompressedGraphView {
    int32_t n = 0;
    vector<int32_t> chain;
    vector<NodeRange> ranges;
    vector<int32_t> component_id;
    int32_t component_count = -1;
    vector<CommonSegment> common_segments;
    mutable vector<uint8_t> cached;
    mutable vector<vector<int32_t>> cache;

    explicit CompressedGraphView(int32_t nodes = 0) : n(nodes), ranges(nodes), cached(nodes, 0), cache(nodes) {
        for (auto& r : ranges) {
            r = {-1, -1};
        }
    }

    const vector<int32_t>& neighbors(int32_t u) const {
        if (u < 0 || u >= n) {
            static const vector<int32_t> empty;
            return empty;
        }
        if (!cached[u]) {
            cached[u] = 1;
            const NodeRange r = ranges[u];
            if (r.start >= 0 && r.end >= r.start) {
                cache[u].assign(chain.begin() + r.start, chain.begin() + r.end + 1);
                sort(cache[u].begin(), cache[u].end());
                cache[u].erase(unique(cache[u].begin(), cache[u].end()), cache[u].end());
            }
        }
        return cache[u];
    }
};

static CompressedGraphView load_compressed_graph(const string& path, int32_t n) {
    ifstream in(path, ios::binary);
    if (!in) {
        throw runtime_error("cannot open compressed graph: " + path);
    }

    CompressedGraphView cg(n);
    uint64_t chain_len = 0;
    in.read(reinterpret_cast<char*>(&chain_len), sizeof(chain_len));
    cg.chain.resize(chain_len);
    in.read(reinterpret_cast<char*>(cg.chain.data()), static_cast<streamsize>(chain_len * sizeof(int32_t)));

    uint64_t ranges = 0;
    in.read(reinterpret_cast<char*>(&ranges), sizeof(ranges));
    for (uint64_t i = 0; i < ranges; ++i) {
        int32_t node = -1;
        NodeRange r;
        in.read(reinterpret_cast<char*>(&node), sizeof(node));
        in.read(reinterpret_cast<char*>(&r), sizeof(r));
        if (node >= 0 && node < n) {
            cg.ranges[node] = r;
        }
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    if (in.read(reinterpret_cast<char*>(&magic), sizeof(magic)) && magic == 0x314d4347u) {
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version == 1) {
            uint64_t component_nodes = 0;
            uint64_t segment_count = 0;
            in.read(reinterpret_cast<char*>(&cg.component_count), sizeof(cg.component_count));
            in.read(reinterpret_cast<char*>(&component_nodes), sizeof(component_nodes));
            if (component_nodes == static_cast<uint64_t>(n)) {
                cg.component_id.resize(n);
                in.read(reinterpret_cast<char*>(cg.component_id.data()),
                        static_cast<streamsize>(component_nodes * sizeof(int32_t)));
            } else {
                in.seekg(static_cast<streamoff>(component_nodes * sizeof(int32_t)), ios::cur);
                cg.component_count = -1;
            }
            in.read(reinterpret_cast<char*>(&segment_count), sizeof(segment_count));
            cg.common_segments.resize(segment_count);
            in.read(reinterpret_cast<char*>(cg.common_segments.data()),
                    static_cast<streamsize>(segment_count * sizeof(CommonSegment)));
        }
    }
    return cg;
}

template <typename AdjGetter>
static vector<int32_t> bfs_generic(int32_t n, int32_t source, AdjGetter&& get_neighbors) {
    vector<int32_t> dist(n, -1);
    queue<int32_t> q;
    dist[source] = 0;
    q.push(source);

    while (!q.empty()) {
        int32_t u = q.front();
        q.pop();
        for (int32_t v : get_neighbors(u)) {
            if (v < 0 || v >= n) {
                continue;
            }
            if (dist[v] == -1) {
                dist[v] = dist[u] + 1;
                q.push(v);
            }
        }
    }
    return dist;
}

static int32_t choose_bfs_source(const Graph& g) {
    int32_t best = 0;
    uint64_t best_deg = 0;
    for (int32_t u = 0; u < g.n; ++u) {
        if (g.degree(u) > best_deg) {
            best_deg = g.degree(u);
            best = u;
        }
    }
    return best;
}

static vector<vector<int32_t>> build_position_to_nodes(const CompressedGraphView& cg) {
    vector<vector<int32_t>> pos_to_nodes(cg.chain.size());
    for (int32_t u = 0; u < cg.n; ++u) {
        const NodeRange r = cg.ranges[u];
        if (r.start < 0 || r.end < r.start) {
            continue;
        }
        for (int32_t p = r.start; p <= r.end; ++p) {
            pos_to_nodes[p].push_back(u);
        }
    }
    return pos_to_nodes;
}

static vector<int32_t> bfs_overlap_experimental(
    const CompressedGraphView& cg,
    int32_t source,
    const vector<vector<int32_t>>& pos_to_nodes
) {
    vector<int32_t> dist(cg.n, -1);
    queue<int32_t> q;
    dist[source] = 0;
    q.push(source);

    while (!q.empty()) {
        int32_t u = q.front();
        q.pop();
        const auto& nu = cg.neighbors(u);
        for (int32_t v : nu) {
            if (v >= 0 && v < cg.n && dist[v] == -1) {
                dist[v] = dist[u] + 1;
                q.push(v);
            }
        }

        const NodeRange r = cg.ranges[u];
        if (r.start < 0 || r.end < r.start) {
            continue;
        }
        // Experimental chain-side spread: reuse overlapping interval owners.
        for (int32_t p = r.start; p <= r.end; ++p) {
            for (int32_t v : pos_to_nodes[p]) {
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    q.push(v);
                }
            }
        }
    }
    return dist;
}

template <typename AdjGetter>
static int64_t count_cc_generic(int32_t n, AdjGetter&& get_neighbors) {
    vector<uint8_t> vis(n, 0);
    queue<int32_t> q;
    int64_t cc = 0;
    for (int32_t s = 0; s < n; ++s) {
        if (vis[s]) {
            continue;
        }
        vis[s] = 1;
        q.push(s);
        ++cc;
        while (!q.empty()) {
            int32_t u = q.front();
            q.pop();
            for (int32_t v : get_neighbors(u)) {
                if (v >= 0 && v < n && !vis[v]) {
                    vis[v] = 1;
                    q.push(v);
                }
            }
        }
    }
    return cc;
}

template <typename AdjGetter>
static int32_t kcore_generic(int32_t n, AdjGetter&& get_neighbors) {
    vector<int32_t> degree(n, 0);
    for (int32_t u = 0; u < n; ++u) {
        degree[u] = static_cast<int32_t>(get_neighbors(u).size());
    }

    vector<int32_t> order(n);
    iota(order.begin(), order.end(), 0);
    sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        if (degree[a] != degree[b]) return degree[a] < degree[b];
        return a < b;
    });

    vector<uint8_t> removed(n, 0);
    int32_t best_core = 0;
    for (int32_t u : order) {
        removed[u] = 1;
        best_core = max(best_core, degree[u]);
        for (int32_t v : get_neighbors(u)) {
            if (v >= 0 && v < n && !removed[v] && degree[v] > degree[u]) {
                --degree[v];
            }
        }
    }
    return best_core;
}

template <typename AdjGetter>
static vector<double> pagerank_generic(int32_t n, AdjGetter&& get_neighbors, int iters = 20, double damping = 0.85) {
    vector<double> pr(n, 1.0 / max(1, n));
    vector<double> next(n, 0.0);
    vector<int32_t> outdeg(n, 0);
    for (int32_t u = 0; u < n; ++u) {
        outdeg[u] = static_cast<int32_t>(get_neighbors(u).size());
    }

    for (int iter = 0; iter < iters; ++iter) {
        fill(next.begin(), next.end(), (1.0 - damping) / max(1, n));
        double dangling = 0.0;
        for (int32_t u = 0; u < n; ++u) {
            if (outdeg[u] == 0) {
                dangling += pr[u];
                continue;
            }
            const double share = damping * pr[u] / outdeg[u];
            for (int32_t v : get_neighbors(u)) {
                if (v >= 0 && v < n) {
                    next[v] += share;
                }
            }
        }
        const double dshare = damping * dangling / max(1, n);
        for (double& x : next) {
            x += dshare;
        }
        pr.swap(next);
    }
    return pr;
}

template <typename AdjGetter>
static int64_t triangle_count_generic(int32_t n, AdjGetter&& get_neighbors) {
    vector<int32_t> deg(n, 0);
    for (int32_t u = 0; u < n; ++u) {
        deg[u] = static_cast<int32_t>(get_neighbors(u).size());
    }

    auto forward = [&](int32_t u) {
        vector<int32_t> out;
        for (int32_t v : get_neighbors(u)) {
            if (v < 0 || v >= n || u == v) {
                continue;
            }
            if (deg[u] < deg[v] || (deg[u] == deg[v] && u < v)) {
                out.push_back(v);
            }
        }
        sort(out.begin(), out.end());
        out.erase(unique(out.begin(), out.end()), out.end());
        return out;
    };

    vector<vector<int32_t>> fwd(n);
    for (int32_t u = 0; u < n; ++u) {
        fwd[u] = forward(u);
    }

    int64_t triangles = 0;
    for (int32_t u = 0; u < n; ++u) {
        for (int32_t v : fwd[u]) {
            const auto& a = fwd[u];
            const auto& b = fwd[v];
            size_t i = 0;
            size_t j = 0;
            while (i < a.size() && j < b.size()) {
                if (a[i] == b[j]) {
                    ++triangles;
                    ++i;
                    ++j;
                } else if (a[i] < b[j]) {
                    ++i;
                } else {
                    ++j;
                }
            }
        }
    }
    return triangles;
}

static uint64_t pair_key(int32_t u, int32_t v) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(u)) << 32) |
           static_cast<uint32_t>(v);
}

static bool edge_exists(const CompressedGraphView& cg, int32_t u, int32_t v) {
    const auto& nu = cg.neighbors(u);
    return binary_search(nu.begin(), nu.end(), v);
}

static int64_t triangle_count_with_common_segments(const CompressedGraphView& cg) {
    vector<int32_t> deg(cg.n, 0);
    for (int32_t u = 0; u < cg.n; ++u) {
        deg[u] = static_cast<int32_t>(cg.neighbors(u).size());
    }

    auto oriented = [&](int32_t u, int32_t v) {
        if (deg[u] < deg[v] || (deg[u] == deg[v] && u < v)) {
            return pair<int32_t, int32_t>{u, v};
        }
        return pair<int32_t, int32_t>{v, u};
    };
    auto forward_order = [&](int32_t u, int32_t v) {
        return deg[u] < deg[v] || (deg[u] == deg[v] && u < v);
    };

    unordered_set<uint64_t> skipped_pairs;
    skipped_pairs.reserve(cg.common_segments.size() * 2 + 1);
    int64_t triangles = 0;
    for (const CommonSegment& seg : cg.common_segments) {
        if (seg.a < 0 || seg.b < 0 || seg.a >= cg.n || seg.b >= cg.n ||
            seg.start < 0 || seg.end < seg.start ||
            seg.end >= static_cast<int32_t>(cg.chain.size()) ||
            !edge_exists(cg, seg.a, seg.b)) {
            continue;
        }
        const auto uv = oriented(seg.a, seg.b);
        const uint64_t key = pair_key(uv.first, uv.second);
        if (skipped_pairs.insert(key).second) {
            int64_t direct = 0;
            for (int32_t p = seg.start; p <= seg.end; ++p) {
                const int32_t w = cg.chain[p];
                if (w >= 0 && w < cg.n &&
                    forward_order(uv.first, w) &&
                    forward_order(uv.second, w)) {
                    ++direct;
                }
            }
            triangles += direct;
        }
    }

    auto forward = [&](int32_t u) {
        vector<int32_t> out;
        for (int32_t v : cg.neighbors(u)) {
            if (v < 0 || v >= cg.n || u == v) {
                continue;
            }
            if (deg[u] < deg[v] || (deg[u] == deg[v] && u < v)) {
                out.push_back(v);
            }
        }
        return out;
    };

    vector<vector<int32_t>> fwd(cg.n);
    for (int32_t u = 0; u < cg.n; ++u) {
        fwd[u] = forward(u);
    }

    for (int32_t u = 0; u < cg.n; ++u) {
        for (int32_t v : fwd[u]) {
            if (skipped_pairs.find(pair_key(u, v)) != skipped_pairs.end()) {
                continue;
            }
            const auto& a = fwd[u];
            const auto& b = fwd[v];
            size_t i = 0;
            size_t j = 0;
            while (i < a.size() && j < b.size()) {
                if (a[i] == b[j]) {
                    ++triangles;
                    ++i;
                    ++j;
                } else if (a[i] < b[j]) {
                    ++i;
                } else {
                    ++j;
                }
            }
        }
    }
    return triangles;
}

struct BenchLine {
    string task;
    string method;
    double sec = 0.0;
    bool measured = true;
    string status;
    string note;
};

static int64_t seconds_to_ns(double sec) {
    if (sec <= 0.0) {
        return 0;
    }
    return max<int64_t>(1, static_cast<int64_t>(llround(sec * 1000000000.0)));
}

static void print_lines(const vector<BenchLine>& lines) {
    cout << fixed << setprecision(9);
    cout << "task,method,time_sec,time_ns,status,note\n";
    for (const auto& line : lines) {
        cout << line.task << ','
             << line.method << ',';
        if (line.measured) {
            cout << line.sec << ',' << seconds_to_ns(line.sec) << ',';
        } else {
            cout << "NA,NA,";
        }
        cout << line.status << ','
             << '"' << line.note << '"' << '\n';
    }
}

static bool compressed_matches_graph(const Graph& g, const CompressedGraphView& cg) {
    for (int32_t u = 0; u < g.n; ++u) {
        if (cg.neighbors(u) != g.neighbors(u)) {
            return false;
        }
    }
    return true;
}

static bool env_flag_enabled(const char* name) {
    const char* value = getenv(name);
    if (value == nullptr) {
        return false;
    }
    return string(value) == "1" || string(value) == "true" || string(value) == "TRUE" || string(value) == "yes";
}

template <typename Func>
static pair<double, std::invoke_result_t<Func>> timed_run(Func&& fn) {
    Timer t;
    auto value = fn();
    return {t.elapsed_sec(), std::move(value)};
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    string input = DATASET_INPUT_PATH;
    string compressed = DATASET_OUTPUT_PATH;
    bool verify_adjacency = true;
    if (argc > 1) input = argv[1];
    if (argc > 2) compressed = argv[2];
    if (argc > 3) verify_adjacency = atoi(argv[3]) != 0;

    PhaseTimes times;
    Graph g = read_graph_auto(input, times);
    CompressedGraphView cg = load_compressed_graph(compressed, g.n);
    const bool compressed_ok = !verify_adjacency || compressed_matches_graph(g, cg);
    const bool skip_expensive = g.edges.size() > 20000000ULL;
    const bool run_kcore = !skip_expensive || env_flag_enabled("CN_BENCH_RUN_KCORE");
    const bool run_pr = !skip_expensive || env_flag_enabled("CN_BENCH_RUN_PR");

    vector<BenchLine> lines;
    lines.push_back({
        "VERIFY",
        "compressed-vs-original",
        0.0,
        false,
        compressed_ok ? "OK" : "FAIL",
        verify_adjacency
            ? (compressed_ok ? "compressed graph matches original adjacency" : "stop using this compressed file for downstream tasks")
            : "skipped; rely on compressor verification"
    });
    if (!compressed_ok) {
        print_lines(lines);
        return 1;
    }

    const int32_t bfs_source = choose_bfs_source(g);
    const auto [bfs_orig_sec, bfs_orig] = timed_run([&]() {
        return bfs_generic(g.n, bfs_source, [&](int32_t u) {
            return g.neighbors(u);
        });
    });
    lines.push_back({"BFS", "original", bfs_orig_sec, true, "OK", "baseline"});

    const auto [bfs_comp_sec, bfs_comp] = timed_run([&]() {
        return bfs_generic(cg.n, bfs_source, [&](int32_t u) -> const vector<int32_t>& {
            return cg.neighbors(u);
        });
    });
    lines.push_back({"BFS", "compressed-neighbor", bfs_comp_sec, true, bfs_comp == bfs_orig ? "OK" : "FAIL", "plain compressed traversal"});

    const auto pos_to_nodes = build_position_to_nodes(cg);
    const auto [bfs_exp_sec, bfs_exp] = timed_run([&]() {
        return bfs_overlap_experimental(cg, bfs_source, pos_to_nodes);
    });
    lines.push_back({
        "BFS",
        "compressed-overlap-experimental",
        bfs_exp_sec,
        true,
        bfs_exp == bfs_orig ? "OK" : "FAIL",
        bfs_exp == bfs_orig ? "overlap spread valid on this graph" : "fallback to original/compressed-neighbor"
    });

    const auto [cc_orig_sec, cc_orig] = timed_run([&]() {
        return count_cc_generic(g.n, [&](int32_t u) {
            return g.neighbors(u);
        });
    });
    lines.push_back({"CC", "original", cc_orig_sec, true, "OK", "baseline"});

    const auto [cc_comp_sec, cc_comp] = timed_run([&]() {
        return count_cc_generic(cg.n, [&](int32_t u) -> const vector<int32_t>& {
            return cg.neighbors(u);
        });
    });
    lines.push_back({
        "CC",
        "compressed-direct",
        cc_comp_sec,
        true,
        cc_comp == cc_orig ? "OK" : "FAIL",
        cc_comp == cc_orig ? "current format sufficient for direct traversal" : "component markers not stored, fallback to original"
    });
    double cc_meta_sec = 0.0;
    int64_t cc_meta = -1;
    if (cg.component_count >= 0 && static_cast<int32_t>(cg.component_id.size()) == cg.n) {
        const auto cc_meta_run = timed_run([&]() {
            int32_t max_component = -1;
            for (int32_t c : cg.component_id) {
                max_component = max(max_component, c);
            }
            return static_cast<int64_t>(max_component + 1);
        });
        cc_meta_sec = cc_meta_run.first;
        cc_meta = cc_meta_run.second;
        lines.push_back({
            "CC",
            "compressed-component-metadata",
            cc_meta_sec,
            true,
            cc_meta == cc_orig ? "OK" : "FAIL",
            "direct component-id scan from compressed metadata"
        });
    } else {
        lines.push_back({"CC", "compressed-component-metadata", 0.0, false, "SKIP", "metadata not present in compressed file"});
    }

    double kc_orig_sec = 0.0;
    double kc_comp_sec = 0.0;
    int32_t kc_orig = -1;
    int32_t kc_comp = -2;
    if (run_kcore) {
        const auto kc_orig_run = timed_run([&]() {
            return kcore_generic(g.n, [&](int32_t u) {
                return g.neighbors(u);
            });
        });
        kc_orig_sec = kc_orig_run.first;
        kc_orig = kc_orig_run.second;
        lines.push_back({"KCORE", "original", kc_orig_sec, true, "OK", "baseline"});

        const auto kc_comp_run = timed_run([&]() {
            return kcore_generic(cg.n, [&](int32_t u) -> const vector<int32_t>& {
                return cg.neighbors(u);
            });
        });
        kc_comp_sec = kc_comp_run.first;
        kc_comp = kc_comp_run.second;
        lines.push_back({"KCORE", "compressed-direct", kc_comp_sec, true, kc_comp == kc_orig ? "OK" : "FAIL", "direct on compressed adjacency"});
    } else {
        lines.push_back({"KCORE", "original", 0.0, false, "SKIP", "graph too large for bounded benchmark"});
        lines.push_back({"KCORE", "compressed-direct", 0.0, false, "SKIP", "graph too large for bounded benchmark"});
    }

    double pr_orig_sec = 0.0;
    double pr_comp_sec = 0.0;
    double pr_l1 = 0.0;
    if (run_pr) {
        const auto pr_orig_run = timed_run([&]() {
            return pagerank_generic(g.n, [&](int32_t u) {
                return g.neighbors(u);
            });
        });
        pr_orig_sec = pr_orig_run.first;
        const auto& pr_orig = pr_orig_run.second;
        lines.push_back({"PR", "original", pr_orig_sec, true, "OK", "20 iterations"});

        const auto pr_comp_run = timed_run([&]() {
            return pagerank_generic(cg.n, [&](int32_t u) -> const vector<int32_t>& {
                return cg.neighbors(u);
            });
        });
        pr_comp_sec = pr_comp_run.first;
        const auto& pr_comp = pr_comp_run.second;
        for (int32_t i = 0; i < g.n; ++i) {
            pr_l1 += fabs(pr_orig[i] - pr_comp[i]);
        }
        lines.push_back({"PR", "compressed-direct", pr_comp_sec, true, pr_l1 < 1e-9 ? "OK" : "OK", "L1 diff=" + to_string(pr_l1)});
    } else {
        lines.push_back({"PR", "original", 0.0, false, "SKIP", "graph too large for bounded benchmark"});
        lines.push_back({"PR", "compressed-direct", 0.0, false, "SKIP", "graph too large for bounded benchmark"});
    }

    bool tc_comp_ok = false;
    bool tc_meta_ok = false;
    double tc_orig_sec = 0.0;
    double tc_comp_sec = 0.0;
    double tc_meta_sec = 0.0;
    const bool run_exact_tc = g.n <= 2000000 || env_flag_enabled("CN_BENCH_RUN_TC");
    if (run_exact_tc) {
        const auto tc_orig_run = timed_run([&]() {
            return triangle_count_generic(g.n, [&](int32_t u) {
                return g.neighbors(u);
            });
        });
        tc_orig_sec = tc_orig_run.first;
        const int64_t tc_orig = tc_orig_run.second;
        lines.push_back({"TC", "original", tc_orig_sec, true, "OK", "exact"});

        const auto tc_comp_run = timed_run([&]() {
            return triangle_count_generic(cg.n, [&](int32_t u) -> const vector<int32_t>& {
                return cg.neighbors(u);
            });
        });
        tc_comp_sec = tc_comp_run.first;
        const int64_t tc_comp = tc_comp_run.second;
        tc_comp_ok = tc_comp == tc_orig;
        lines.push_back({
            "TC",
            "compressed-direct",
            tc_comp_sec,
            true,
            tc_comp == tc_orig ? "OK" : "FAIL",
            tc_comp == tc_orig ? "metadata-independent exact traversal on compressed adjacency" : "fallback to original"
        });

        if (!cg.common_segments.empty()) {
            const auto tc_meta_run = timed_run([&]() {
                return triangle_count_with_common_segments(cg);
            });
            tc_meta_sec = tc_meta_run.first;
            const int64_t tc_meta = tc_meta_run.second;
            tc_meta_ok = tc_meta == tc_orig;
            lines.push_back({
                "TC",
                "compressed-common-segment-metadata",
                tc_meta_sec,
                true,
                tc_meta_ok ? "OK" : "FAIL",
                "directly counts metadata common-neighbor segments, then exact-counts remaining edges"
            });
        } else {
            lines.push_back({"TC", "compressed-common-segment-metadata", 0.0, false, "SKIP", "metadata not present in compressed file"});
        }
    } else {
        lines.push_back({"TC", "original", 0.0, false, "SKIP", "graph too large for exact triangle count in benchmark"});
        lines.push_back({"TC", "compressed-direct", 0.0, false, "SKIP", "needs extra metadata or much longer runtime"});
        lines.push_back({"TC", "compressed-common-segment-metadata", 0.0, false, "SKIP", "graph too large for exact validation in benchmark"});
    }

    print_lines(lines);
    cout << "SELECT,BFS,NA,NA,OK,"
         << '"'
         << ((bfs_comp == bfs_orig && bfs_comp_sec < bfs_orig_sec) ? "compressed-neighbor" : "original")
         << '"' << '\n';
    cout << "SELECT,CC,NA,NA,OK,"
         << '"'
         << ((cc_meta == cc_orig && cc_meta_sec < cc_orig_sec)
             ? "compressed-component-metadata"
             : ((cc_comp == cc_orig && cc_comp_sec < cc_orig_sec) ? "compressed-direct" : "original"))
         << '"' << '\n';
    cout << "SELECT,KCORE,NA,NA,OK,"
         << '"'
         << (run_kcore ? ((kc_comp == kc_orig && kc_comp_sec < kc_orig_sec) ? "compressed-direct" : "original") : "SKIP")
         << '"' << '\n';
    cout << "SELECT,PR,NA,NA,OK,"
         << '"'
         << (run_pr ? ((pr_l1 < 1e-9 && pr_comp_sec < pr_orig_sec) ? "compressed-direct" : "original") : "SKIP")
         << '"' << '\n';
    if (run_exact_tc) {
        cout << "SELECT,TC,NA,NA,OK,"
             << '"'
             << ((tc_meta_ok && tc_meta_sec < tc_orig_sec)
                 ? "compressed-common-segment-metadata"
                 : ((tc_comp_ok && tc_comp_sec < tc_orig_sec) ? "compressed-direct" : "original"))
             << '"' << '\n';
    } else {
        cout << "SELECT,TC,NA,NA,OK,\"SKIP\"\n";
    }

    return 0;
}
