#include "netviz/layout/layered_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace netviz {

namespace {

struct Cancelled {};

constexpr uint32_t kCheckEvery = 4096; // items between stop_token checks

// A vertical piece of a net's trunk in a channel, on one track.
struct TrunkPiece {
    float lo, hi;
    int track = 0;
};

// Wires of one net in one channel: where horizontal wires meet its vertical
// trunk, the trunk's pieces (more than one only for doglegs) and how many
// horizontal segments end at each height (for junction dots).
struct ChannelSpan {
    float y_min = std::numeric_limits<float>::max();
    float y_max = std::numeric_limits<float>::lowest();
    std::vector<float> ys;
    std::vector<TrunkPiece> pieces; // top to bottom
    std::map<float, int> branches;  // y -> number of horizontal segments ending there

    void add(float y) {
        y_min = std::min(y_min, y);
        y_max = std::max(y_max, y);
        ys.push_back(y);
    }

    const TrunkPiece& piece_at(float y) const {
        for (const TrunkPiece& p : pieces)
            if (y <= p.hi)
                return p;
        return pieces.back();
    }
};

// One driver pin of one net, and the dummy nodes that carry its wire across
// layers: dummies[i] is in rank first_rank + 1 + i.
struct Chain {
    uint32_t net;
    uint32_t driver_pin;
    int first_rank;
    std::vector<uint32_t> dummies;
};

class LayeredLayout {
  public:
    LayeredLayout(const DisplayGraph& g, const StringPool& names, const LayoutOptions& o,
                  std::stop_token stop, LayoutProgress* progress)
        : g_(g), names_(names), o_(o), stop_(std::move(stop)), progress_(progress),
          n_(static_cast<uint32_t>(g.nodes().size())) {}

    LayoutResult run() {
        result_.nodes.assign(n_, Rect{});
        result_.pins.assign(g_.pins().size(), Vec2{});
        result_.nets.assign(g_.nets().size(), NetRoute{});

        set_stage(LayoutStage::Ranking);
        size_nodes();
        build_edges();
        break_cycles();
        assign_ranks();

        set_stage(LayoutStage::Ordering);
        add_dummies();
        order_layers();

        set_stage(LayoutStage::Placing);
        place_y();
        plan_tracks();
        place_x();

        set_stage(LayoutStage::Routing);
        route();
        compute_bounds();
        return std::move(result_);
    }

  private:
    // -- helpers --------------------------------------------------------------

    void set_stage(LayoutStage stage) {
        check_stop();
        if (progress_)
            progress_->stage.store(stage, std::memory_order_relaxed);
    }

    void check_stop() const {
        if (stop_.stop_requested())
            throw Cancelled{};
    }

    void tick(uint32_t i) const {
        if (i % kCheckEvery == 0)
            check_stop();
    }

    float text_width(NameId id) const {
        return id.valid() ? static_cast<float>(names_.str(id).size()) * o_.char_width : 0.0f;
    }

    bool is_left_port(uint32_t v) const {
        NodeKind k = g_.nodes()[v].kind;
        return k == NodeKind::InputPort || k == NodeKind::InOutPort;
    }

    bool is_output_port(uint32_t v) const {
        return v < n_ && g_.nodes()[v].kind == NodeKind::OutputPort;
    }

    uint32_t node_of_pin(uint32_t pin) const { return g_.pins()[pin].node.index; }

    float grid_ceil(float x) const { return std::ceil(x / o_.grid - 1e-3f) * o_.grid; }
    float grid_round(float x) const { return std::round(x / o_.grid) * o_.grid; }

    float center_y(uint32_t v) const { return y_[v] + height_[v] * 0.5f; }

    // Text lines centred at the top of a cell, between its pin labels: its
    // name (unless auto-generated) and type.
    uint32_t label_lines(uint32_t v) const { return g_.nodes()[v].hidden_name ? 1u : 2u; }

    // Pin position relative to the top-left of its node.
    Vec2 pin_offset(uint32_t pin_index) const {
        const DisplayPin& pin = g_.pins()[pin_index];
        uint32_t v = pin.node.index;
        float x = pin.side == PinSide::Left ? 0.0f : width_[v];
        const DisplayNode& node = g_.nodes()[v];
        if (node.kind != NodeKind::Cell && node.kind != NodeKind::Instance)
            return Vec2{x, height_[v] * 0.5f};
        return Vec2{x, (static_cast<float>(pin.order) + 0.5f) * o_.line_height};
    }

    float pin_y(uint32_t pin) const { return y_[node_of_pin(pin)] + pin_offset(pin).y; }

    // -- 1. sizes ---------------------------------------------------------------

    void size_nodes() {
        width_.assign(n_, 0.0f);
        height_.assign(n_, 0.0f);
        const float pad = o_.node_padding;
        for (uint32_t v = 0; v < n_; v++) {
            const DisplayNode& node = g_.nodes()[v];
            if (node.kind != NodeKind::Cell && node.kind != NodeKind::Instance) {
                // One pin pitch tall, so the pin (at mid-height) is on the grid.
                width_[v] = grid_ceil(text_width(node.name) + 2.0f * pad + o_.line_height * 0.6f);
                height_[v] = o_.line_height;
                continue;
            }
            float left = 0.0f, right = 0.0f;
            for (const DisplayPin& pin : g_.pins_of(node))
                (pin.side == PinSide::Left ? left : right) =
                    std::max(pin.side == PinSide::Left ? left : right, text_width(pin.name));
            // Pins start at the top; the centred labels need room for the
            // wider pin-label column on both sides.
            float name = node.hidden_name ? 0.0f : text_width(node.name);
            float label = std::max(name, text_width(node.type));
            float sides = 2.0f * (std::max(left, right) + 2.0f * pad);
            width_[v] = grid_ceil(std::max(label + sides, 5.0f * o_.char_width));
            uint32_t rows = std::max({node.left_pins, node.right_pins, label_lines(v)});
            height_[v] = static_cast<float>(rows) * o_.line_height;
        }
    }

    // -- 2. edges between real nodes --------------------------------------------

    bool unrouted(uint32_t net) const {
        return net < o_.unrouted_nets.size() && o_.unrouted_nets[net] != 0;
    }

    void build_edges() {
        uint32_t i = 0;
        for (const DisplayNet& net : g_.nets()) {
            if (unrouted(i)) {
                i++;
                continue;
            }
            tick(i++);
            for (DisplayPinId d : g_.drivers(net))
                for (DisplayPinId l : g_.loads(net)) {
                    uint32_t u = g_.pin(d).node.index, v = g_.pin(l).node.index;
                    if (u != v)
                        edges_.push_back({u, v});
                }
        }
        std::sort(edges_.begin(), edges_.end());
        edges_.erase(std::unique(edges_.begin(), edges_.end()), edges_.end());
        out_begin_.assign(n_ + 1, 0);
        for (auto [u, v] : edges_)
            out_begin_[u + 1]++;
        for (uint32_t v = 0; v < n_; v++)
            out_begin_[v + 1] += out_begin_[v];
        feedback_.assign(edges_.size(), 0);
    }

    // -- 3. cycles: SCCs, then a deterministic DFS inside each SCC ----------------

    void break_cycles() {
        // Iterative Tarjan.
        std::vector<int> index(n_, -1), low(n_, 0), comp(n_, -1);
        std::vector<char> on_stack(n_, 0);
        std::vector<uint32_t> stack;
        struct Frame {
            uint32_t v;
            uint32_t edge;
        };
        std::vector<Frame> call;
        int next_index = 0, comps = 0;
        for (uint32_t s = 0; s < n_; s++) {
            if (index[s] != -1)
                continue;
            tick(s);
            index[s] = low[s] = next_index++;
            stack.push_back(s);
            on_stack[s] = 1;
            call.push_back({s, out_begin_[s]});
            while (!call.empty()) {
                size_t top = call.size() - 1;
                uint32_t v = call[top].v;
                if (call[top].edge < out_begin_[v + 1]) {
                    uint32_t w = edges_[call[top].edge++].second;
                    if (index[w] == -1) {
                        index[w] = low[w] = next_index++;
                        stack.push_back(w);
                        on_stack[w] = 1;
                        call.push_back({w, out_begin_[w]});
                    } else if (on_stack[w]) {
                        low[v] = std::min(low[v], index[w]);
                    }
                    continue;
                }
                if (low[v] == index[v]) {
                    uint32_t w;
                    do {
                        w = stack.back();
                        stack.pop_back();
                        on_stack[w] = 0;
                        comp[w] = comps;
                    } while (w != v);
                    comps++;
                }
                call.pop_back();
                if (!call.empty()) {
                    uint32_t parent = call.back().v;
                    low[parent] = std::min(low[parent], low[v]);
                }
            }
        }

        // Inside each multi-node SCC, a DFS (starting from nodes entered from
        // outside, then by index) marks back edges as feedback.
        std::vector<std::vector<uint32_t>> members(comps);
        for (uint32_t v = 0; v < n_; v++)
            members[comp[v]].push_back(v);
        std::vector<char> entry(n_, 0);
        for (auto [u, v] : edges_)
            if (comp[u] != comp[v])
                entry[v] = 1;
        std::vector<uint8_t> state(n_, 0); // 0 new, 1 on DFS stack, 2 done
        for (const std::vector<uint32_t>& group : members) {
            if (group.size() < 2)
                continue;
            check_stop();
            std::vector<uint32_t> starts;
            for (uint32_t v : group)
                if (entry[v])
                    starts.push_back(v);
            for (uint32_t v : group)
                starts.push_back(v);
            for (uint32_t s : starts) {
                if (state[s] != 0)
                    continue;
                state[s] = 1;
                call.push_back({s, out_begin_[s]});
                while (!call.empty()) {
                    size_t top = call.size() - 1;
                    uint32_t v = call[top].v;
                    if (call[top].edge < out_begin_[v + 1]) {
                        uint32_t e = call[top].edge++;
                        uint32_t w = edges_[e].second;
                        if (comp[w] != comp[v])
                            continue;
                        if (state[w] == 1)
                            feedback_[e] = 1;
                        else if (state[w] == 0) {
                            state[w] = 1;
                            call.push_back({w, out_begin_[w]});
                        }
                        continue;
                    }
                    state[v] = 2;
                    call.pop_back();
                }
            }
        }
    }

    // -- 4. ranks: longest path over the acyclic edges ------------------------------

    void assign_ranks() {
        rank_.assign(n_, 1);
        std::vector<uint32_t> indegree(n_, 0);
        for (size_t e = 0; e < edges_.size(); e++)
            if (!feedback_[e])
                indegree[edges_[e].second]++;
        std::vector<uint32_t> queue;
        for (uint32_t v = 0; v < n_; v++) {
            if (is_left_port(v))
                rank_[v] = 0;
            if (indegree[v] == 0)
                queue.push_back(v);
        }
        for (size_t qi = 0; qi < queue.size(); qi++) {
            tick(static_cast<uint32_t>(qi));
            uint32_t u = queue[qi];
            for (uint32_t e = out_begin_[u]; e < out_begin_[u + 1]; e++) {
                if (feedback_[e])
                    continue;
                uint32_t v = edges_[e].second;
                if (!is_left_port(v))
                    rank_[v] = std::max(rank_[v], rank_[u] + 1);
                if (--indegree[v] == 0)
                    queue.push_back(v);
            }
        }
        int last = 0;
        for (uint32_t v = 0; v < n_; v++)
            if (!is_output_port(v))
                last = std::max(last, rank_[v]);
        for (uint32_t v = 0; v < n_; v++)
            if (is_output_port(v))
                rank_[v] = last + 1;
        max_rank_ = 0;
        for (int r : rank_)
            max_rank_ = std::max(max_rank_, r);
    }

    // -- 5. dummy nodes for wires that cross layers ---------------------------------

    uint32_t add_node(int rank) {
        rank_.push_back(rank);
        width_.push_back(0.0f);
        height_.push_back(0.0f); // a point: its wire runs at its y, on the grid
        return static_cast<uint32_t>(rank_.size() - 1);
    }

    void link(uint32_t a, uint32_t b) {
        succs_[a].push_back(b);
        preds_[b].push_back(a);
    }

    void add_dummies() {
        for (uint32_t net = 0; net < g_.nets().size(); net++) {
            tick(net);
            if (unrouted(net))
                continue;
            const DisplayNet& dn = g_.nets()[net];
            for (DisplayPinId d : g_.drivers(dn)) {
                Chain chain{net, d.index, rank_[node_of_pin(d.index)], {}};
                int far = chain.first_rank + 1;
                for (DisplayPinId l : g_.loads(dn))
                    far = std::max(far, rank_[node_of_pin(l.index)]);
                for (int r = chain.first_rank + 1; r < far; r++)
                    chain.dummies.push_back(add_node(r));
                chains_.push_back(std::move(chain));
            }
        }
        size_t total = rank_.size();
        preds_.assign(total, {});
        succs_.assign(total, {});
        for (const Chain& chain : chains_) {
            uint32_t u = node_of_pin(chain.driver_pin);
            uint32_t prev = u;
            for (uint32_t dummy : chain.dummies) {
                link(prev, dummy);
                prev = dummy;
            }
            for (DisplayPinId l : g_.loads(g_.nets()[chain.net])) {
                uint32_t v = node_of_pin(l.index);
                int rv = rank_[v];
                if (v == u || rv <= chain.first_rank)
                    continue; // same node or feedback: not an ordering constraint
                link(rv == chain.first_rank + 1 ? u : chain.dummies[rv - chain.first_rank - 2], v);
            }
        }
        for (auto& list : preds_) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        for (auto& list : succs_) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
    }

    // -- 6. order within layers: DFS order, then barycenter sweeps ------------------

    bool fixed_layer(int r) const {
        // Layers holding only ports keep the module's port order.
        for (uint32_t v : layers_[r])
            if (v >= n_ ||
                (g_.nodes()[v].kind == NodeKind::Cell || g_.nodes()[v].kind == NodeKind::Instance))
                return false;
        return true;
    }

    void order_layers() {
        size_t total = rank_.size();
        layers_.assign(max_rank_ + 1, {});
        // Discovery order of a DFS from the left ports, then everything else.
        std::vector<uint32_t> discovered(total, UINT32_MAX);
        uint32_t counter = 0;
        std::vector<uint32_t> stack;
        auto dfs = [&](uint32_t s) {
            if (discovered[s] != UINT32_MAX)
                return;
            stack.push_back(s);
            while (!stack.empty()) {
                uint32_t v = stack.back();
                stack.pop_back();
                if (discovered[v] != UINT32_MAX)
                    continue;
                discovered[v] = counter++;
                for (size_t i = succs_[v].size(); i-- > 0;)
                    if (discovered[succs_[v][i]] == UINT32_MAX)
                        stack.push_back(succs_[v][i]);
            }
        };
        for (uint32_t v = 0; v < n_; v++)
            if (is_left_port(v))
                dfs(v);
        for (uint32_t v = 0; v < total; v++)
            dfs(v);
        for (uint32_t v = 0; v < total; v++)
            layers_[rank_[v]].push_back(v);
        for (auto& layer : layers_)
            std::stable_sort(layer.begin(), layer.end(),
                             [&](uint32_t a, uint32_t b) { return discovered[a] < discovered[b]; });
        std::vector<char> fixed(layers_.size());
        for (int r = 0; r <= max_rank_; r++) {
            fixed[r] = fixed_layer(r);
            if (fixed[r])
                std::sort(layers_[r].begin(), layers_[r].end());
        }

        std::vector<float> pos(total, 0.0f);
        auto update_positions = [&](int r) {
            for (size_t i = 0; i < layers_[r].size(); i++)
                pos[layers_[r][i]] = static_cast<float>(i);
        };
        for (int r = 0; r <= max_rank_; r++)
            update_positions(r);

        auto sweep = [&](int r, const std::vector<std::vector<uint32_t>>& neighbours) {
            if (fixed[r])
                return;
            check_stop();
            std::vector<std::pair<float, uint32_t>> keyed;
            keyed.reserve(layers_[r].size());
            for (uint32_t v : layers_[r]) {
                const auto& ns = neighbours[v];
                float key = pos[v];
                if (!ns.empty()) {
                    float sum = 0.0f;
                    for (uint32_t u : ns)
                        sum += pos[u];
                    key = sum / static_cast<float>(ns.size());
                }
                keyed.push_back({key, v});
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keyed.size(); i++)
                layers_[r][i] = keyed[i].second;
            update_positions(r);
        };
        for (int pass = 0; pass < 4; pass++) {
            for (int r = 1; r <= max_rank_; r++)
                sweep(r, preds_);
            for (int r = max_rank_ - 1; r >= 0; r--)
                sweep(r, succs_);
        }
    }

    // -- 7. vertical placement ----------------------------------------------------

    float gap_between(uint32_t a, uint32_t b) const {
        return (a >= n_ || b >= n_) ? o_.track_pitch * 2.0f : o_.node_gap;
    }

    // Places a layer's nodes as close to their desired tops as possible
    // (least squares) while keeping their order and the gaps between them.
    // With s_i = the minimum distance from the first node's top to node i's
    // top, the constraint is that z_i = y_i - s_i never decreases; the best
    // such z is the isotonic regression of (desired_i - s_i), computed with
    // pool-adjacent-violators. Overlapping groups are centred on what they
    // want, so positions do not drift in one direction. Results are snapped to
    // the grid; rounding keeps z non-decreasing, and the offsets are grid
    // multiples, so the gaps are kept.
    void settle(const std::vector<uint32_t>& layer, const std::vector<float>& desired) {
        size_t count = layer.size();
        std::vector<float> offset(count, 0.0f); // s_i
        for (size_t i = 1; i < count; i++)
            offset[i] = offset[i - 1] + height_[layer[i - 1]] + gap_between(layer[i - 1], layer[i]);

        struct Block {
            float sum; // of targets in the block
            size_t size;
            size_t end; // one past the last index
            float value() const { return sum / static_cast<float>(size); }
        };
        std::vector<Block> blocks;
        for (size_t i = 0; i < count; i++) {
            blocks.push_back({desired[i] - offset[i], 1, i + 1});
            while (blocks.size() > 1 && blocks[blocks.size() - 2].value() > blocks.back().value()) {
                Block last = blocks.back();
                blocks.pop_back();
                blocks.back().sum += last.sum;
                blocks.back().size += last.size;
                blocks.back().end = last.end;
            }
        }
        size_t i = 0;
        for (const Block& block : blocks)
            for (; i < block.end; i++)
                y_[layer[i]] = grid_round(block.value()) + offset[i];
    }

    // Moves a layer's nodes toward the median centre of their neighbours in
    // the adjacent layer (the median ignores outliers such as a clock net).
    void align(int r, const std::vector<std::vector<uint32_t>>& neighbours) {
        const auto& layer = layers_[r];
        std::vector<float> desired(layer.size());
        std::vector<float> centers;
        for (size_t i = 0; i < layer.size(); i++) {
            uint32_t v = layer[i];
            const auto& ns = neighbours[v];
            if (ns.empty()) {
                desired[i] = y_[v];
                continue;
            }
            centers.clear();
            for (uint32_t u : ns)
                centers.push_back(center_y(u));
            std::sort(centers.begin(), centers.end());
            size_t mid = centers.size() / 2;
            float median =
                centers.size() % 2 ? centers[mid] : (centers[mid - 1] + centers[mid]) * 0.5f;
            desired[i] = median - height_[v] * 0.5f;
        }
        settle(layer, desired);
    }

    void place_y() {
        y_.assign(rank_.size(), 0.0f);
        for (const auto& layer : layers_) {
            std::vector<float> desired(layer.size(), 0.0f);
            settle(layer, desired);
        }
        // Alternate directions a few times so the layers settle together.
        for (int round = 0; round < 4; round++) {
            check_stop();
            for (int r = 1; r <= max_rank_; r++)
                align(r, preds_);
            for (int r = max_rank_ - 1; r >= 0; r--)
                align(r, succs_);
        }
        float top = std::numeric_limits<float>::max();
        for (float y : y_)
            top = std::min(top, y);
        if (!y_.empty())
            for (float& y : y_)
                y -= top;
    }

    // -- 8. tracks (which vertical wires go where), then horizontal placement --------

    ChannelSpan& span(uint32_t net, int channel) { return spans_[{net, channel}]; }

    // A horizontal wire of `net` meeting its vertical trunk in `channel` at
    // height y, coming from the layer on the left of the channel or going to
    // the layer on its right. Recorded for the track constraints.
    void from_left(uint32_t net, int channel, float y) {
        span(net, channel).add(y);
        left_at_[{channel, y}].push_back(net);
    }
    void to_right(uint32_t net, int channel, float y) {
        span(net, channel).add(y);
        right_at_[{channel, y}].push_back(net);
    }

    // Records, per net and channel, the vertical extent its wires need, by
    // walking each connection the same way route() draws it; then packs those
    // spans into tracks.
    void plan_tracks() {
        float bottom = 0.0f;
        for (uint32_t v = 0; v < n_; v++)
            bottom = std::max(bottom, y_[v] + height_[v]);
        lane_base_ = bottom + o_.node_gap;
        lane_of_.assign(g_.nets().size(), -1);
        int lanes = 0;

        for (const Chain& chain : chains_) {
            float py = pin_y(chain.driver_pin);
            int ru = chain.first_rank;
            const DisplayNet& net = g_.nets()[chain.net];
            // Forward: along the chain of dummies.
            int last = ru; // last channel used
            for (DisplayPinId l : g_.loads(net)) {
                int rv = rank_[node_of_pin(l.index)];
                if (rv > ru)
                    last = std::max(last, rv - 1);
            }
            for (int k = ru; k <= last && has_forward(chain); k++) {
                float entry = k == ru ? py
                                      : y_[chain.dummies[k - ru - 1]] +
                                            height_[chain.dummies[k - ru - 1]] * 0.5f;
                from_left(chain.net, k, entry);
                if (k + 1 <= last)
                    to_right(chain.net, k, dummy_y(chain, k + 1));
                for (DisplayPinId l : g_.loads(net))
                    if (rank_[node_of_pin(l.index)] == k + 1)
                        to_right(chain.net, k, pin_y(l.index));
            }
            // Feedback: down to a lane below the drawing and back.
            for (DisplayPinId l : g_.loads(net)) {
                uint32_t v = node_of_pin(l.index);
                int rv = rank_[v];
                if (rv > ru || rv == 0 || v == node_of_pin(chain.driver_pin))
                    continue;
                if (lane_of_[chain.net] < 0)
                    lane_of_[chain.net] = lanes++;
                float lane = lane_y(chain.net);
                // Lanes are below every node and one per net, so they
                // never share a height with another net's wire.
                from_left(chain.net, ru, py);
                span(chain.net, ru).add(lane);
                span(chain.net, rv - 1).add(lane);
                to_right(chain.net, rv - 1, pin_y(l.index));
            }
        }

        // Pack spans into tracks per channel.
        channel_tracks_.assign(std::max(max_rank_, 0) + 1, 0);
        std::map<int, std::vector<uint32_t>> by_channel; // channel -> nets
        for (auto& [key, s] : spans_)
            by_channel[key.second].push_back(key.first);
        for (auto& [channel, nets] : by_channel) {
            check_stop();
            int tracks = pack_channel(channel, nets);
            if (channel >= 0 && channel < static_cast<int>(channel_tracks_.size()))
                channel_tracks_[channel] = tracks;
        }
    }

    // Assigns the nets of one channel to tracks (left to right), with the
    // constrained left-edge algorithm of channel routing:
    //
    // - Two trunks may share a track when they are a track pitch apart.
    // - Where net A comes from the left at height y and net B goes to the right
    //   at the same y, A's trunk must be left of B's, or their horizontal wires
    //   would overlap. A track only takes trunks whose such "left" trunks
    //   already have tracks.
    // - These constraints can form cycles (two nets that cross). Nets on a
    //   cycle get doglegs: their trunk is split between consecutive wire
    //   heights, at a free grid height, into pieces on separate tracks joined
    //   by a short horizontal jog. Each piece then holds wires of one height
    //   only, so it has only incoming or only outgoing constraints and no
    //   cycle remains (unless there is no free height to split at; then the
    //   constraint is dropped and the wires may overlap).
    //
    // Returns the number of tracks.
    int pack_channel(int channel, const std::vector<uint32_t>& nets) {
        std::map<uint32_t, size_t> index; // net -> position in nets
        for (size_t i = 0; i < nets.size(); i++)
            index[nets[i]] = i;
        std::set<float> used; // heights of horizontal wires in this channel
        for (uint32_t net : nets)
            for (float y : spans_.at({net, channel}).ys)
                used.insert(y);

        // Constraints at shared heights: (y, left net, right net).
        std::vector<std::tuple<float, uint32_t, uint32_t>> conflicts;
        auto first = left_at_.lower_bound({channel, std::numeric_limits<float>::lowest()});
        for (auto it = first; it != left_at_.end() && it->first.first == channel; ++it) {
            auto right = right_at_.find(it->first);
            if (right == right_at_.end())
                continue;
            // Several nets per side when they share a multi-bit pin.
            for (uint32_t a : it->second)
                for (uint32_t b : right->second)
                    if (a != b)
                        conflicts.push_back({it->first.second, a, b});
        }

        // Nets on constraint cycles: those left over by both a forward and a
        // backward topological sort (this also includes nets on paths between
        // cycles, which only costs them a dogleg).
        std::vector<std::vector<size_t>> out(nets.size()), in(nets.size());
        for (auto [y, a, b] : conflicts) {
            out[index.at(a)].push_back(index.at(b));
            in[index.at(b)].push_back(index.at(a));
        }
        auto leftover = [&](const std::vector<std::vector<size_t>>& fwd,
                            const std::vector<std::vector<size_t>>& back) {
            std::vector<size_t> degree(nets.size()), queue;
            for (size_t i = 0; i < nets.size(); i++)
                if ((degree[i] = back[i].size()) == 0)
                    queue.push_back(i);
            std::vector<char> left(nets.size(), 1);
            while (!queue.empty()) {
                size_t i = queue.back();
                queue.pop_back();
                left[i] = 0;
                for (size_t j : fwd[i])
                    if (--degree[j] == 0)
                        queue.push_back(j);
            }
            return left;
        };
        std::vector<char> ahead = leftover(out, in), behind = leftover(in, out);

        // Trunk pieces.
        for (size_t i = 0; i < nets.size(); i++) {
            ChannelSpan& s = spans_.at({nets[i], channel});
            s.pieces.clear();
            if (!(ahead[i] && behind[i])) {
                s.pieces.push_back({s.y_min, s.y_max});
                continue;
            }
            std::vector<float> ys = s.ys;
            std::sort(ys.begin(), ys.end());
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            float lo = ys[0];
            for (size_t k = 0; k + 1 < ys.size(); k++) {
                std::optional<float> split = free_height(ys[k], ys[k + 1], used);
                if (!split)
                    continue;
                used.insert(*split);
                s.pieces.push_back({lo, *split});
                lo = *split;
            }
            s.pieces.push_back({lo, ys.back()});
        }

        // Pieces in top-to-bottom order, and the constraints between them.
        struct Item {
            float lo;
            uint32_t net;
            size_t piece;
            TrunkPiece* trunk;
        };
        std::vector<Item> items;
        std::vector<ChannelSpan*> span_of(nets.size());
        for (size_t i = 0; i < nets.size(); i++) {
            ChannelSpan& s = spans_.at({nets[i], channel});
            span_of[i] = &s;
            for (size_t k = 0; k < s.pieces.size(); k++)
                items.push_back({s.pieces[k].lo, nets[i], k, &s.pieces[k]});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return std::tie(a.lo, a.net, a.piece) < std::tie(b.lo, b.net, b.piece);
        });
        std::map<std::pair<uint32_t, size_t>, size_t> item_of; // (net, piece) -> item
        for (size_t i = 0; i < items.size(); i++)
            item_of[{items[i].net, items[i].piece}] = i;
        auto item_at = [&](uint32_t net, float y) {
            const ChannelSpan& s = *span_of[index.at(net)];
            size_t k = static_cast<size_t>(&s.piece_at(y) - s.pieces.data());
            return item_of.at({net, k});
        };
        std::vector<std::vector<size_t>> after(items.size());
        std::vector<int> waiting(items.size(), 0); // unplaced items that must be left of it
        for (auto [y, a, b] : conflicts) {
            size_t ia = item_at(a, y), ib = item_at(b, y);
            after[ia].push_back(ib);
            waiting[ib]++;
        }

        std::vector<float> lows(items.size());
        for (size_t i = 0; i < items.size(); i++)
            lows[i] = items[i].lo;
        std::vector<char> placed(items.size(), 0);
        // Unplaced items with no waiting constraints. Items are numbered top
        // to bottom, so the set is ordered by height.
        std::set<size_t> ready;
        for (size_t i = 0; i < items.size(); i++)
            if (waiting[i] == 0)
                ready.insert(i);
        size_t done = 0;
        int track = 0;
        for (; done < items.size(); track++) {
            if (ready.empty()) { // a cycle left: release the topmost unplaced item
                for (size_t i = 0; i < items.size(); i++)
                    if (!placed[i]) {
                        waiting[i] = 0;
                        ready.insert(i);
                        break;
                    }
            }
            // Fill this track top to bottom: repeatedly take the topmost ready
            // piece starting a track pitch below the previous one.
            std::vector<size_t> released; // ready from the next track on
            size_t from = 0;
            for (auto it = ready.lower_bound(from); it != ready.end();
                 it = ready.lower_bound(from)) {
                size_t i = *it;
                ready.erase(it);
                TrunkPiece& piece = *items[i].trunk;
                piece.track = track;
                placed[i] = 1;
                done++;
                for (size_t j : after[i])
                    if (!placed[j] && --waiting[j] == 0)
                        released.push_back(j);
                float limit = piece.hi + o_.track_pitch;
                from = static_cast<size_t>(std::upper_bound(lows.begin(), lows.end(), limit) -
                                           lows.begin());
            }
            ready.insert(released.begin(), released.end());
        }
        return track;
    }

    // A grid height strictly between a and b with no wire on it, nearest the
    // middle.
    std::optional<float> free_height(float a, float b, const std::set<float>& used) const {
        long first = std::lround(std::floor(a / o_.grid)) + 1;
        long last = std::lround(std::ceil(b / o_.grid)) - 1;
        std::optional<float> best;
        float mid = (a + b) * 0.5f;
        for (long k = first; k <= last; k++) {
            float y = static_cast<float>(k) * o_.grid;
            if (!used.contains(y) && (!best || std::abs(y - mid) < std::abs(*best - mid)))
                best = y;
        }
        return best;
    }

    bool has_forward(const Chain& chain) const {
        for (DisplayPinId l : g_.loads(g_.nets()[chain.net]))
            if (rank_[node_of_pin(l.index)] > chain.first_rank)
                return true;
        return false;
    }

    float dummy_y(const Chain& chain, int rank) const {
        uint32_t d = chain.dummies[rank - chain.first_rank - 1];
        return y_[d] + height_[d] * 0.5f;
    }

    float lane_y(uint32_t net) const {
        return lane_base_ + static_cast<float>(lane_of_[net]) * o_.track_pitch;
    }

    void place_x() {
        int layers = max_rank_ + 1;
        layer_w_.assign(layers, 0.0f);
        layer_x_.assign(layers, 0.0f);
        channel_x_.assign(layers, 0.0f);
        channel_w_.assign(layers, 0.0f);
        for (uint32_t v = 0; v < n_; v++)
            layer_w_[rank_[v]] = std::max(layer_w_[rank_[v]], width_[v]);
        float x = 0.0f;
        for (int r = 0; r < layers; r++) {
            layer_x_[r] = x;
            channel_x_[r] = x + layer_w_[r];
            int tracks = r < static_cast<int>(channel_tracks_.size()) ? channel_tracks_[r] : 0;
            channel_w_[r] =
                o_.channel_gap + static_cast<float>(std::max(tracks - 1, 0)) * o_.track_pitch;
            x = channel_x_[r] + channel_w_[r];
        }
        for (uint32_t v = 0; v < n_; v++) {
            int r = rank_[v];
            float free = layer_w_[r] - width_[v];
            NodeKind k = g_.nodes()[v].kind;
            // Input ports hug the channel on their right, output ports the one on their left.
            float offset = (k == NodeKind::InputPort || k == NodeKind::InOutPort) ? free
                           : k == NodeKind::OutputPort ? 0.0f
                                                       : grid_round(free * 0.5f);
            result_.nodes[v] = Rect{Vec2{layer_x_[r] + offset, y_[v]},
                                    Vec2{layer_x_[r] + offset + width_[v], y_[v] + height_[v]}};
        }
        for (uint32_t p = 0; p < g_.pins().size(); p++) {
            const Rect& box = result_.nodes[node_of_pin(p)];
            Vec2 off = pin_offset(p);
            result_.pins[p] = Vec2{box.min.x + off.x, box.min.y + off.y};
        }
    }

    float track_x(int channel, int track) const {
        int tracks = channel_tracks_[channel];
        float used = static_cast<float>(std::max(tracks - 1, 0)) * o_.track_pitch;
        float start = channel_x_[channel] + (channel_w_[channel] - used) * 0.5f;
        return start + static_cast<float>(track) * o_.track_pitch;
    }

    // x of the trunk piece of `net` that a horizontal wire at height y meets.
    float track_x(uint32_t net, int channel, float y) const {
        return track_x(channel, spans_.at({net, channel}).piece_at(y).track);
    }

    // -- 9. routing ---------------------------------------------------------------

    void horizontal(uint32_t net, int channel, float x0, float x1, float y) {
        if (x0 != x1)
            result_.nets[net].segments.push_back({Vec2{x0, y}, Vec2{x1, y}});
        span(net, channel).branches[y]++;
    }

    void route() {
        uint32_t i = 0;
        for (const Chain& chain : chains_) {
            tick(i++);
            uint32_t net = chain.net;
            const DisplayNet& dn = g_.nets()[net];
            Vec2 p = result_.pins[chain.driver_pin];
            int ru = chain.first_rank;
            int last = ru;
            for (DisplayPinId l : g_.loads(dn))
                if (rank_[node_of_pin(l.index)] > ru)
                    last = std::max(last, rank_[node_of_pin(l.index)] - 1);

            bool entry_drawn = false; // driver pin -> its first track
            if (has_forward(chain)) {
                entry_drawn = true;
                float from_x = p.x;
                for (int k = ru; k <= last; k++) {
                    float entry = k == ru ? p.y : dummy_y(chain, k);
                    horizontal(net, k, from_x, track_x(net, k, entry), entry);
                    for (DisplayPinId l : g_.loads(dn)) {
                        if (rank_[node_of_pin(l.index)] != k + 1)
                            continue;
                        Vec2 q = result_.pins[l.index];
                        horizontal(net, k, track_x(net, k, q.y), q.x, q.y);
                    }
                    if (k < last)
                        from_x = track_x(net, k, dummy_y(chain, k + 1));
                }
            }
            for (DisplayPinId l : g_.loads(dn)) {
                uint32_t v = node_of_pin(l.index);
                int rv = rank_[v];
                if (rv > ru || rv == 0 || v == node_of_pin(chain.driver_pin))
                    continue;
                Vec2 q = result_.pins[l.index];
                float lane = lane_y(net);
                float out_x = track_x(net, ru, lane);
                float in_x = track_x(net, rv - 1, lane);
                if (!entry_drawn) {
                    horizontal(net, ru, p.x, track_x(net, ru, p.y), p.y);
                    entry_drawn = true;
                }
                horizontal(net, ru, out_x, in_x, lane);
                span(net, rv - 1).branches[lane]++;
                horizontal(net, rv - 1, track_x(net, rv - 1, q.y), q.x, q.y);
            }
        }

        // Vertical trunks (with dogleg jogs between their pieces) and junction
        // dots, per net and channel.
        for (auto& [key, s] : spans_) {
            auto [net, channel] = key;
            NetRoute& route = result_.nets[net];
            for (size_t k = 0; k < s.pieces.size(); k++) {
                const TrunkPiece& piece = s.pieces[k];
                float tx = track_x(channel, piece.track);
                if (piece.hi > piece.lo)
                    route.segments.push_back({Vec2{tx, piece.lo}, Vec2{tx, piece.hi}});
                if (k > 0) {
                    float prev = track_x(channel, s.pieces[k - 1].track);
                    route.segments.push_back(
                        {Vec2{std::min(prev, tx), piece.lo}, Vec2{std::max(prev, tx), piece.lo}});
                }
                // Jog ends are corners, never junctions; only wire heights count.
                for (auto it = s.branches.lower_bound(piece.lo);
                     it != s.branches.end() && it->first <= piece.hi; ++it) {
                    float y = it->first;
                    int incident = it->second + (y > piece.lo ? 1 : 0) + (y < piece.hi ? 1 : 0);
                    if (incident >= 3)
                        route.junctions.push_back(Vec2{tx, y});
                }
            }
        }
        for (NetRoute& route : result_.nets) {
            auto key = [](const Segment& s) { return std::make_tuple(s.a.x, s.a.y, s.b.x, s.b.y); };
            std::sort(route.segments.begin(), route.segments.end(),
                      [&](const Segment& a, const Segment& b) { return key(a) < key(b); });
            route.segments.erase(
                std::unique(route.segments.begin(), route.segments.end(),
                            [&](const Segment& a, const Segment& b) { return key(a) == key(b); }),
                route.segments.end());
        }
    }

    void compute_bounds() {
        Rect b{Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}};
        bool first = true;
        auto add = [&](Vec2 p) {
            if (first) {
                b = Rect{p, p};
                first = false;
            }
            b.min.x = std::min(b.min.x, p.x);
            b.min.y = std::min(b.min.y, p.y);
            b.max.x = std::max(b.max.x, p.x);
            b.max.y = std::max(b.max.y, p.y);
        };
        for (const Rect& r : result_.nodes) {
            add(r.min);
            add(r.max);
        }
        for (const NetRoute& route : result_.nets)
            for (const Segment& s : route.segments) {
                add(s.a);
                add(s.b);
            }
        result_.bounds = b;
    }

    const DisplayGraph& g_;
    const StringPool& names_;
    const LayoutOptions& o_;
    std::stop_token stop_;
    LayoutProgress* progress_;
    uint32_t n_; // real nodes; dummies are numbered from n_

    std::vector<std::pair<uint32_t, uint32_t>> edges_; // u -> v between real nodes, sorted
    std::vector<uint32_t> out_begin_; // edges of u: [out_begin_[u], out_begin_[u+1])
    std::vector<char> feedback_;      // per edge: ignored when ranking

    std::vector<int> rank_; // per layout node (real and dummy)
    std::vector<float> width_, height_, y_;
    std::vector<std::vector<uint32_t>> preds_, succs_; // between adjacent ranks
    std::vector<std::vector<uint32_t>> layers_;
    int max_rank_ = 0;
    std::vector<Chain> chains_;

    std::map<std::pair<uint32_t, int>, ChannelSpan> spans_; // (net, channel)
    // (channel, y) -> nets whose horizontal wires meet their trunks there from
    // the left / towards the right (see from_left()).
    std::map<std::pair<int, float>, std::vector<uint32_t>> left_at_, right_at_;
    std::vector<int> channel_tracks_;
    std::vector<int> lane_of_; // per net: feedback lane, or -1
    float lane_base_ = 0.0f;
    std::vector<float> layer_x_, layer_w_, channel_x_, channel_w_;

    LayoutResult result_;
};

} // namespace

LayoutResult layered_layout(const DisplayGraph& graph, const StringPool& names,
                            const LayoutOptions& options, std::stop_token stop,
                            LayoutProgress* progress) {
    if (graph.nodes().size() > options.max_nodes) {
        LayoutResult result;
        result.status = LayoutResult::Status::TooLarge;
        return result;
    }
    try {
        return LayeredLayout(graph, names, options, std::move(stop), progress).run();
    } catch (const Cancelled&) {
        LayoutResult result;
        result.status = LayoutResult::Status::Cancelled;
        return result;
    }
}

} // namespace netviz
