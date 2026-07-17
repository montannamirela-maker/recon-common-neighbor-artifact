#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;

struct Timer {
    Clock::time_point start = Clock::now();

    double elapsed_sec() const {
        return chrono::duration<double>(Clock::now() - start).count();
    }
};

struct PhaseTimes {
    double pass1_sec = 0.0;
    double pass2_sec = 0.0;
    double sort_sec = 0.0;
    double partition_sec = 0.0;
    double pair_count_sec = 0.0;
    double local_merge_sec = 0.0;
    double global_merge_sec = 0.0;
    double verify_sec = 0.0;
    double total_sec = 0.0;
};

struct Graph {
    int32_t n = 0;
    uint64_t m_input = 0;
    vector<uint64_t> offsets;
    vector<int32_t> edges;

    uint64_t degree(int32_t u) const {
        return offsets[u + 1] - offsets[u];
    }

    const int32_t* begin(int32_t u) const {
        return edges.data() + offsets[u];
    }

    vector<int32_t> neighbors(int32_t u) const {
        return vector<int32_t>(edges.begin() + static_cast<ptrdiff_t>(offsets[u]),
                               edges.begin() + static_cast<ptrdiff_t>(offsets[u + 1]));
    }

    size_t original_bytes() const {
        return edges.size() * sizeof(int32_t);
    }
};

struct NodeRange {
    int32_t start = -1;
    int32_t end = -1;
};

struct CommonSegment {
    int32_t a = -1;
    int32_t b = -1;
    int32_t start = -1;
    int32_t end = -1;
};

struct Chain {
    vector<int32_t> values;
    unordered_map<int32_t, NodeRange> ranges;
    vector<CommonSegment> common_segments;
};

struct NodeState {
    vector<int32_t> residual;
    int chain_id = -1;
    NodeRange range;
    bool consumed = false;
};

struct PartitionResult {
    Chain chain;
    uint64_t successful_merges = 0;
    double pair_count_sec = 0.0;
    double local_merge_sec = 0.0;
};

struct CompressionResult {
    Chain global_chain;
    PhaseTimes times;
    uint64_t original_bytes = 0;
    uint64_t compressed_bytes = 0;
    uint64_t metadata_bytes = 0;
    uint64_t partitions = 0;
    uint64_t merges = 0;
    int32_t component_count = 0;
    vector<int32_t> component_id;
    bool verified = false;
};

struct Config {
    string input_path;
    string output_path;
    int partition_limit = 0;
    int bfs_hops = 6;
    int min_common = 0;
    int max_postings = 0;
    int max_pair_nodes = 0;
    int threads = max(1u, thread::hardware_concurrency());
    bool verify = false;
    bool disable_residual_stitching = false;
};

static bool has_suffix(const string& s, const string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool next_non_comment_line(ifstream& in, string& line, char comment = '%') {
    while (getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (line[0] == comment) {
            continue;
        }
        return true;
    }
    return false;
}

static bool is_comment_or_empty(const string& line) {
    if (line.empty()) {
        return true;
    }
    const unsigned char c = static_cast<unsigned char>(line[0]);
    return c == '#' || c == '%' || c == '/';
}

static bool parse_edge_line(string line, int64_t& u, int64_t& v) {
    for (char& ch : line) {
        if (ch == ',') {
            ch = ' ';
        }
    }
    istringstream iss(line);
    return static_cast<bool>(iss >> u >> v);
}

static Graph build_graph_from_edges(
    const vector<pair<int32_t, int32_t>>& input_edges,
    int32_t n,
    bool undirected,
    uint64_t m_input,
    double& sort_sec
) {
    Graph g;
    g.n = n;
    g.m_input = m_input;

    vector<uint64_t> degree(g.n, 0);
    for (const auto& e : input_edges) {
        ++degree[e.first];
        if (undirected && e.first != e.second) {
            ++degree[e.second];
        }
    }

    g.offsets.assign(g.n + 1, 0);
    for (int32_t i = 0; i < g.n; ++i) {
        g.offsets[i + 1] = g.offsets[i] + degree[i];
    }
    g.edges.assign(g.offsets.back(), -1);

    vector<uint64_t> cursor = g.offsets;
    for (const auto& e : input_edges) {
        g.edges[cursor[e.first]++] = e.second;
        if (undirected && e.first != e.second) {
            g.edges[cursor[e.second]++] = e.first;
        }
    }

    Timer t;
    const int threads = max(1u, thread::hardware_concurrency());
    atomic<int32_t> next_node(0);
    vector<uint64_t> unique_deg(g.n, 0);
    vector<thread> workers;
    workers.reserve(threads);

    auto sort_worker = [&]() {
        for (;;) {
            const int32_t u = next_node.fetch_add(1);
            if (u >= g.n) {
                break;
            }
            auto first = g.edges.begin() + static_cast<ptrdiff_t>(g.offsets[u]);
            auto last = g.edges.begin() + static_cast<ptrdiff_t>(g.offsets[u + 1]);
            sort(first, last);
            auto it = unique(first, last);
            unique_deg[u] = static_cast<uint64_t>(it - first);
        }
    };

    for (int i = 0; i < threads; ++i) {
        workers.emplace_back(sort_worker);
    }
    for (auto& th : workers) {
        th.join();
    }

    vector<uint64_t> new_offsets(g.n + 1, 0);
    for (int32_t u = 0; u < g.n; ++u) {
        new_offsets[u + 1] = new_offsets[u] + unique_deg[u];
    }

    vector<int32_t> compact(new_offsets.back());
    atomic<int32_t> write_node(0);
    workers.clear();
    auto write_worker = [&]() {
        for (;;) {
            const int32_t u = write_node.fetch_add(1);
            if (u >= g.n) {
                break;
            }
            auto first = g.edges.begin() + static_cast<ptrdiff_t>(g.offsets[u]);
            auto last = first + static_cast<ptrdiff_t>(unique_deg[u]);
            copy(first, last, compact.begin() + static_cast<ptrdiff_t>(new_offsets[u]));
        }
    };
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back(write_worker);
    }
    for (auto& th : workers) {
        th.join();
    }

    g.offsets.swap(new_offsets);
    g.edges.swap(compact);
    sort_sec = t.elapsed_sec();
    return g;
}

static Graph read_matrix_market_auto(const string& path, PhaseTimes& times) {
    Timer total;
    ifstream in(path);
    if (!in) {
        throw runtime_error("cannot open input file: " + path);
    }

    string header;
    if (!getline(in, header)) {
        throw runtime_error("empty MatrixMarket file: " + path);
    }
    const bool symmetric = header.find("symmetric") != string::npos;

    string size_line;
    if (!next_non_comment_line(in, size_line, '%')) {
        throw runtime_error("invalid MatrixMarket size line: " + path);
    }

    uint64_t rows = 0;
    uint64_t cols = 0;
    uint64_t nnz = 0;
    {
        istringstream iss(size_line);
        iss >> rows >> cols >> nnz;
        if (!iss) {
            throw runtime_error("failed to parse MatrixMarket size line: " + path);
        }
    }

    times.pass1_sec = total.elapsed_sec();
    Timer pass2;
    vector<pair<int32_t, int32_t>> edges;
    edges.reserve(nnz);
    int64_t u = 0;
    int64_t v = 0;
    while (in >> u >> v) {
        --u;
        --v;
        if (u < 0 || v < 0 || u >= static_cast<int64_t>(rows) || v >= static_cast<int64_t>(cols)) {
            continue;
        }
        edges.push_back({static_cast<int32_t>(u), static_cast<int32_t>(v)});
    }
    times.pass2_sec = pass2.elapsed_sec();

    return build_graph_from_edges(edges, static_cast<int32_t>(max(rows, cols)), symmetric, nnz, times.sort_sec);
}

static Graph read_edge_list_auto(const string& path, PhaseTimes& times) {
    Timer pass1;
    ifstream in(path);
    if (!in) {
        throw runtime_error("cannot open edge list: " + path);
    }

    string line;
    int64_t min_node = numeric_limits<int64_t>::max();
    int64_t max_node = numeric_limits<int64_t>::min();
    uint64_t edges_count = 0;

    while (getline(in, line)) {
        if (is_comment_or_empty(line)) {
            continue;
        }
        int64_t u = 0;
        int64_t v = 0;
        if (!parse_edge_line(line, u, v)) {
            continue;
        }
        min_node = min(min_node, min(u, v));
        max_node = max(max_node, max(u, v));
        ++edges_count;
    }

    if (edges_count == 0) {
        throw runtime_error("empty edge list: " + path);
    }
    times.pass1_sec = pass1.elapsed_sec();

    const int64_t shift = (min_node >= 1) ? 1 : 0;
    const int32_t n = static_cast<int32_t>(max_node - shift + 1);

    Timer pass2;
    in.clear();
    in.seekg(0);
    vector<pair<int32_t, int32_t>> edges;
    edges.reserve(edges_count);

    while (getline(in, line)) {
        if (is_comment_or_empty(line)) {
            continue;
        }
        int64_t u = 0;
        int64_t v = 0;
        if (!parse_edge_line(line, u, v)) {
            continue;
        }
        u -= shift;
        v -= shift;
        if (u < 0 || v < 0 || u >= n || v >= n) {
            continue;
        }
        edges.push_back({static_cast<int32_t>(u), static_cast<int32_t>(v)});
    }
    times.pass2_sec = pass2.elapsed_sec();

    return build_graph_from_edges(edges, n, true, edges_count, times.sort_sec);
}

static Graph read_graph_auto(const string& path, PhaseTimes& times) {
    if (has_suffix(path, ".mtx")) {
        return read_matrix_market_auto(path, times);
    }
    if (has_suffix(path, ".edges")) {
        return read_edge_list_auto(path, times);
    }
    throw runtime_error("unsupported input format: " + path);
}

static int intersection_size(const vector<int32_t>& a, const vector<int32_t>& b) {
    size_t i = 0;
    size_t j = 0;
    int count = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            ++count;
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return count;
}

static vector<int32_t> set_intersection_vec(const vector<int32_t>& a, const vector<int32_t>& b) {
    vector<int32_t> out;
    out.reserve(min(a.size(), b.size()));
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out.push_back(a[i]);
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

static vector<int32_t> set_difference_vec(const vector<int32_t>& a, const vector<int32_t>& b) {
    vector<int32_t> out;
    out.reserve(a.size());
    size_t i = 0;
    size_t j = 0;
    while (i < a.size()) {
        if (j >= b.size() || a[i] < b[j]) {
            out.push_back(a[i]);
            ++i;
        } else if (a[i] == b[j]) {
            ++i;
            ++j;
        } else {
            ++j;
        }
    }
    return out;
}

static int suffix_prefix_overlap(
    const vector<int32_t>& left,
    const vector<int32_t>& right,
    int max_check
) {
    const int limit = min<int>({static_cast<int>(left.size()), static_cast<int>(right.size()), max_check});
    for (int len = limit; len >= 1; --len) {
        bool ok = true;
        const int left_start = static_cast<int>(left.size()) - len;
        for (int i = 0; i < len; ++i) {
            if (left[left_start + i] != right[i]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return len;
        }
    }
    return 0;
}

static Chain greedy_merge_items_with_overlap(vector<Chain> items, int max_overlap_check) {
    Chain merged;
    if (items.empty()) {
        return merged;
    }

    vector<uint8_t> used(items.size(), 0);
    size_t current = 0;
    for (size_t i = 1; i < items.size(); ++i) {
        if (items[i].values.size() > items[current].values.size()) {
            current = i;
        }
    }

    merged = std::move(items[current]);
    used[current] = 1;

    for (size_t step = 1; step < items.size(); ++step) {
        int best_idx = -1;
        int best_overlap = -1;
        size_t best_size = 0;

        for (size_t i = 0; i < items.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const int overlap = suffix_prefix_overlap(merged.values, items[i].values, max_overlap_check);
            if (overlap > best_overlap ||
                (overlap == best_overlap && items[i].values.size() > best_size)) {
                best_idx = static_cast<int>(i);
                best_overlap = overlap;
                best_size = items[i].values.size();
            }
        }

        if (best_idx == -1) {
            break;
        }

        Chain& add = items[best_idx];
        const int overlap = max(0, best_overlap);
        const int32_t shift = static_cast<int32_t>(merged.values.size()) - overlap;
        for (const auto& kv : add.ranges) {
            merged.ranges[kv.first] = {kv.second.start + shift, kv.second.end + shift};
        }
        for (CommonSegment seg : add.common_segments) {
            seg.start += shift;
            seg.end += shift;
            merged.common_segments.push_back(seg);
        }
        merged.values.insert(merged.values.end(), add.values.begin() + overlap, add.values.end());
        used[best_idx] = 1;
    }

    return merged;
}

static Chain concatenate_items(vector<Chain> items) {
    Chain merged;
    size_t total_values = 0;
    size_t total_segments = 0;
    for (const auto& item : items) {
        total_values += item.values.size();
        total_segments += item.common_segments.size();
    }
    merged.values.reserve(total_values);
    merged.common_segments.reserve(total_segments);

    int32_t shift = 0;
    for (auto& item : items) {
        for (const auto& kv : item.ranges) {
            merged.ranges[kv.first] = {kv.second.start + shift, kv.second.end + shift};
        }
        for (CommonSegment seg : item.common_segments) {
            seg.start += shift;
            seg.end += shift;
            merged.common_segments.push_back(seg);
        }
        merged.values.insert(merged.values.end(), item.values.begin(), item.values.end());
        shift = static_cast<int32_t>(merged.values.size());
    }
    return merged;
}

static bool fast_global_concat_enabled() {
    const char* value = getenv("CN_FAST_GLOBAL_CONCAT");
    return value != nullptr && string(value) != "0" && string(value) != "";
}

static vector<int32_t> compute_components(const Graph& g, int32_t& component_count) {
    vector<int32_t> comp(g.n, -1);
    queue<int32_t> q;
    component_count = 0;
    for (int32_t s = 0; s < g.n; ++s) {
        if (comp[s] != -1) {
            continue;
        }
        comp[s] = component_count;
        q.push(s);
        while (!q.empty()) {
            int32_t u = q.front();
            q.pop();
            for (const int32_t* it = g.begin(u); it != g.begin(u) + static_cast<ptrdiff_t>(g.degree(u)); ++it) {
                int32_t v = *it;
                if (v >= 0 && v < g.n && comp[v] == -1) {
                    comp[v] = component_count;
                    q.push(v);
                }
            }
        }
        ++component_count;
    }
    return comp;
}

static void auto_tune_config(const Graph& g, Config& cfg) {
    const uint64_t edges = g.m_input;
    if (cfg.partition_limit <= 0) {
        if (edges >= 100000000ULL) cfg.partition_limit = 384;
        else if (edges >= 30000000ULL) cfg.partition_limit = 448;
        else if (edges >= 10000000ULL) cfg.partition_limit = 512;
        else if (edges >= 1000000ULL) cfg.partition_limit = 512;
        else cfg.partition_limit = 384;
    }
    if (cfg.min_common <= 0) {
        if (edges >= 100000000ULL) cfg.min_common = 5;
        else if (edges >= 30000000ULL) cfg.min_common = 4;
        else if (edges >= 10000000ULL) cfg.min_common = 3;
        else cfg.min_common = 2;
    }
    if (cfg.max_postings <= 0) {
        if (edges >= 100000000ULL) cfg.max_postings = 96;
        else if (edges >= 30000000ULL) cfg.max_postings = 128;
        else if (edges >= 10000000ULL) cfg.max_postings = 160;
        else cfg.max_postings = 256;
    }
    if (cfg.max_pair_nodes <= 0) {
        if (edges >= 100000000ULL) cfg.max_pair_nodes = 96;
        else if (edges >= 30000000ULL) cfg.max_pair_nodes = 128;
        else if (edges >= 10000000ULL) cfg.max_pair_nodes = 192;
        else if (edges >= 1000000ULL) cfg.max_pair_nodes = 384;
        else cfg.max_pair_nodes = 768;
    }
}

static vector<vector<int32_t>> create_partitions(const Graph& g, int limit, int bfs_hops, double& part_sec) {
    Timer t;
    vector<vector<int32_t>> parts;
    vector<uint8_t> assigned(g.n, 0);
    vector<int32_t> q;
    vector<int32_t> dist;
    q.reserve(limit * 2);
    dist.reserve(limit * 2);

    vector<int32_t> seeds;
    seeds.reserve(g.n);
    for (int32_t u = 0; u < g.n; ++u) {
        if (g.degree(u) > 0) {
            seeds.push_back(u);
        }
    }
    sort(seeds.begin(), seeds.end(), [&](int32_t a, int32_t b) {
        const uint64_t da = g.degree(a);
        const uint64_t db = g.degree(b);
        if (da != db) {
            return da > db;
        }
        return a < b;
    });

    for (int32_t seed : seeds) {
        if (assigned[seed] || g.degree(seed) == 0) {
            continue;
        }
        parts.push_back({});
        auto& part = parts.back();
        q.clear();
        dist.clear();
        q.push_back(seed);
        dist.push_back(0);
        assigned[seed] = 1;

        for (size_t head = 0; head < q.size() && part.size() < static_cast<size_t>(limit); ++head) {
            int32_t u = q[head];
            int d = dist[head];
            part.push_back(u);
            if (d >= bfs_hops) {
                continue;
            }
            for (const int32_t* it = g.begin(u); it != g.begin(u) + static_cast<ptrdiff_t>(g.degree(u)); ++it) {
                int32_t v = *it;
                if (!assigned[v] && q.size() < static_cast<size_t>(limit)) {
                    assigned[v] = 1;
                    q.push_back(v);
                    dist.push_back(d + 1);
                    if (q.size() >= static_cast<size_t>(limit)) {
                        break;
                    }
                }
            }
        }
    }

    for (int32_t u = 0; u < g.n; ++u) {
        if (g.degree(u) > 0 && !assigned[u]) {
            parts.push_back({u});
        }
    }
    part_sec = t.elapsed_sec();
    return parts;
}

struct PairChoice {
    int32_t a = -1;
    int32_t b = -1;
    int common = 0;
};

static PairChoice find_best_pair(
    const vector<int32_t>& nodes,
    const unordered_map<int32_t, NodeState>& states,
    int min_common,
    int max_postings,
    int max_pair_nodes,
    double& pair_count_sec
) {
    Timer t;
    vector<pair<int32_t, size_t>> ranked_nodes;
    ranked_nodes.reserve(nodes.size());

    for (int32_t u : nodes) {
        auto it = states.find(u);
        if (it == states.end()) {
            continue;
        }
        const NodeState& st = it->second;
        if (st.consumed || st.residual.size() < static_cast<size_t>(min_common)) {
            continue;
        }
        ranked_nodes.push_back({u, st.residual.size()});
    }

    sort(ranked_nodes.begin(), ranked_nodes.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    if (static_cast<int>(ranked_nodes.size()) > max_pair_nodes) {
        ranked_nodes.resize(max_pair_nodes);
    }

    unordered_map<int32_t, vector<int32_t>> postings;
    postings.reserve(ranked_nodes.size() * 4);

    for (const auto& item : ranked_nodes) {
        const int32_t u = item.first;
        const NodeState& st = states.at(u);
        for (int32_t x : st.residual) {
            auto& vec = postings[x];
            if (static_cast<int>(vec.size()) < max_postings) {
                vec.push_back(u);
            }
        }
    }

    unordered_map<uint64_t, int> counter;
    counter.reserve(ranked_nodes.size() * 8);
    auto key = [](int32_t x, int32_t y) {
        if (x > y) {
            swap(x, y);
        }
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(y);
    };

    for (const auto& kv : postings) {
        const auto& vec = kv.second;
        for (size_t i = 0; i < vec.size(); ++i) {
            for (size_t j = i + 1; j < vec.size(); ++j) {
                ++counter[key(vec[i], vec[j])];
            }
        }
    }

    PairChoice best;
    for (const auto& kv : counter) {
        if (kv.second < min_common || kv.second < best.common) {
            continue;
        }
        int32_t a = static_cast<int32_t>(kv.first >> 32);
        int32_t b = static_cast<int32_t>(kv.first & 0xffffffffu);
        const auto ita = states.find(a);
        const auto itb = states.find(b);
        if (ita == states.end() || itb == states.end()) {
            continue;
        }
        const NodeState& sa = ita->second;
        const NodeState& sb = itb->second;
        if (sa.consumed || sb.consumed || sa.chain_id != -1 || sb.chain_id != -1) {
            continue;
        }
        const int exact = intersection_size(sa.residual, sb.residual);
        if (exact > best.common) {
            best = {a, b, exact};
        }
    }

    pair_count_sec += t.elapsed_sec();
    return best;
}

static bool merge_pair_into_partition(
    int32_t a,
    int32_t b,
    unordered_map<int32_t, NodeState>& states,
    vector<Chain>& chains
) {
    NodeState& sa = states[a];
    NodeState& sb = states[b];
    vector<int32_t> common = set_intersection_vec(sa.residual, sb.residual);
    if (common.empty()) {
        return false;
    }

    vector<int32_t> a_only = set_difference_vec(sa.residual, common);
    vector<int32_t> b_only = set_difference_vec(sb.residual, common);
    if (sa.chain_id != -1 || sb.chain_id != -1) {
        return false;
    }

    Chain chain;
    chain.values.reserve(a_only.size() + common.size() + b_only.size());
    chain.values.insert(chain.values.end(), a_only.begin(), a_only.end());
    chain.values.insert(chain.values.end(), common.begin(), common.end());
    chain.values.insert(chain.values.end(), b_only.begin(), b_only.end());
    chain.common_segments.push_back({
        a,
        b,
        static_cast<int32_t>(a_only.size()),
        static_cast<int32_t>(a_only.size() + common.size()) - 1
    });

    const int32_t cid = static_cast<int32_t>(chains.size());
    chains.push_back(std::move(chain));
    Chain& c = chains.back();

    sa.chain_id = cid;
    sa.range = {0, static_cast<int32_t>(a_only.size() + common.size()) - 1};
    sa.consumed = true;
    sa.residual.clear();
    c.ranges[a] = sa.range;

    sb.chain_id = cid;
    sb.range = {static_cast<int32_t>(a_only.size()), static_cast<int32_t>(c.values.size()) - 1};
    sb.consumed = true;
    sb.residual.clear();
    c.ranges[b] = sb.range;
    return true;
}

static PartitionResult compress_partition(const Graph& g, const vector<int32_t>& part_nodes, const Config& cfg) {
    PartitionResult out;
    unordered_map<int32_t, NodeState> states;
    states.reserve(part_nodes.size() * 2);

    for (int32_t u : part_nodes) {
        if (g.degree(u) == 0) {
            continue;
        }
        NodeState st;
        st.residual = g.neighbors(u);
        states.emplace(u, std::move(st));
    }

    vector<Chain> chains;
    int idle_rounds = 0;
    while (states.size() >= 2) {
        PairChoice best = find_best_pair(
            part_nodes,
            states,
            cfg.min_common,
            cfg.max_postings,
            cfg.max_pair_nodes,
            out.pair_count_sec
        );
        if (best.a == -1 || best.common < cfg.min_common) {
            break;
        }
        Timer t;
        const bool merged = merge_pair_into_partition(best.a, best.b, states, chains);
        out.local_merge_sec += t.elapsed_sec();
        if (!merged) {
            ++idle_rounds;
            if (idle_rounds >= 8) {
                break;
            }
            states[best.a].consumed = true;
            states[best.b].consumed = true;
            continue;
        }
        idle_rounds = 0;
        ++out.successful_merges;
    }

    vector<Chain> items;
    items.reserve(chains.size() + states.size());
    for (auto& c : chains) {
        if (!c.values.empty()) {
            items.push_back(std::move(c));
        }
    }
    for (const auto& kv : states) {
        int32_t node = kv.first;
        const NodeState& st = kv.second;
        if (st.chain_id != -1 || g.degree(node) == 0) {
            continue;
        }
        Chain singleton;
        singleton.values = g.neighbors(node);
        singleton.ranges[node] = {0, static_cast<int32_t>(singleton.values.size()) - 1};
        items.push_back(std::move(singleton));
    }

    if (items.empty()) {
        return out;
    }

    if (cfg.disable_residual_stitching) {
        out.chain = concatenate_items(std::move(items));
    } else {
        out.chain = greedy_merge_items_with_overlap(std::move(items), 4096);
    }

    return out;
}

static CompressionResult compress_graph(const Graph& g, const Config& cfg, PhaseTimes times) {
    CompressionResult result;
    result.original_bytes = g.original_bytes();
    result.times = times;
    result.component_id = compute_components(g, result.component_count);

    vector<vector<int32_t>> partitions = create_partitions(g, cfg.partition_limit, cfg.bfs_hops, result.times.partition_sec);
    result.partitions = partitions.size();

    Timer global_merge_timer;
    vector<PartitionResult> part_results(partitions.size());
    atomic<size_t> next_part(0);
    vector<thread> workers;
    workers.reserve(cfg.threads);

    auto worker = [&]() {
        for (;;) {
            size_t idx = next_part.fetch_add(1);
            if (idx >= partitions.size()) {
                break;
            }
            part_results[idx] = compress_partition(g, partitions[idx], cfg);
        }
    };

    for (int i = 0; i < cfg.threads; ++i) {
        workers.emplace_back(worker);
    }
    for (auto& th : workers) {
        th.join();
    }

    vector<Chain> chains;
    chains.reserve(part_results.size());
    for (auto& pr : part_results) {
        result.merges += pr.successful_merges;
        result.times.pair_count_sec += pr.pair_count_sec;
        result.times.local_merge_sec += pr.local_merge_sec;
        if (!pr.chain.values.empty()) {
            chains.push_back(std::move(pr.chain));
        }
    }

    if (!chains.empty() && (cfg.disable_residual_stitching || fast_global_concat_enabled())) {
        result.global_chain = concatenate_items(std::move(chains));
    } else if (!chains.empty()) {
        result.global_chain = greedy_merge_items_with_overlap(std::move(chains), 8192);
    }

    result.times.global_merge_sec = global_merge_timer.elapsed_sec();
    result.compressed_bytes =
        result.global_chain.values.size() * sizeof(int32_t) +
        result.global_chain.ranges.size() * (sizeof(NodeRange) + sizeof(int32_t));
    result.metadata_bytes =
        result.component_id.size() * sizeof(int32_t) +
        result.global_chain.common_segments.size() * sizeof(CommonSegment) +
        sizeof(uint32_t) * 2 + sizeof(int32_t) + sizeof(uint64_t);
    return result;
}

static bool verify_compression(const Graph& g, const Chain& chain, double& verify_sec) {
    Timer t;
    vector<int32_t> recovered;
    for (int32_t u = 0; u < g.n; ++u) {
        auto it = chain.ranges.find(u);
        if (g.degree(u) == 0) {
            if (it != chain.ranges.end()) {
                verify_sec += t.elapsed_sec();
                return false;
            }
            continue;
        }
        if (it == chain.ranges.end()) {
            verify_sec += t.elapsed_sec();
            return false;
        }
        if (it->second.start < 0 || it->second.end < it->second.start ||
            it->second.end >= static_cast<int32_t>(chain.values.size())) {
            verify_sec += t.elapsed_sec();
            return false;
        }
        recovered.assign(chain.values.begin() + it->second.start, chain.values.begin() + it->second.end + 1);
        sort(recovered.begin(), recovered.end());
        recovered.erase(unique(recovered.begin(), recovered.end()), recovered.end());
        if (recovered.size() != g.degree(u)) {
            verify_sec += t.elapsed_sec();
            return false;
        }
        if (!equal(recovered.begin(), recovered.end(),
                   g.edges.begin() + static_cast<ptrdiff_t>(g.offsets[u]))) {
            verify_sec += t.elapsed_sec();
            return false;
        }
    }
    verify_sec += t.elapsed_sec();
    return true;
}

static void save_compressed(const CompressionResult& result, const string& path) {
    const Chain& chain = result.global_chain;
    ofstream out(path, ios::binary);
    if (!out) {
        throw runtime_error("cannot write output file: " + path);
    }
    uint64_t chain_len = chain.values.size();
    uint64_t ranges = chain.ranges.size();
    out.write(reinterpret_cast<const char*>(&chain_len), sizeof(chain_len));
    out.write(reinterpret_cast<const char*>(chain.values.data()),
              static_cast<streamsize>(chain_len * sizeof(int32_t)));
    out.write(reinterpret_cast<const char*>(&ranges), sizeof(ranges));
    for (const auto& kv : chain.ranges) {
        out.write(reinterpret_cast<const char*>(&kv.first), sizeof(kv.first));
        out.write(reinterpret_cast<const char*>(&kv.second), sizeof(kv.second));
    }

    const uint32_t magic = 0x314d4347u; // GCM1
    const uint32_t version = 1;
    const int32_t component_count = result.component_count;
    const uint64_t component_nodes = result.component_id.size();
    const uint64_t segment_count = chain.common_segments.size();
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&component_count), sizeof(component_count));
    out.write(reinterpret_cast<const char*>(&component_nodes), sizeof(component_nodes));
    out.write(reinterpret_cast<const char*>(result.component_id.data()),
              static_cast<streamsize>(component_nodes * sizeof(int32_t)));
    out.write(reinterpret_cast<const char*>(&segment_count), sizeof(segment_count));
    out.write(reinterpret_cast<const char*>(chain.common_segments.data()),
              static_cast<streamsize>(segment_count * sizeof(CommonSegment)));
}

static void print_summary(const Graph& g, const CompressionResult& r, const Config& cfg) {
    const double base_edges = static_cast<double>(g.m_input) * 2.0;
    const double chain_ratio = base_edges == 0.0
        ? 0.0
        : static_cast<double>(r.global_chain.values.size()) / base_edges;
    const double chain_saving = base_edges == 0.0 ? 0.0 : (1.0 - chain_ratio) * 100.0;

    cout << fixed << setprecision(3);
    cout << "Input graph\n";
    cout << "  path                  : " << cfg.input_path << '\n';
    cout << "  nodes                 : " << g.n << '\n';
    cout << "  input edges(records)  : " << g.m_input << '\n';
    cout << "  unique edges(stored)  : " << g.edges.size() << '\n';
    cout << '\n';
    cout << "Compression config\n";
    cout << "  partition_limit       : " << cfg.partition_limit << '\n';
    cout << "  bfs_hops              : " << cfg.bfs_hops << '\n';
    cout << "  min_common            : " << cfg.min_common << '\n';
    cout << "  max_postings          : " << cfg.max_postings << '\n';
    cout << "  max_pair_nodes        : " << cfg.max_pair_nodes << '\n';
    cout << "  threads               : " << cfg.threads << '\n';
    cout << "  verify                : " << (cfg.verify ? 1 : 0) << '\n';
    cout << "  residual_stitching    : " << (cfg.disable_residual_stitching ? 0 : 1) << '\n';
    cout << '\n';
    cout << "Compression result\n";
    cout << "  partitions            : " << r.partitions << '\n';
    cout << "  merges                : " << r.merges << '\n';
    cout << "  chain length          : " << r.global_chain.values.size() << '\n';
    cout << "  ranged nodes          : " << r.global_chain.ranges.size() << '\n';
    cout << "  original bytes        : " << r.original_bytes << '\n';
    cout << "  compressed bytes      : " << r.compressed_bytes << '\n';
    cout << "  metadata bytes        : " << r.metadata_bytes << '\n';
    cout << "  compression ratio     : " << chain_ratio << '\n';
    cout << "  space saving          : " << chain_saving << "%\n";
    cout << "  components            : " << r.component_count << '\n';
    cout << "  common segments       : " << r.global_chain.common_segments.size() << '\n';
    cout << "  verification          : " << (cfg.verify ? (r.verified ? "PASSED" : "FAILED") : "SKIPPED") << '\n';
    cout << '\n';
    cout << "Timing(sec)\n";
    cout << "  read pass1            : " << r.times.pass1_sec << '\n';
    cout << "  read pass2            : " << r.times.pass2_sec << '\n';
    cout << "  sort+unique           : " << r.times.sort_sec << '\n';
    cout << "  partition             : " << r.times.partition_sec << '\n';
    cout << "  pair counting         : " << r.times.pair_count_sec << '\n';
    cout << "  local merge           : " << r.times.local_merge_sec << '\n';
    cout << "  global merge          : " << r.times.global_merge_sec << '\n';
    cout << "  verification          : " << r.times.verify_sec << '\n';
    cout << "  total                 : " << r.times.total_sec << '\n';
}

static int run_dataset(const Config& cfg) {
    Timer total;
    PhaseTimes times;
    Graph g = read_graph_auto(cfg.input_path, times);
    Config tuned = cfg;
    auto_tune_config(g, tuned);
    CompressionResult result = compress_graph(g, tuned, times);
    if (cfg.verify) {
        result.verified = verify_compression(g, result.global_chain, result.times.verify_sec);
    }
    result.times.total_sec = total.elapsed_sec();
    save_compressed(result, tuned.output_path);
    print_summary(g, result, tuned);
    return 0;
}

#ifndef DATASET_CPP_NAME
#define DATASET_CPP_NAME "recon"
#endif

#ifndef DATASET_INPUT_PATH
#define DATASET_INPUT_PATH "data/toy/toy_graph.mtx"
#endif

#ifndef DATASET_OUTPUT_PATH
#define DATASET_OUTPUT_PATH "results/toy_graph.recon.bin"
#endif

#ifndef DATASET_VERIFY_DEFAULT
#define DATASET_VERIFY_DEFAULT 0
#endif

#ifndef GRAPH_COMPRESS_NO_MAIN
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Config cfg;
    cfg.input_path = DATASET_INPUT_PATH;
    cfg.output_path = DATASET_OUTPUT_PATH;
    cfg.verify = DATASET_VERIFY_DEFAULT != 0;

    if (argc > 1) cfg.input_path = argv[1];
    if (argc > 2) cfg.output_path = argv[2];
    if (argc > 3) cfg.partition_limit = atoi(argv[3]);
    if (argc > 4) cfg.min_common = atoi(argv[4]);
    if (argc > 5) cfg.max_postings = atoi(argv[5]);
    if (argc > 6) cfg.threads = max(1, atoi(argv[6]));
    if (argc > 7) cfg.verify = atoi(argv[7]) != 0;
    if (argc > 8) cfg.disable_residual_stitching = atoi(argv[8]) != 0;

    try {
        return run_dataset(cfg);
    } catch (const exception& ex) {
        cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
#endif
