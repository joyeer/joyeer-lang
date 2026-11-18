#ifndef __joyeer_ir_dominance_h__
#define __joyeer_ir_dominance_h__

// Block dominance for the verifier: a value is usable only where its definition dominates.

#include "module.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace joyeer::ir {

// Snapshot of one function's block dominance, as the verifier checks it. Rebuild after changing the
// function's blocks or terminators. The entry block, and every other block without predecessors, roots
// the graph; a block's dominators are the maximal fixed point of Dom(b) = {b} + the intersection of
// Dom(p) over its predecessors p, with Dom(root) = {root}. A block that no root reaches (it is reached
// only around a cycle of such blocks) is therefore dominated by every block.
class DominanceAnalysis {
public:
    explicit DominanceAnalysis(const Function& function);
    // Dominance from `root` alone along `edges` (each block's successors, or its predecessors for the
    // reverse graph): a block dominates another when every path from `root` to it passes through it.
    // The blocks are those `root` reaches, itself included.
    DominanceAnalysis(BlockId root, const std::unordered_map<BlockId, std::unordered_set<BlockId>>& edges);
    // Whether `dominator` dominates `block`; a block dominates itself. False when either is not a
    // block of the function.
    [[nodiscard]] bool dominates(BlockId dominator, BlockId block) const;
    // The elementary steps the construction took, so tests can bound its growth without timing it.
    [[nodiscard]] size_t work() const {
        return steps;
    }

private:
    // The dominator tree of `count` blocks under a virtual root numbered `count`, from their edges.
    void build(size_t count, const std::vector<std::vector<size_t>>& successors,
               const std::vector<std::vector<size_t>>& predecessors);

    // Each block's dense index, and per index the entry and exit numbers of the dominator tree's
    // preorder walk; a block that no root reaches keeps the unreached sentinel.
    std::unordered_map<BlockId, size_t> indices;
    std::vector<size_t> entered;
    std::vector<size_t> exited;
    size_t steps = 0;
};

} // namespace joyeer::ir

#endif
