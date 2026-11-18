#include "dominance.h"

#include <limits>
#include <utility>

namespace joyeer::ir {

namespace {

constexpr size_t unreachedBlock = std::numeric_limits<size_t>::max();

} // namespace

// Dominators by Cooper, Harvey and Kennedy's iterative algorithm over a virtual root whose successors
// are the entry and every other block without predecessors. Visiting blocks in reverse postorder
// settles the immediate dominators in a pass or two; a block the virtual root does not reach keeps no
// immediate dominator, which is the "dominated by every block" of the fixed point.
DominanceAnalysis::DominanceAnalysis(const Function& function) {
    std::vector<BlockId> ids;
    for (const auto& block : function.blocks) {
        if (indices.emplace(block.id, ids.size()).second) ids.push_back(block.id);
    }
    const size_t count = ids.size();
    const size_t root = count;
    std::vector<std::vector<size_t>> successors(count + 1);
    std::vector<std::vector<size_t>> predecessors(count + 1);
    const auto connect = [&](size_t from, size_t to) {
        successors[from].push_back(to);
        predecessors[to].push_back(from);
    };
    const auto connectTarget = [&](size_t from, BlockId target) {
        const auto found = indices.find(target);
        if (found != indices.end()) connect(from, found->second);
    };
    for (const auto& block : function.blocks) {
        if (block.instructions.empty()) continue;
        const auto from = indices.at(block.id);
        const auto& terminator = block.instructions.back();
        for (const auto target : terminator.targets) connectTarget(from, target);
        if (terminator.opcode == Opcode::switchPattern) {
            for (const auto& switchCase : terminator.switchCases) connectTarget(from, switchCase.target);
        }
    }
    for (size_t index = 0; index < count; ++index) {
        if (ids[index] == function.entry || predecessors[index].empty()) connect(root, index);
    }
    build(count, successors, predecessors);
}

DominanceAnalysis::DominanceAnalysis(
        BlockId root, const std::unordered_map<BlockId, std::unordered_set<BlockId>>& edges) {
    // Number the blocks `root` reaches, then connect them; the virtual root's only successor is `root`.
    std::vector<BlockId> ids { root };
    indices.emplace(root, 0);
    for (size_t next = 0; next < ids.size(); ++next) {
        ++steps;
        const auto found = edges.find(ids[next]);
        if (found == edges.end()) continue;
        for (const auto target : found->second) {
            ++steps;
            if (indices.emplace(target, ids.size()).second) ids.push_back(target);
        }
    }
    const size_t count = ids.size();
    std::vector<std::vector<size_t>> successors(count + 1);
    std::vector<std::vector<size_t>> predecessors(count + 1);
    for (size_t from = 0; from < count; ++from) {
        const auto found = edges.find(ids[from]);
        if (found == edges.end()) continue;
        for (const auto target : found->second) {
            const auto to = indices.at(target);
            successors[from].push_back(to);
            predecessors[to].push_back(from);
        }
    }
    successors[count].push_back(0);
    predecessors[0].push_back(count);
    build(count, successors, predecessors);
}

void DominanceAnalysis::build(
        size_t count, const std::vector<std::vector<size_t>>& successors,
        const std::vector<std::vector<size_t>>& predecessors) {
    const size_t root = count;

    // Postorder numbers from an iterative depth-first walk, which a long chain of blocks cannot overflow.
    std::vector<size_t> postorder;
    std::vector<size_t> order(count + 1, unreachedBlock);
    std::vector<bool> visited(count + 1, false);
    std::vector<std::pair<size_t, size_t>> walk {{root, 0}};
    visited[root] = true;
    while (!walk.empty()) {
        ++steps;
        const auto node = walk.back().first;
        const auto next = walk.back().second++;
        if (next < successors[node].size()) {
            const auto successor = successors[node][next];
            if (!visited[successor]) {
                visited[successor] = true;
                walk.emplace_back(successor, 0);
            }
        } else {
            order[node] = postorder.size();
            postorder.push_back(node);
            walk.pop_back();
        }
    }

    std::vector<size_t> immediate(count + 1, unreachedBlock);
    immediate[root] = root;
    const auto intersect = [&](size_t left, size_t right) {
        while (left != right) {
            while (order[left] < order[right]) {
                left = immediate[left];
                ++steps;
            }
            while (order[right] < order[left]) {
                right = immediate[right];
                ++steps;
            }
        }
        return left;
    };
    for (bool changed = true; changed;) {
        changed = false;
        // The root is last in postorder; every other block follows it in reverse postorder.
        for (size_t position = postorder.size() - 1; position-- > 0;) {
            const auto node = postorder[position];
            auto candidate = unreachedBlock;
            for (const auto predecessor : predecessors[node]) {
                ++steps;
                if (immediate[predecessor] == unreachedBlock) continue;
                candidate = candidate == unreachedBlock ? predecessor : intersect(predecessor, candidate);
            }
            if (immediate[node] != candidate) {
                immediate[node] = candidate;
                changed = true;
            }
        }
    }

    // Number the dominator tree so that a dominance query compares two intervals.
    std::vector<std::vector<size_t>> children(count + 1);
    for (const auto node : postorder) {
        if (node != root) children[immediate[node]].push_back(node);
    }
    entered.assign(count + 1, unreachedBlock);
    exited.assign(count + 1, unreachedBlock);
    size_t clock = 0;
    entered[root] = clock++;
    walk.assign({{root, 0}});
    while (!walk.empty()) {
        ++steps;
        const auto node = walk.back().first;
        const auto next = walk.back().second++;
        if (next < children[node].size()) {
            const auto child = children[node][next];
            entered[child] = clock++;
            walk.emplace_back(child, 0);
        } else {
            exited[node] = clock++;
            walk.pop_back();
        }
    }
}

bool DominanceAnalysis::dominates(BlockId dominator, BlockId block) const {
    const auto dominatorIndex = indices.find(dominator);
    const auto blockIndex = indices.find(block);
    if (dominatorIndex == indices.end() || blockIndex == indices.end()) return false;
    const auto upper = dominatorIndex->second;
    const auto lower = blockIndex->second;
    if (entered[lower] == unreachedBlock) return true;
    return entered[upper] != unreachedBlock && entered[upper] <= entered[lower] && exited[lower] <= exited[upper];
}

} // namespace joyeer::ir
