// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Constant folding
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// CONST TRANSFORMATIONS:
//   Call on one node for PARAM values, or netlist for overall constant folding:
//      Bottom up traversal:
//          Attempt to convert operands to constants
//          If operands are constant, replace this node with constant.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "config_build.h"
#include "verilatedos.h"

#include "V3Const.h"

#include "V3Ast.h"
#include "V3Global.h"
#include "V3Simulate.h"
#include "V3Stats.h"
#include "V3String.h"
#include "V3UniqueNames.h"
#include "V3Width.h"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

#define TREE_SKIP_VISIT(...)
#define TREEOP1(...)
#define TREEOPA(...)
#define TREEOP(...)
#define TREEOPS(...)
#define TREEOPC(...)
#define TREEOPV(...)

//######################################################################
// Utilities

static bool isConst(const AstNode* nodep, uint64_t v) {
    const AstConst* const constp = VN_CAST(nodep, Const);
    return constp && constp->toUQuad() == v;
}

template <typename T>
static typename std::enable_if<std::is_integral<T>::value, bool>::type isPow2(T val) {
    return (val & (val - 1)) == 0;
}

static int countTrailingZeroes(uint64_t val) {
    UASSERT(val, "countTrailingZeroes argument must be non-zero");
#if defined(__GNUC__) && !defined(VL_NO_BUILTINS)
    return __builtin_ctzll(val);
#else
    int bit = 0;
    val = ~val;
    while (val & 1) {
        ++bit;
        val >>= 1;
    }
    return bit;
#endif
}

// This visitor can be used in the post-expanded Ast from V3Expand, where the Ast satisfies:
// - Constants are 64 bit at most (because words are accessed via AstWordSel)
// - Variables are scoped.
class ConstBitOpTreeVisitor final : public VNVisitorConst {
    // NODE STATE
    // AstVarRef::user4u      -> Base index of m_varInfos that points VarInfo
    // AstVarScope::user4u    -> Same as AstVarRef::user4
    const VNUser4InUse m_inuser4;

    // TYPES

    // Holds a node to be added as a term in the reduction tree, it's equivalent op count, and a
    // bool indicating if the term is clean (0/1 value, or if the top bits might be dirty)
    using ResultTerm = std::tuple<AstNodeExpr*, unsigned, bool>;

    class LeafInfo final {  // Leaf node (either AstConst or AstVarRef)
        // MEMBERS
        bool m_polarity = true;
        int m_lsb = 0;  // LSB of actually used bit of m_refp->varp()
        int m_msb = 0;  // MSB of actually used bit of m_refp->varp()
        int m_wordIdx = -1;  // -1 means AstWordSel is not used.
        AstVarRef* m_refp = nullptr;
        const AstConst* m_constp = nullptr;

    public:
        // CONSTRUCTORS
        LeafInfo() = default;
        LeafInfo(const LeafInfo& other) = default;
        explicit LeafInfo(int lsb)
            : m_lsb{lsb} {}

        // METHODS
        void setLeaf(AstVarRef* refp) {
            UASSERT_OBJ(!m_refp && !m_constp, refp, "Must be called just once");
            m_refp = refp;
            m_msb = refp->varp()->widthMin() - 1;
        }
        void setLeaf(const AstConst* constp) {
            UASSERT_OBJ(!m_refp && !m_constp, constp, "Must be called just once");
            m_constp = constp;
            m_msb = constp->widthMin() - 1;
        }
        // updateBitRange(), limitBitRangeToLsb(), and polarity() must be called during ascending
        // back to the root.
        void updateBitRange(int newLsb, int newMsb) {
            if ((m_lsb <= m_msb && newLsb > newMsb) || (m_lsb > m_msb && m_lsb < newLsb)) {
                // When the new bit range is out of m_refp, clear polarity because nodes below is
                // shifted out to zero.
                // This kind of clear may happen several times. e.g. (!(1'b1 >> 1)) >> 1
                polarity(true);
            }
            m_lsb = newLsb;
            m_msb = newMsb;
        }
        void updateBitRange(const AstCCast* castp) {
            updateBitRange(m_lsb, std::min(m_msb, m_lsb + castp->width() - 1));
        }
        void updateBitRange(const AstShiftR* shiftp) {
            updateBitRange(m_lsb + VN_AS(shiftp->rhsp(), Const)->toUInt(), m_msb);
        }
        void limitBitRangeToLsb() { updateBitRange(m_lsb, std::min(m_msb, m_lsb)); }
        int wordIdx() const { return m_wordIdx; }
        void wordIdx(int i) { m_wordIdx = i; }
        bool polarity() const { return m_polarity; }
        void polarity(bool p) { m_polarity = p; }

        AstVarRef* refp() const { return m_refp; }
        const AstConst* constp() const { return m_constp; }
        bool missingWordSel() const {
            // When V3Expand is skipped, WordSel is not inserted.
            return m_refp->isWide() && m_wordIdx == -1;
        }
        int lsb() const { return m_lsb; }

        int msb() const { return std::min(m_msb, varWidth() - 1); }
        int varWidth() const {
            UASSERT(m_refp, "m_refp should be set");
            const int width = m_refp->varp()->widthMin();
            if (!m_refp->isWide()) {
                UASSERT_OBJ(m_wordIdx == -1, m_refp, "Bad word index into non-wide");
                return width;
            } else {
                if (missingWordSel()) return width;
                UASSERT_OBJ(m_wordIdx >= 0, m_refp, "Bad word index into wide");
                const int bitsInMSW = VL_BITBIT_E(width) ? VL_BITBIT_E(width) : VL_EDATASIZE;
                return m_wordIdx == m_refp->widthWords() - 1 ? bitsInMSW : VL_EDATASIZE;
            }
        }
    };

    struct BitPolarityEntry final {  // Found bit polarity during iterate()
        LeafInfo m_info;
        bool m_polarity;
        int m_bit;
        BitPolarityEntry(const LeafInfo& info, bool pol, int bit)
            : m_info{info}
            , m_polarity{pol}
            , m_bit{bit} {}
        BitPolarityEntry() = default;
    };

    struct FrozenNodeInfo final {  // Context when a frozen node is found
        bool m_polarity;
        int m_lsb;
        bool operator<(const FrozenNodeInfo& other) const {
            if (m_lsb != other.m_lsb) return m_lsb < other.m_lsb;
            return m_polarity < other.m_polarity;
        }
    };

    class Restorer final {  // Restore the original state unless disableRestore() is called
        ConstBitOpTreeVisitor& m_visitor;
        const size_t m_polaritiesSize;
        const size_t m_frozenSize;
        const unsigned m_ops;
        const bool m_polarity;
        bool m_restore;

    public:
        explicit Restorer(ConstBitOpTreeVisitor& visitor)
            : m_visitor{visitor}
            , m_polaritiesSize{visitor.m_bitPolarities.size()}
            , m_frozenSize{visitor.m_frozenNodes.size()}
            , m_ops{visitor.m_ops}
            , m_polarity{visitor.m_polarity}
            , m_restore{true} {}
        ~Restorer() {
            UASSERT(m_visitor.m_bitPolarities.size() >= m_polaritiesSize,
                    "m_bitPolarities must grow monotonically");
            UASSERT(m_visitor.m_frozenNodes.size() >= m_frozenSize,
                    "m_frozenNodes must grow monotonically");
            if (m_restore) restoreNow();
        }
        void disableRestore() { m_restore = false; }
        void restoreNow() {
            UASSERT(m_restore, "Can be called just once");
            m_visitor.m_bitPolarities.resize(m_polaritiesSize);
            m_visitor.m_frozenNodes.resize(m_frozenSize);
            m_visitor.m_ops = m_ops;
            m_visitor.m_polarity = m_polarity;
            m_restore = false;
        }
    };
    // Collect information for each Variable to transform as below
    class VarInfo final {
        // MEMBERS
        int m_knownResult = -1;  // -1: result is not known, 0 or 1: result of this tree
        const ConstBitOpTreeVisitor* const
            m_parentp;  // ConstBitOpTreeVisitor holding this VarInfo
        AstVarRef* const m_refp;  // Points the variable that this VarInfo covers
        const int m_width;  // Width of term this VarInfo refers to
        V3Number m_bitPolarity;  // Coefficient of each bit

    public:
        // METHODS
        bool hasConstResult() const { return m_knownResult >= 0 || m_bitPolarity.isAllX(); }
        // The constant result. Only valid if hasConstResult() returned true.
        bool getConstResult() const {
            // Note that this condition covers m_knownResult == -1 but m_bitPolarity.isAllX(),
            // in which case the result is 0
            return m_knownResult == 1;
        }
        const AstVarRef* refp() const { return m_refp; }
        bool sameVarAs(const AstNodeVarRef* otherp) const { return m_refp->sameNode(otherp); }
        void setPolarity(bool compBit, int bit) {
            // Ignore if already determined a known reduction
            if (m_knownResult >= 0) return;
            UASSERT_OBJ(bit < m_width, m_refp,
                        "Bit index out of range: " << bit << " width: " << m_width);
            if (m_bitPolarity.bitIsX(bit)) {  // The bit is not yet marked with either polarity
                m_bitPolarity.setBit(bit, compBit);
            } else {  // The bit has already been marked with some polarity
                const bool sameFlag = m_bitPolarity.bitIs1(bit) == compBit;
                if (m_parentp->isXorTree()) {
                    UASSERT_OBJ(compBit && sameFlag, m_refp, "Only true is set in Xor tree");
                    // a ^ a ^ b == b so we can ignore a
                    m_bitPolarity.setBit(bit, 'x');
                } else {  // And, Or
                    // Can ignore this nodep as the bit is already marked with the same polarity
                    if (sameFlag) return;  // a & a == a, b | b == b
                    // Otherwise result is constant (a & ~a == 0) or (a | ~a == 1)
                    m_knownResult = m_parentp->isAndTree() ? 0 : 1;
                    m_bitPolarity.setAllBitsX();  // The variable is not referred anymore
                }
            }
        }

        // Return reduction term for this VarInfo, together with the number of ops in the term,
        // and a boolean indicating if the term is clean (1-bit vs multi-bit value)
        ResultTerm getResultTerm() const {
            UASSERT_OBJ(!hasConstResult(), m_refp, "getTerm on reduction that yields constant");
            FileLine* const fl = m_refp->fileline();

            // Get the term we are referencing (the WordSel, if wide, otherwise just the VarRef)
            AstNodeExpr* srcp = VN_CAST(m_refp->backp(), WordSel);
            if (!srcp) srcp = m_refp;
            srcp = srcp->cloneTree(false);

            // Signed variables might have redundant sign bits that need masking.
            const bool hasRedundantSignBits
                = m_refp->varp()->dtypep()->isSigned()
                  && (m_refp->isWide() ? (m_width != VL_EDATASIZE)
                                       : (m_width < 8 || !isPow2(m_width)));

            // Get the mask that selects the bits that are relevant in this term
            V3Number maskNum{srcp, m_width, 0};
            maskNum.opBitsNonX(m_bitPolarity);  // 'x' -> 0, 0->1, 1->1
            const uint64_t maskVal = maskNum.toUQuad();
            UASSERT_OBJ(maskVal != 0, m_refp,
                        "Should have been recognized as having const 0 result");

            // Parts of the return value
            AstNodeExpr* resultp = srcp;  // The tree for this term
            unsigned ops = 0;  // Number of ops in this term
            bool clean = false;  // Whether the term is clean (has value 0 or 1)

            if (isPow2(maskVal)) {
                // If we only want a single bit, shift it out instead of a masked compare. Shifts
                // don't go through the flags register on x86 and are hence faster. This is also
                // always fewer or same ops as mask and compare, but with shorter instructions on
                // x86.

                // Find the index of the bit we want.
                const int bit = countTrailingZeroes(maskVal);
                // If we want something other than the bottom bit, shift it out
                if (bit != 0) {
                    resultp = new AstShiftR{fl, resultp,
                                            new AstConst{fl, static_cast<uint32_t>(bit)}, m_width};
                    ++ops;
                }
                // Negate it if necessary
                const bool negate = m_bitPolarity.bitIs0(bit);
                if (negate) {
                    resultp = new AstNot{fl, resultp};
                    ++ops;
                }
                // Clean if MSB of unsigned value, and not negated
                clean = (bit == m_width - 1) && !hasRedundantSignBits && !negate;
            } else {
                // We want multiple bits. Go ahead and extract them.

                // Check if masking is required, and if so apply it
                const bool needsMasking = maskVal != VL_MASK_Q(m_width) || hasRedundantSignBits;
                if (needsMasking) {
                    resultp = new AstAnd{fl, new AstConst{fl, maskNum}, resultp};
                    ++ops;
                }

                // Create the sub-expression for this term
                if (m_parentp->isXorTree()) {
                    if (needsMasking) {
                        // Reduce the masked term to the minimum known width,
                        // to use the smallest RedXor formula
                        const int widthMin = maskNum.widthToFit();
                        resultp->dtypeChgWidth(widthMin, widthMin);
                    }
                    resultp = new AstRedXor{fl, resultp};
                    ++ops;
                    clean = false;
                    // VL_REDXOR_* returns IData, set width accordingly to avoid unnecessary casts
                    resultp->dtypeChgWidth(VL_IDATASIZE, 1);
                } else if (m_parentp->isAndTree()) {
                    V3Number compNum{srcp, m_width, 0};
                    compNum.opBitsOne(m_bitPolarity);  // 'x'->0, 0->0, 1->1
                    resultp = new AstEq{fl, new AstConst{fl, compNum}, resultp};
                    ++ops;
                    clean = true;
                } else {  // Or
                    V3Number compNum{srcp, m_width, 0};
                    compNum.opBitsOne(m_bitPolarity);  // 'x'->0, 0->0, 1->1
                    compNum.opXor(V3Number{compNum}, maskNum);
                    resultp = new AstNeq{fl, new AstConst{fl, compNum}, resultp};
                    ++ops;
                    clean = true;
                }
            }

            return ResultTerm{resultp, ops, clean};
        }

        // CONSTRUCTORS
        VarInfo(ConstBitOpTreeVisitor* parent, AstVarRef* refp, int width)
            : m_parentp{parent}
            , m_refp{refp}
            , m_width{width}
            , m_bitPolarity{refp, m_width} {
            m_bitPolarity.setAllBitsX();
        }
    };

    // MEMBERS
    bool m_failed = false;
    bool m_polarity = true;  // Flip when AstNot comes
    unsigned m_ops;  // Number of operations such as And, Or, Xor, Sel...
    int m_lsb = 0;  // Current LSB
    LeafInfo* m_leafp = nullptr;  // AstConst or AstVarRef that currently looking for
    const AstNodeExpr* const m_rootp;  // Root of this AST subtree

    std::vector<std::pair<AstNodeExpr*, FrozenNodeInfo>>
        m_frozenNodes;  // Nodes that cannot be optimized
    std::vector<BitPolarityEntry> m_bitPolarities;  // Polarity of bits found during iterate()
    std::vector<std::unique_ptr<VarInfo>> m_varInfos;  // VarInfo for each variable, [0] is nullptr

    // METHODS

    bool isAndTree() const { return VN_IS(m_rootp, And); }
    bool isOrTree() const { return VN_IS(m_rootp, Or); }
    bool isXorTree() const { return VN_IS(m_rootp, Xor) || VN_IS(m_rootp, RedXor); }

#define CONST_BITOP_RETURN_IF(cond, nodep) \
    if (setFailed(cond, #cond, nodep, __LINE__)) return

#define CONST_BITOP_SET_FAILED(reason, nodep) setFailed(true, reason, nodep, __LINE__)

    bool setFailed(bool fail, const char* reason, AstNode* nodep, int line) {
        if (fail && !m_failed) {
            UINFO(9, "cannot optimize " << m_rootp << " reason:" << reason << " called from line:"
                                        << line << " when checking:" << nodep);
            // if (debug() >= 9) m_rootp->dumpTree("-  root: ");
            m_failed = true;
        }
        return m_failed;
    }
    void incrOps(const AstNode* nodep, int line) {
        ++m_ops;
        UINFO(9, "Increment to " << m_ops << " " << nodep << " called from line " << line);
    }
    VarInfo& getVarInfo(const LeafInfo& ref) {
        UASSERT_OBJ(ref.refp(), m_rootp, "null varref in And/Or/Xor optimization");
        AstNode* nodep = ref.refp()->varScopep();
        if (!nodep) nodep = ref.refp()->varp();  // Not scoped
        int baseIdx = nodep->user4();
        if (baseIdx == 0) {  // Not set yet
            baseIdx = m_varInfos.size();
            const int numWords
                = ref.refp()->dtypep()->isWide() ? ref.refp()->dtypep()->widthWords() : 1;
            m_varInfos.resize(m_varInfos.size() + numWords);
            nodep->user4(baseIdx);
        }
        const size_t idx = baseIdx + std::max(0, ref.wordIdx());
        VarInfo* varInfop = m_varInfos[idx].get();
        if (!varInfop) {
            varInfop = new VarInfo{this, ref.refp(), ref.varWidth()};
            m_varInfos[idx].reset(varInfop);
            if (ref.missingWordSel()) {
                // ConstBitOpTreeVisitor makes some constants for masks and its type is uint64_t.
                // That's why V3Expand, that inserts WordSel, is needed.
                CONST_BITOP_SET_FAILED("V3Expand is skipped", ref.refp());
            }
        } else {
            if (!varInfop->sameVarAs(ref.refp()))
                CONST_BITOP_SET_FAILED("different var (scope?)", ref.refp());
        }
        return *varInfop;
    }

    // Traverse down to see AstConst or AstVarRef
    LeafInfo findLeaf(AstNode* nodep, bool expectConst) {
        LeafInfo info{m_lsb};
        {
            VL_RESTORER(m_leafp);
            m_leafp = &info;
            iterateConst(nodep);
        }

        bool ok = !m_failed;
        if (expectConst) {
            ok &= !info.refp() && info.constp();
        } else {
            ok &= info.refp() && !info.constp();
        }
        return ok ? info : LeafInfo{};
    }

    // VISITORS
    void visit(AstNode* nodep) override { CONST_BITOP_SET_FAILED("Hit unexpected op", nodep); }
    void visit(AstCCast* nodep) override {
        iterateChildrenConst(nodep);
        if (m_leafp) m_leafp->updateBitRange(nodep);
    }
    void visit(AstShiftR* nodep) override {
        CONST_BITOP_RETURN_IF(!m_leafp, nodep);
        AstConst* const constp = VN_CAST(nodep->rhsp(), Const);
        CONST_BITOP_RETURN_IF(!constp, nodep->rhsp());
        m_lsb += constp->toUInt();
        incrOps(nodep, __LINE__);
        iterateConst(nodep->lhsp());
        m_leafp->updateBitRange(nodep);
        m_lsb -= constp->toUInt();
    }
    void visit(AstNot* nodep) override {
        CONST_BITOP_RETURN_IF(nodep->widthMin() != 1, nodep);
        AstNode* lhsp = nodep->lhsp();
        AstCCast* const castp = VN_CAST(lhsp, CCast);
        if (castp) lhsp = castp->lhsp();
        CONST_BITOP_RETURN_IF(!isXorTree() && !VN_IS(lhsp, VarRef) && !VN_IS(lhsp, ShiftR), lhsp);
        incrOps(nodep, __LINE__);
        m_polarity = !m_polarity;
        iterateChildrenConst(nodep);
        // Don't restore m_polarity for Xor as it counts parity of the entire tree
        if (!isXorTree()) m_polarity = !m_polarity;
        if (m_leafp && castp) m_leafp->updateBitRange(castp);
        if (m_leafp) m_leafp->polarity(!m_leafp->polarity());
    }
    void visit(AstWordSel* nodep) override {
        CONST_BITOP_RETURN_IF(!m_leafp, nodep);
        AstConst* const constp = VN_CAST(nodep->bitp(), Const);
        CONST_BITOP_RETURN_IF(!constp, nodep->bitp());
        UASSERT_OBJ(m_leafp->wordIdx() == -1, nodep, "Unexpected nested WordSel");
        m_leafp->wordIdx(constp->toSInt());
        iterateConst(nodep->fromp());
    }
    void visit(AstVarRef* nodep) override {
        CONST_BITOP_RETURN_IF(!m_leafp, nodep);
        m_leafp->setLeaf(nodep);
    }
    void visit(AstConst* nodep) override {
        CONST_BITOP_RETURN_IF(!m_leafp, nodep);
        m_leafp->setLeaf(nodep);
    }

    void visit(AstRedXor* nodep) override {
        Restorer restorer{*this};
        CONST_BITOP_RETURN_IF(!VN_IS(m_rootp, Xor), nodep);
        AstNode* lhsp = nodep->lhsp();
        const AstCCast* const castp = VN_CAST(lhsp, CCast);
        if (castp) lhsp = castp->lhsp();
        if (const AstAnd* const andp = VN_CAST(lhsp, And)) {  // '^(mask & leaf)'
            CONST_BITOP_RETURN_IF(!andp, lhsp);

            const LeafInfo& mask = findLeaf(andp->lhsp(), true);
            CONST_BITOP_RETURN_IF(!mask.constp() || mask.lsb() != 0, andp->lhsp());

            LeafInfo ref = findLeaf(andp->rhsp(), false);
            CONST_BITOP_RETURN_IF(!ref.refp(), andp->rhsp());
            if (castp) ref.updateBitRange(castp);

            restorer.disableRestore();  // Now all subtree succeeded

            const V3Number& maskNum = mask.constp()->num();

            incrOps(nodep, __LINE__);
            incrOps(andp, __LINE__);

            // Mark all bits checked in this reduction
            const int maxBitIdx = std::min(ref.lsb() + maskNum.width(), ref.msb() + 1);
            for (int bitIdx = ref.lsb(); bitIdx < maxBitIdx; ++bitIdx) {
                const int maskIdx = bitIdx - ref.lsb();
                if (maskNum.bitIs0(maskIdx)) continue;
                // Set true, m_polarity takes care of the entire parity
                m_bitPolarities.emplace_back(ref, true, bitIdx);
            }
        } else {  // '^leaf'
            LeafInfo ref = findLeaf(lhsp, false);
            CONST_BITOP_RETURN_IF(!ref.refp(), lhsp);
            if (castp) ref.updateBitRange(castp);

            restorer.disableRestore();  // Now all checks passed

            incrOps(nodep, __LINE__);

            // Mark all bits checked by this comparison
            for (int bitIdx = ref.lsb(); bitIdx <= ref.msb(); ++bitIdx) {
                m_bitPolarities.emplace_back(ref, true, bitIdx);
            }
        }
    }

    void visit(AstNodeBiop* nodep) override {
        if (VN_IS(nodep, And) && isConst(nodep->lhsp(), 1)) {  // 1 & _
            // Always reach past a plain making AND
            Restorer restorer{*this};
            incrOps(nodep, __LINE__);
            iterateConst(nodep->rhsp());
            if (m_leafp) m_leafp->limitBitRangeToLsb();
            CONST_BITOP_RETURN_IF(m_failed, nodep->rhsp());
            restorer.disableRestore();  // Now all checks passed
        } else if (nodep->type() == m_rootp->type()) {  // And, Or, Xor
            // subtree under NOT can be optimized only in XOR tree.
            CONST_BITOP_RETURN_IF(!m_polarity && !isXorTree(), nodep);
            incrOps(nodep, __LINE__);
            for (const bool right : {false, true}) {
                VL_RESTORER(m_leafp);
                Restorer restorer{*this};
                LeafInfo leafInfo{m_lsb};
                // cppcheck-suppress danglingLifetime
                m_leafp = &leafInfo;
                AstNodeExpr* opp = right ? nodep->rhsp() : nodep->lhsp();
                const bool origFailed = m_failed;
                iterateConst(opp);
                if (leafInfo.constp() || m_failed) {
                    // Revert changes in leaf
                    restorer.restoreNow();
                    // Reach past a cast then add to frozen nodes to be added to final reduction
                    if (const AstCCast* const castp = VN_CAST(opp, CCast)) opp = castp->lhsp();
                    const bool pol = isXorTree() || m_polarity;  // Only AND/OR tree needs polarity
                    UASSERT(pol, "AND/OR tree expects m_polarity==true");
                    m_frozenNodes.emplace_back(opp, FrozenNodeInfo{pol, m_lsb});
                    m_failed = origFailed;
                    continue;
                }
                restorer.disableRestore();  // Now all checks passed
                if (leafInfo.refp()) {
                    // The conditional on the lsb being in range is necessary for some degenerate
                    // case, e.g.: (IData)((QData)wide[0] >> 32), or <1-bit-var> >> 1, which is
                    // just zero
                    if (leafInfo.lsb() <= leafInfo.msb()) {
                        m_bitPolarities.emplace_back(leafInfo, isXorTree() || leafInfo.polarity(),
                                                     leafInfo.lsb());
                    } else if ((isAndTree() && leafInfo.polarity())
                               || (isOrTree() && !leafInfo.polarity())) {
                        // If there is a constant 0 term in an And tree or 1 term in an Or tree, we
                        // must include it. Fudge this by adding a bit with both polarities, which
                        // will simplify to zero or one respectively.
                        // Note that Xor tree does not need this kind of care, polarity of Xor tree
                        // is already cared when visitin AstNot. Taking xor with 1'b0 is nop.
                        m_bitPolarities.emplace_back(leafInfo, true, 0);
                        m_bitPolarities.emplace_back(leafInfo, false, 0);
                    }
                }
            }
        } else if ((isAndTree() && VN_IS(nodep, Eq)) || (isOrTree() && VN_IS(nodep, Neq))) {
            Restorer restorer{*this};
            CONST_BITOP_RETURN_IF(!m_polarity, nodep);
            CONST_BITOP_RETURN_IF(m_lsb, nodep);  // the result of EQ/NE is 1 bit width
            const AstNode* lhsp = nodep->lhsp();
            if (const AstCCast* const castp = VN_CAST(lhsp, CCast)) lhsp = castp->lhsp();
            const AstConst* const constp = VN_CAST(lhsp, Const);
            CONST_BITOP_RETURN_IF(!constp, nodep->lhsp());

            const V3Number& compNum = constp->num();

            auto setPolarities = [this, &compNum](const LeafInfo& ref, const V3Number* maskp) {
                const bool maskFlip = isAndTree() ^ ref.polarity();
                int constantWidth = compNum.width();
                if (maskp) constantWidth = std::max(constantWidth, maskp->width());
                const int maxBitIdx = std::max(ref.lsb() + constantWidth, ref.msb() + 1);
                // Mark all bits checked by this comparison
                for (int bitIdx = ref.lsb(); bitIdx < maxBitIdx; ++bitIdx) {
                    const int maskIdx = bitIdx - ref.lsb();
                    const bool mask0 = maskp && maskp->bitIs0(maskIdx);
                    const bool outOfRange = bitIdx > ref.msb();
                    if (mask0 || outOfRange) {  // RHS is 0
                        if (compNum.bitIs1(maskIdx)) {
                            // LHS is 1
                            // And tree: 1 == 0 => always false, set v && !v
                            // Or tree : 1 != 0 => always true, set v || !v
                            m_bitPolarities.emplace_back(ref, true, 0);
                            m_bitPolarities.emplace_back(ref, false, 0);
                            break;
                        } else {  // This bitIdx is irrelevant
                            continue;
                        }
                    }
                    const bool polarity = compNum.bitIs1(maskIdx) != maskFlip;
                    m_bitPolarities.emplace_back(ref, polarity, bitIdx);
                }
            };

            if (const AstAnd* const andp = VN_CAST(nodep->rhsp(), And)) {  // comp == (mask & v)
                const LeafInfo& mask = findLeaf(andp->lhsp(), true);
                CONST_BITOP_RETURN_IF(!mask.constp() || mask.lsb() != 0, andp->lhsp());

                const LeafInfo& ref = findLeaf(andp->rhsp(), false);
                CONST_BITOP_RETURN_IF(!ref.refp(), andp->rhsp());

                restorer.disableRestore();  // Now all checks passed

                const V3Number& maskNum = mask.constp()->num();

                incrOps(nodep, __LINE__);
                incrOps(andp, __LINE__);

                setPolarities(ref, &maskNum);
            } else {  // comp == v
                const LeafInfo& ref = findLeaf(nodep->rhsp(), false);
                CONST_BITOP_RETURN_IF(!ref.refp(), nodep->rhsp());

                restorer.disableRestore();  // Now all checks passed

                incrOps(nodep, __LINE__);

                setPolarities(ref, nullptr);
            }
        } else {
            CONST_BITOP_SET_FAILED("Mixture of different ops cannot be optimized", nodep);
        }
    }

    // CONSTRUCTORS
    ConstBitOpTreeVisitor(AstNodeExpr* nodep, unsigned externalOps)
        : m_ops{externalOps}
        , m_rootp{nodep} {
        // Fill nullptr at [0] because AstVarScope::user4 is 0 by default
        m_varInfos.push_back(nullptr);
        CONST_BITOP_RETURN_IF(!isAndTree() && !isOrTree() && !isXorTree(), nodep);
        if (AstNodeBiop* const biopp = VN_CAST(nodep, NodeBiop)) {
            iterateConst(biopp);
        } else {
            UASSERT_OBJ(VN_IS(nodep, RedXor), nodep, "Must be RedXor");
            incrOps(nodep, __LINE__);
            iterateChildrenConst(nodep);
        }
        for (auto&& entry : m_bitPolarities) {
            getVarInfo(entry.m_info).setPolarity(entry.m_polarity, entry.m_bit);
        }
        UASSERT_OBJ(isXorTree() || m_polarity, nodep, "must be the original polarity");
    }
    virtual ~ConstBitOpTreeVisitor() = default;
#undef CONST_BITOP_RETURN_IF
#undef CONST_BITOP_SET_FAILED

public:
    // Transform as below.
    // v[0] & v[1] => 2'b11 == (2'b11 & v)
    // v[0] | v[1] => 2'b00 != (2'b11 & v)
    // v[0] ^ v[1] => ^{2'b11 & v}
    // (3'b011 == (3'b011 & v)) & v[2]  => 3'b111 == (3'b111 & v)
    // (3'b000 != (3'b011 & v)) | v[2]  => 3'b000 != (3'b111 & v)
    // Reduction ops are transformed in the same way.
    // &{v[0], v[1]} => 2'b11 == (2'b11 & v)
    static AstNodeExpr* simplify(AstNodeExpr* nodep, int resultWidth, unsigned externalOps,
                                 VDouble0& reduction) {
        UASSERT_OBJ(1 <= resultWidth && resultWidth <= 64, nodep, "resultWidth out of range");

        // Walk tree, gathering all terms referenced in expression
        const ConstBitOpTreeVisitor visitor{nodep, externalOps};

        // If failed on root node is not optimizable, or there are no variable terms, then done
        if (visitor.m_failed || visitor.m_varInfos.size() == 1) return nullptr;

        // FileLine used for constructing all new nodes in this function
        FileLine* const fl = nodep->fileline();

        // Get partial result each term referenced, count total number of ops and keep track of
        // whether we have clean/dirty terms. visitor.m_varInfos appears in deterministic order,
        // so the optimized tree is deterministic as well.

        std::vector<AstNodeExpr*> termps;
        termps.reserve(visitor.m_varInfos.size() - 1);
        unsigned resultOps = 0;
        bool hasCleanTerm = false;
        bool hasDirtyTerm = false;

        for (auto&& v : visitor.m_varInfos) {
            if (!v) continue;  // Skip nullptr at m_varInfos[0]
            if (v->hasConstResult()) {
                // If a constant term is known, we can either drop it or the whole tree is constant
                AstNodeExpr* resultp = nullptr;
                if (v->getConstResult()) {
                    UASSERT_OBJ(visitor.isOrTree(), nodep,
                                "Only OR tree can yield known 1 result");
                    UINFO(9, "OR tree with const 1 term: " << v->refp());
                    // Known 1 bit in OR tree, whole result is 1
                    resultp = new AstConst{fl, AstConst::BitTrue{}};
                } else if (visitor.isAndTree()) {
                    UINFO(9, "AND tree with const 0 term: " << v->refp());
                    // Known 0 bit in AND tree, whole result is 0
                    resultp = new AstConst{fl, AstConst::BitFalse{}};
                } else {
                    // Known 0 bit in OR or XOR tree. Ignore it.
                    continue;
                }
                // Set width and widthMin precisely
                resultp->dtypeChgWidth(resultWidth, 1);
                for (AstNode* const termp : termps) VL_DO_DANGLING(termp->deleteTree(), termp);
                return resultp;
            }
            const ResultTerm result = v->getResultTerm();
            termps.push_back(std::get<0>(result));
            resultOps += std::get<1>(result);
            if (std::get<2>(result)) {
                hasCleanTerm = true;
                UINFO(9, "Clean term: " << termps.back());
            } else {
                hasDirtyTerm = true;
                UINFO(9, "Dirty term: " << termps.back());
            }
        }

        // Group by FrozenNodeInfo
        std::map<FrozenNodeInfo, std::vector<AstNodeExpr*>> frozenNodes;
        // Check if frozen terms are clean or not
        for (const auto& frozenInfo : visitor.m_frozenNodes) {
            AstNodeExpr* const termp = frozenInfo.first;
            // Comparison operators are clean
            if ((VN_IS(termp, Eq) || VN_IS(termp, Neq) || VN_IS(termp, Lt) || VN_IS(termp, Lte)
                 || VN_IS(termp, Gt) || VN_IS(termp, Gte))
                && frozenInfo.second.m_lsb == 0) {
                hasCleanTerm = true;
            } else {
                // Otherwise, conservatively assume the frozen term is dirty
                hasDirtyTerm = true;
                UINFO(9, "Dirty frozen term: " << termp);
            }
            frozenNodes[frozenInfo.second].push_back(termp);
        }

        // Figure out if a final negation is required
        const bool needsFlip = visitor.isXorTree() && !visitor.m_polarity;

        // Figure out if the final tree needs cleaning
        const bool needsCleaning = visitor.isAndTree() ? !hasCleanTerm : hasDirtyTerm;

        // Add size of reduction tree to op count
        resultOps += termps.size() - 1;
        for (const auto& lsbAndNodes : frozenNodes) {
            if (lsbAndNodes.first.m_lsb > 0) ++resultOps;  // Needs AstShiftR
            if (!lsbAndNodes.first.m_polarity) ++resultOps;  // Needs AstNot
            resultOps += lsbAndNodes.second.size();
        }
        // Add final polarity flip in Xor tree
        if (needsFlip) ++resultOps;
        // Add final cleaning AND
        if (needsCleaning) ++resultOps;

        if (debug() >= 9) {  // LCOV_EXCL_START
            cout << "-  Bitop tree considered:\n";
            for (AstNodeExpr* const termp : termps) termp->dumpTree("-  Reduced term: ");
            for (const std::pair<AstNodeExpr*, FrozenNodeInfo>& termp : visitor.m_frozenNodes) {
                termp.first->dumpTree("-  Frozen term with lsb "
                                      + std::to_string(termp.second.m_lsb) + " polarity "
                                      + std::to_string(termp.second.m_polarity) + ": ");
            }
            cout << "-  Needs flipping: " << needsFlip << "\n";
            cout << "-  Needs cleaning: " << needsCleaning << "\n";
            cout << "-  Size: " << resultOps << " input size: " << visitor.m_ops << "\n";
        }  // LCOV_EXCL_END

        // Sometimes we have no terms left after ignoring redundant terms
        // (all of which were zeroes)
        if (termps.empty() && visitor.m_frozenNodes.empty()) {
            reduction += visitor.m_ops;
            AstNodeExpr* const resultp = needsFlip ? new AstConst{fl, AstConst::BitTrue{}}
                                                   : new AstConst{fl, AstConst::BitFalse{}};
            resultp->dtypeChgWidth(resultWidth, 1);
            return resultp;
        }

        // Only substitute the result if beneficial as determined by operation count
        if (visitor.m_ops <= resultOps) {
            for (AstNode* const termp : termps) VL_DO_DANGLING(termp->deleteTree(), termp);
            return nullptr;
        }

        // Update statistics
        reduction += visitor.m_ops - resultOps;

        // Reduction op to combine terms
        const auto reduce = [&visitor, fl](AstNodeExpr* lhsp, AstNodeExpr* rhsp) -> AstNodeExpr* {
            if (!lhsp) return rhsp;
            if (visitor.isAndTree()) {
                return new AstAnd{fl, lhsp, rhsp};
            } else if (visitor.isOrTree()) {
                return new AstOr{fl, lhsp, rhsp};
            } else {
                return new AstXor{fl, lhsp, rhsp};
            }
        };

        // Compute result by reducing all terms
        AstNodeExpr* resultp = nullptr;
        for (AstNodeExpr* const termp : termps) {  //
            resultp = reduce(resultp, termp);
        }
        // Add any frozen terms to the reduction
        for (auto&& nodes : frozenNodes) {
            // nodes.second has same lsb and polarity
            AstNodeExpr* termp = nullptr;
            for (AstNodeExpr* const itemp : nodes.second) {
                termp = reduce(termp, itemp->unlinkFrBack());
            }
            if (nodes.first.m_lsb > 0) {  // LSB is not 0, so shiftR
                AstNodeDType* const dtypep = termp->dtypep();
                termp = new AstShiftR{termp->fileline(), termp,
                                      new AstConst(termp->fileline(), AstConst::WidthedValue{},
                                                   termp->width(), nodes.first.m_lsb)};
                termp->dtypep(dtypep);
            }
            if (!nodes.first.m_polarity) {  // Polarity is inverted, so append Not
                AstNodeDType* const dtypep = termp->dtypep();
                termp = new AstNot{termp->fileline(), termp};
                termp->dtypep(dtypep);
            }
            resultp = reduce(resultp, termp);
        }

        // Set width of masks to expected result width. This is required to prevent later removal
        // of the masking node e.g. by the "AND with all ones" rule. If the result width happens
        // to be 1, we still need to ensure the AstAnd is not dropped, so use a wider mask in this
        // special case.
        const int maskWidth
            = std::max(resultp->width(), resultWidth == 1 ? VL_IDATASIZE : resultWidth);

        // Apply final polarity flip
        if (needsFlip) {
            if (needsCleaning) {
                // Cleaning will be added below. Use a NOT which is a byte shorter on x86
                resultp = new AstNot{fl, resultp};
            } else {
                // Keep result clean by using XOR(1, _)
                AstConst* const maskp = new AstConst{fl, AstConst::WidthedValue{}, maskWidth, 1};
                resultp = new AstXor{fl, maskp, resultp};
            }
        }

        // Apply final cleaning
        if (needsCleaning) {
            AstConst* const maskp = new AstConst{fl, AstConst::WidthedValue{}, maskWidth, 1};
            resultp = new AstAnd{fl, maskp, resultp};
        }

        // Cast back to original size if required
        if (resultp->width() != resultWidth) {
            resultp = new AstCCast{fl, resultp, resultWidth, 1};
        }

        // Set width and widthMin precisely
        resultp->dtypeChgWidth(resultWidth, 1);

        return resultp;
    }
};

//######################################################################
// Const state, as a visitor of each AstNode

class ConstVisitor final : public VNVisitor {
    // CONSTANTS
    static constexpr unsigned CONCAT_MERGABLE_MAX_DEPTH = 10;  // Limit alg recursion

    // NODE STATE
    // ** only when m_warn/m_doExpensive is set.  If state is needed other times,
    // ** must track down everywhere V3Const is called and make sure no overlaps.
    // AstVar::user4p           -> Used by variable marking/finding
    // AstEnum::user4           -> bool.  Recursing.

    // STATE
    static constexpr bool m_doShort = true;  // Remove expressions that short circuit
    bool m_params = false;  // If true, propagate parameterized and true numbers only
    bool m_required = false;  // If true, must become a constant
    bool m_wremove = true;  // Inside scope, no assignw removal
    bool m_warn = false;  // Output warnings
    bool m_doExpensive = false;  // Enable computationally expensive optimizations
    bool m_doCpp = false;  // Enable late-stage C++ optimizations
    bool m_doNConst = false;  // Enable non-constant-child simplifications
    bool m_doV = false;  // Verilog, not C++ conversion
    bool m_doGenerate = false;  // Postpone width checking inside generate
    bool m_convertLogicToBit = false;  // Convert logical operators to bitwise
    bool m_hasJumpDelay = false;  // JumpGo or Delay under this while
    bool m_underRecFunc = false;  // Under a recursive function
    AstNodeModule* m_modp = nullptr;  // Current module
    const AstArraySel* m_selp = nullptr;  // Current select
    const AstNode* m_scopep = nullptr;  // Current scope
    const AstAttrOf* m_attrp = nullptr;  // Current attribute
    VDouble0 m_statBitOpReduction;  // Ops reduced in ConstBitOpTreeVisitor
    const bool m_globalPass;  // ConstVisitor invoked as a global pass
    static uint32_t s_globalPassNum;  // Counts number of times ConstVisitor invoked as global pass
    V3UniqueNames m_concswapNames;  // For generating unique temporary variable names
    std::map<const AstNode*, bool> m_containsMemberAccess;  // Caches results of matchBiopToBitwise
    std::unordered_set<AstJumpBlock*> m_usedJumpBlocks;  // JumpBlocks used by some JumpGo

    // METHODS

    V3Number constNumV(AstNode* nodep) {
        // Contract C width to V width (if needed, else just direct copy)
        // The upper zeros in the C representation can otherwise cause
        // wrong results in some operations, e.g. MulS
        const V3Number& numc = VN_AS(nodep, Const)->num();
        return !numc.isNumber() ? numc : V3Number{nodep, nodep->widthMinV(), numc};
    }
    V3Number toNumC(AstNode* nodep, V3Number& numv) {
        // Extend V width back to C width for given node
        return !numv.isNumber() ? numv : V3Number{nodep, nodep->width(), numv};
    }

    bool operandConst(AstNode* nodep) { return VN_IS(nodep, Const); }
    bool operandAsvConst(const AstNode* nodep) {
        // BIASV(CONST, BIASV(CONST,...)) -> BIASV( BIASV_CONSTED(a,b), ...)
        const AstNodeBiComAsv* const bnodep = VN_CAST(nodep, NodeBiComAsv);
        if (!bnodep) return false;
        if (!VN_IS(bnodep->lhsp(), Const)) return false;
        const AstNodeBiComAsv* const rnodep = VN_CAST(bnodep->rhsp(), NodeBiComAsv);
        if (!rnodep) return false;
        if (rnodep->type() != bnodep->type()) return false;
        if (rnodep->width() != bnodep->width()) return false;
        if (rnodep->lhsp()->width() != bnodep->lhsp()->width()) return false;
        if (!VN_IS(rnodep->lhsp(), Const)) return false;
        return true;
    }
    bool operandAsvSame(const AstNode* nodep) {
        // BIASV(SAMEa, BIASV(SAMEb,...)) -> BIASV( BIASV(SAMEa,SAMEb), ...)
        const AstNodeBiComAsv* const bnodep = VN_CAST(nodep, NodeBiComAsv);
        if (!bnodep) return false;
        const AstNodeBiComAsv* const rnodep = VN_CAST(bnodep->rhsp(), NodeBiComAsv);
        if (!rnodep) return false;
        if (rnodep->type() != bnodep->type()) return false;
        if (rnodep->width() != bnodep->width()) return false;
        return operandsSame(bnodep->lhsp(), rnodep->lhsp());
    }
    bool operandAsvLUp(const AstNode* nodep) {
        // BIASV(BIASV(CONSTll,lr),r) -> BIASV(CONSTll,BIASV(lr,r)) ?
        //
        // Example of how this is useful:
        // BIASV(BIASV(CONSTa,b...),BIASV(CONSTc,d...))  // hits operandAsvUp
        // BIASV(CONSTa,BIASV(b...,BIASV(CONSTc,d...)))  // hits operandAsvUp
        // BIASV(CONSTa,BIASV(CONSTc,BIASV(c...,d...)))  // hits operandAsvConst
        // BIASV(BIASV(CONSTa,CONSTc),BIASV(c...,d...))) // hits normal constant propagation
        // BIASV(CONST_a_c,BIASV(c...,d...)))
        //
        // Idea for the future: All BiComAsvs could be lists, sorted by if they're constant
        const AstNodeBiComAsv* const bnodep = VN_CAST(nodep, NodeBiComAsv);
        if (!bnodep) return false;
        const AstNodeBiComAsv* const lnodep = VN_CAST(bnodep->lhsp(), NodeBiComAsv);
        if (!lnodep) return false;
        if (lnodep->type() != bnodep->type()) return false;
        if (lnodep->width() != bnodep->width()) return false;
        return VN_IS(lnodep->lhsp(), Const);
    }
    bool operandAsvRUp(const AstNode* nodep) {
        // BIASV(l,BIASV(CONSTrl,rr)) -> BIASV(CONSTrl,BIASV(l,rr)) ?
        const AstNodeBiComAsv* const bnodep = VN_CAST(nodep, NodeBiComAsv);
        if (!bnodep) return false;
        const AstNodeBiComAsv* const rnodep = VN_CAST(bnodep->rhsp(), NodeBiComAsv);
        if (!rnodep) return false;
        if (rnodep->type() != bnodep->type()) return false;
        if (rnodep->width() != bnodep->width()) return false;
        return VN_IS(rnodep->lhsp(), Const);
    }
    static bool operandSubAdd(const AstNode* nodep) {
        // SUB( ADD(CONSTx,y), CONSTz) -> ADD(SUB(CONSTx,CONSTz), y)
        const AstNodeBiop* const np = VN_CAST(nodep, NodeBiop);
        const AstNodeBiop* const lp = VN_CAST(np->lhsp(), NodeBiop);
        return (lp && VN_IS(lp->lhsp(), Const) && VN_IS(np->rhsp(), Const)
                && lp->width() == np->width());
    }
    bool matchRedundantClean(AstAnd* andp) {
        // Remove And with constant one inserted by V3Clean
        // 1 & (a == b)  -> (IData)(a == b)
        // When bool is casted to int, the value is either 0 or 1
        AstConst* const constp = VN_AS(andp->lhsp(), Const);
        UASSERT_OBJ(constp && constp->isOne(), andp->lhsp(), "TRREEOPC must meet this condition");
        AstNodeExpr* const rhsp = andp->rhsp();
        AstCCast* ccastp = nullptr;
        const auto isEqOrNeq
            = [](AstNode* nodep) -> bool { return VN_IS(nodep, Eq) || VN_IS(nodep, Neq); };
        if (isEqOrNeq(rhsp)) {
            ccastp = new AstCCast{andp->fileline(), rhsp->unlinkFrBack(), andp};
        } else if (AstCCast* const tmpp = VN_CAST(rhsp, CCast)) {
            if (isEqOrNeq(tmpp->lhsp())) {
                if (tmpp->width() == andp->width()) {
                    tmpp->unlinkFrBack();
                    ccastp = tmpp;
                } else {
                    ccastp = new AstCCast{andp->fileline(), tmpp->lhsp()->unlinkFrBack(), andp};
                }
            }
        }
        if (ccastp) {
            andp->replaceWithKeepDType(ccastp);
            VL_DO_DANGLING(pushDeletep(andp), andp);
            return true;
        }
        return false;
    }

    static bool operandAndOrSame(const AstNode* nodep) {
        // OR( AND(VAL,x), AND(VAL,y)) -> AND(VAL,OR(x,y))
        // OR( AND(x,VAL), AND(y,VAL)) -> AND(OR(x,y),VAL)
        const AstNodeBiop* const np = VN_CAST(nodep, NodeBiop);
        const AstNodeBiop* const lp = VN_CAST(np->lhsp(), NodeBiop);
        const AstNodeBiop* const rp = VN_CAST(np->rhsp(), NodeBiop);
        return (lp && rp && lp->width() == rp->width() && lp->type() == rp->type()
                && (operandsSame(lp->lhsp(), rp->lhsp()) || operandsSame(lp->rhsp(), rp->rhsp())));
    }
    bool matchOrAndNot(AstNodeBiop* nodep) {
        // AstOr{$a, AstAnd{AstNot{$b}, $c}} if $a.width1, $a==$b => AstOr{$a,$c}
        // Someday we'll sort the biops completely and this can be simplified
        // This often results from our simplified clock generation:
        // if (rst) ... else if (enable)... -> OR(rst,AND(!rst,enable))
        AstNodeExpr* ap;
        AstNodeBiop* andp;
        if (VN_IS(nodep->lhsp(), And)) {
            andp = VN_AS(nodep->lhsp(), And);
            ap = nodep->rhsp();
        } else if (VN_IS(nodep->rhsp(), And)) {
            andp = VN_AS(nodep->rhsp(), And);
            ap = nodep->lhsp();
        } else {
            return false;
        }
        const AstNodeUniop* notp;
        AstNodeExpr* cp;
        if (VN_IS(andp->lhsp(), Not)) {
            notp = VN_AS(andp->lhsp(), Not);
            cp = andp->rhsp();
        } else if (VN_IS(andp->rhsp(), Not)) {
            notp = VN_AS(andp->rhsp(), Not);
            cp = andp->lhsp();
        } else {
            return false;
        }
        AstNodeExpr* const bp = notp->lhsp();
        if (!operandsSame(ap, bp)) return false;
        // Do it
        cp->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(andp->unlinkFrBack()), andp);
        VL_DANGLING(notp);
        // Replace whichever branch is now dangling
        if (nodep->rhsp()) {
            nodep->lhsp(cp);
        } else {
            nodep->rhsp(cp);
        }
        return true;
    }
    bool matchAndCond(AstAnd* nodep) {
        // Push down a AND into conditional, when one side of conditional is constant
        // (otherwise we'd be trading one operation for two operations)
        // V3Clean often makes this pattern, as it postpones the AND until
        // as high as possible, which is usually the right choice, except for this.
        AstNodeCond* const condp = VN_CAST(nodep->rhsp(), NodeCond);
        if (!condp) return false;
        if (!VN_IS(condp->thenp(), Const) && !VN_IS(condp->elsep(), Const)) return false;
        AstConst* const maskp = VN_CAST(nodep->lhsp(), Const);
        if (!maskp) return false;
        UINFO(4, "AND(CONSTm, CONDcond(c, i, e))->CONDcond(c, AND(m,i), AND(m, e)) " << nodep);
        AstNodeCond* const newp = static_cast<AstNodeCond*>(condp->cloneType(
            condp->condp()->unlinkFrBack(),
            new AstAnd{nodep->fileline(), maskp->cloneTree(false), condp->thenp()->unlinkFrBack()},
            new AstAnd{nodep->fileline(), maskp->cloneTree(false),
                       condp->elsep()->unlinkFrBack()}));
        newp->thenp()->dtypeFrom(nodep);  // As And might have been to change widths
        newp->elsep()->dtypeFrom(nodep);
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }
    bool matchMaskedOr(AstAnd* nodep) {
        // Masking an OR with terms that have no bits set under the mask is replaced with masking
        // only the remaining terms. Canonical example as generated by V3Expand is:
        // 0xff & (a << 8 | b >> 24) --> 0xff & (b >> 24)

        // Compute how many significant bits are in the mask
        const AstConst* const constp = VN_AS(nodep->lhsp(), Const);
        const uint32_t significantBits = constp->num().widthToFit();

        AstOr* const orp = VN_AS(nodep->rhsp(), Or);

        // Predicate for checking whether the bottom 'significantBits' bits of the given expression
        // are all zeroes.
        const auto checkBottomClear = [=](const AstNode* nodep) -> bool {
            if (const AstShiftL* const shiftp = VN_CAST(nodep, ShiftL)) {
                if (const AstConst* const scp = VN_CAST(shiftp->rhsp(), Const)) {
                    return scp->num().toUInt() >= significantBits;
                }
            }
            return false;
        };

        const bool orLIsRedundant = checkBottomClear(orp->lhsp());
        const bool orRIsRedundant = checkBottomClear(orp->rhsp());

        if (orLIsRedundant && orRIsRedundant) {
            nodep->replaceWithKeepDType(
                new AstConst{nodep->fileline(), AstConst::DTyped{}, nodep->dtypep()});
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            return true;
        } else if (orLIsRedundant) {
            orp->replaceWithKeepDType(orp->rhsp()->unlinkFrBack());
            VL_DO_DANGLING(pushDeletep(orp), orp);
            return false;  // input node is still valid, keep going
        } else if (orRIsRedundant) {
            orp->replaceWithKeepDType(orp->lhsp()->unlinkFrBack());
            VL_DO_DANGLING(pushDeletep(orp), orp);
            return false;  // input node is still valid, keep going
        } else {
            return false;
        }
    }
    bool matchMaskedShift(AstAnd* nodep) {
        // Drop redundant masking of right shift result. E.g: 0xff & ((uint32_t)a >> 24). This
        // commonly appears after V3Expand and the simplification in matchMaskedOr. Similarly,
        // drop redundant masking of left shift result. E.g.: 0xff000000 & ((uint32_t)a << 24).

        const auto checkMask = [nodep, this](const V3Number& mask) -> bool {
            const AstConst* const constp = VN_AS(nodep->lhsp(), Const);
            if (constp->num().isCaseEq(mask)) {
                AstNode* const rhsp = nodep->rhsp();
                rhsp->unlinkFrBack();
                nodep->replaceWithKeepDType(rhsp);
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
                return true;
            }
            return false;
        };

        // Check if masking is redundant
        if (const AstShiftR* const shiftp = VN_CAST(nodep->rhsp(), ShiftR)) {
            if (const AstConst* const scp = VN_CAST(shiftp->rhsp(), Const)) {
                // Check if mask is full over the non-zero bits
                V3Number maskLo{nodep, nodep->width()};
                maskLo.setMask(nodep->width() - scp->num().toUInt());
                return checkMask(maskLo);
            }
        } else if (const AstShiftL* const shiftp = VN_CAST(nodep->rhsp(), ShiftL)) {
            if (const AstConst* const scp = VN_CAST(shiftp->rhsp(), Const)) {
                // Check if mask is full over the non-zero bits
                V3Number mask{nodep, nodep->width()};
                const uint32_t shiftAmount = scp->num().toUInt();
                mask.setMask(nodep->width() - shiftAmount, shiftAmount);
                return checkMask(mask);
            }
        }
        return false;
    }

    bool matchBitOpTree(AstNodeExpr* nodep) {
        if (nodep->widthMin() != 1) return false;
        if (!v3Global.opt.fConstBitOpTree()) return false;

        string debugPrefix;
        if (debug() >= 9) {  // LCOV_EXCL_START
            static int c = 0;
            debugPrefix = "-  matchBitOpTree[";
            debugPrefix += cvtToStr(++c);
            debugPrefix += "] ";
            nodep->dumpTree(debugPrefix + "INPUT: ");
        }  // LCOV_EXCL_STOP

        AstNode* newp = nullptr;
        const AstAnd* const andp = VN_CAST(nodep, And);
        const int width = nodep->width();
        if (andp && isConst(andp->lhsp(), 1)) {  // 1 & BitOpTree
            newp = ConstBitOpTreeVisitor::simplify(andp->rhsp(), width, 1, m_statBitOpReduction);
        } else {  // BitOpTree
            newp = ConstBitOpTreeVisitor::simplify(nodep, width, 0, m_statBitOpReduction);
        }

        if (newp) {
            nodep->replaceWithKeepDType(newp);
            UINFO(4, "Transformed leaf of bit tree to " << newp);
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        }

        if (debug() >= 9) {  // LCOV_EXCL_START
            if (newp) {
                newp->dumpTree(debugPrefix + "RESULT: ");
            } else {
                cout << debugPrefix << "not replaced" << endl;
            }
        }  // LCOV_EXCL_STOP

        return newp;
    }
    static bool operandShiftSame(const AstNode* nodep) {
        const AstNodeBiop* const np = VN_AS(nodep, NodeBiop);
        {
            const AstShiftL* const lp = VN_CAST(np->lhsp(), ShiftL);
            const AstShiftL* const rp = VN_CAST(np->rhsp(), ShiftL);
            if (lp && rp) {
                return (lp->width() == rp->width() && lp->lhsp()->width() == rp->lhsp()->width()
                        && operandsSame(lp->rhsp(), rp->rhsp()));
            }
        }
        {
            const AstShiftR* const lp = VN_CAST(np->lhsp(), ShiftR);
            const AstShiftR* const rp = VN_CAST(np->rhsp(), ShiftR);
            if (lp && rp) {
                return (lp->width() == rp->width() && lp->lhsp()->width() == rp->lhsp()->width()
                        && operandsSame(lp->rhsp(), rp->rhsp()));
            }
        }
        return false;
    }
    bool operandHugeShiftL(const AstNodeBiop* nodep) {
        return (VN_IS(nodep->rhsp(), Const) && !VN_AS(nodep->rhsp(), Const)->num().isFourState()
                && (!VN_AS(nodep->rhsp(), Const)->num().fitsInUInt()  // > 2^32 shift
                    || (VN_AS(nodep->rhsp(), Const)->toUInt()
                        >= static_cast<uint32_t>(nodep->width())))
                && nodep->lhsp()->isPure());
    }
    bool operandHugeShiftR(const AstNodeBiop* nodep) {
        return (VN_IS(nodep->rhsp(), Const) && !VN_AS(nodep->rhsp(), Const)->num().isFourState()
                && (!VN_AS(nodep->rhsp(), Const)->num().fitsInUInt()  // > 2^32 shift
                    || (VN_AS(nodep->rhsp(), Const)->toUInt()
                        >= static_cast<uint32_t>(nodep->lhsp()->width())))
                && nodep->lhsp()->isPure());
    }
    bool operandIsTwo(const AstNode* nodep) {
        const AstConst* const constp = VN_CAST(nodep, Const);
        if (!constp) return false;  // not constant
        if (constp->num().isFourState()) return false;  // four-state
        if (nodep->width() > VL_QUADSIZE) return false;  // too wide
        if (nodep->isSigned() && constp->num().isNegative()) return false;  // signed and negative
        return constp->toUQuad() == 2;
    }
    bool operandIsTwostate(const AstNode* nodep) {
        return (VN_IS(nodep, Const) && !VN_AS(nodep, Const)->num().isFourState());
    }
    bool operandIsPowTwo(const AstNode* nodep) {
        if (!operandIsTwostate(nodep)) return false;
        return (1 == VN_AS(nodep, Const)->num().countOnes());
    }
    bool operandShiftOp(const AstNodeBiop* nodep) {
        if (!VN_IS(nodep->rhsp(), Const)) return false;
        const AstNodeBiop* const lhsp = VN_CAST(nodep->lhsp(), NodeBiop);
        if (!lhsp || !(VN_IS(lhsp, And) || VN_IS(lhsp, Or) || VN_IS(lhsp, Xor))) return false;
        if (nodep->width() != lhsp->width()) return false;
        if (nodep->width() != lhsp->lhsp()->width()) return false;
        if (nodep->width() != lhsp->rhsp()->width()) return false;
        return true;
    }
    bool operandShiftShift(const AstNodeBiop* nodep) {
        // We could add a AND though.
        const AstNodeBiop* const lhsp = VN_CAST(nodep->lhsp(), NodeBiop);
        if (!lhsp || !(VN_IS(lhsp, ShiftL) || VN_IS(lhsp, ShiftR))) return false;
        // We can only get rid of a<<b>>c or a<<b<<c, with constant b & c
        // because bits may be masked in that process, or (b+c) may exceed the word width.
        if (!(VN_IS(nodep->rhsp(), Const) && VN_IS(lhsp->rhsp(), Const))) return false;
        if (VN_AS(nodep->rhsp(), Const)->num().isFourState()
            || VN_AS(lhsp->rhsp(), Const)->num().isFourState())
            return false;
        if (nodep->width() != lhsp->width()) return false;
        if (nodep->width() != lhsp->lhsp()->width()) return false;
        return true;
    }
    bool operandWordOOB(const AstWordSel* nodep) {
        // V3Expand may make a arraysel that exceeds the bounds of the array
        // It was an expression, then got constified.  In reality, the WordSel
        // must be wrapped in a Cond, that will be false.
        return (VN_IS(nodep->bitp(), Const) && VN_IS(nodep->fromp(), NodeVarRef)
                && VN_AS(nodep->fromp(), NodeVarRef)->access().isReadOnly()
                && (static_cast<int>(VN_AS(nodep->bitp(), Const)->toUInt())
                    >= VN_AS(nodep->fromp(), NodeVarRef)->varp()->widthWords()));
    }
    bool operandSelFull(const AstSel* nodep) {
        return (VN_IS(nodep->lsbp(), Const) && nodep->lsbConst() == 0
                && static_cast<int>(nodep->widthConst()) == nodep->fromp()->width());
    }
    bool operandSelExtend(AstSel* nodep) {
        // A pattern created by []'s after offsets have been removed
        // SEL(EXTEND(any,width,...),(width-1),0) -> ...
        // Since select's return unsigned, this is always an extend
        AstExtend* const extendp = VN_CAST(nodep->fromp(), Extend);
        if (!(m_doV && extendp && VN_IS(nodep->lsbp(), Const) && nodep->lsbConst() == 0
              && static_cast<int>(nodep->widthConst()) == extendp->lhsp()->width()))
            return false;
        VL_DO_DANGLING(replaceWChild(nodep, extendp->lhsp()), nodep);
        return true;
    }
    bool operandSelBiLower(AstSel* nodep) {
        // SEL(ADD(a,b),(width-1),0) -> ADD(SEL(a),SEL(b))
        // Add or any operation which doesn't care if we discard top bits
        AstNodeBiop* const bip = VN_CAST(nodep->fromp(), NodeBiop);
        if (!(m_doV && bip && VN_IS(nodep->lsbp(), Const) && nodep->lsbConst() == 0)) return false;
        if (debug() >= 9) nodep->dumpTree("-  SEL(BI)-in: ");
        AstNodeExpr* const bilhsp = bip->lhsp()->unlinkFrBack();
        AstNodeExpr* const birhsp = bip->rhsp()->unlinkFrBack();
        bip->lhsp(new AstSel{nodep->fileline(), bilhsp, 0, nodep->widthConst()});
        bip->rhsp(new AstSel{nodep->fileline(), birhsp, 0, nodep->widthConst()});
        if (debug() >= 9) bip->dumpTree("-  SEL(BI)-ou: ");
        VL_DO_DANGLING(replaceWChild(nodep, bip), nodep);
        return true;
    }
    bool operandSelShiftLower(AstSel* nodep) {
        // AND({a}, SHIFTR({b}, {c})) is often shorthand in C for Verilog {b}[{c} :+ {a}]
        // becomes thought other optimizations
        // SEL(SHIFTR({a},{b}),{lsb},{width}) -> SEL({a},{lsb+b},{width})
        AstShiftR* const shiftp = VN_CAST(nodep->fromp(), ShiftR);
        if (!(m_doV && shiftp && VN_IS(shiftp->rhsp(), Const) && VN_IS(nodep->lsbp(), Const))) {
            return false;
        }
        AstNodeExpr* const ap = shiftp->lhsp();
        AstConst* const bp = VN_AS(shiftp->rhsp(), Const);
        AstConst* const lp = VN_AS(nodep->lsbp(), Const);
        if (bp->isWide() || bp->num().isFourState() || bp->num().isNegative() || lp->isWide()
            || lp->num().isFourState() || lp->num().isNegative()) {
            return false;
        }
        const int newLsb = lp->toSInt() + bp->toSInt();
        if (newLsb + nodep->widthConst() > ap->width()) return false;
        //
        UINFO(9, "SEL(SHIFTR(a,b),l,w) -> SEL(a,l+b,w)");
        if (debug() >= 9) nodep->dumpTree("-  SEL(SH)-in: ");
        AstSel* const newp
            = new AstSel{nodep->fileline(), ap->unlinkFrBack(), newLsb, nodep->widthConst()};
        nodep->replaceWithKeepDType(newp);
        if (debug() >= 9) newp->dumpTree("-  SEL(SH)-ou: ");
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }

    bool operandBiExtendConstShrink(AstNodeBiop* nodep) {
        // Loop unrolling favors standalone compares
        // EQ(const{width32}, EXTEND(xx{width3})) -> EQ(const{3}, xx{3})
        // The constant must have zero bits (+ 1 if signed) or compare
        // would be incorrect. See also operandBiExtendConst
        AstExtend* const extendp = VN_CAST(nodep->rhsp(), Extend);
        if (!extendp) return false;
        AstNodeExpr* const smallerp = extendp->lhsp();
        const int subsize = smallerp->width();
        AstConst* const constp = VN_CAST(nodep->lhsp(), Const);
        if (!constp) return false;
        if (!constp->num().isBitsZero(constp->width() - 1, subsize)) return false;
        //
        if (debug() >= 9) nodep->dumpTree("-  BI(EXTEND)-in: ");
        smallerp->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(extendp->unlinkFrBack()), extendp);  // aka nodep->lhsp.
        nodep->rhsp(smallerp);

        constp->unlinkFrBack();
        V3Number num{constp, subsize, constp->num()};
        nodep->lhsp(new AstConst{constp->fileline(), num});
        VL_DO_DANGLING(pushDeletep(constp), constp);
        if (debug() >= 9) nodep->dumpTree("-  BI(EXTEND)-ou: ");
        return true;
    }
    bool operandBiExtendConstOver(const AstNodeBiop* nodep) {
        // EQ(const{width32}, EXTEND(xx{width3})) -> constant
        // When the constant has non-zero bits above the extend it's a constant.
        // Avoids compiler warning
        const AstExtend* const extendp = VN_CAST(nodep->rhsp(), Extend);
        if (!extendp) return false;
        AstNode* const smallerp = extendp->lhsp();
        const int subsize = smallerp->width();
        const AstConst* const constp = VN_CAST(nodep->lhsp(), Const);
        if (!constp) return false;
        if (constp->num().isBitsZero(constp->width() - 1, subsize)) return false;
        return true;
    }

    // Extraction checks
    bool warnSelect(AstSel* nodep) {
        if (m_doGenerate) {
            // Never checked yet
            V3Width::widthParamsEdit(nodep);
            iterateChildren(nodep);  // May need "constifying"
        }
        // Find range of dtype we are selecting from
        // Similar code in V3Unknown::AstSel
        const bool doit = true;
        if (m_warn && VN_IS(nodep->lsbp(), Const) && doit) {
            const int maxDeclBit = nodep->declRange().hiMaxSelect() * nodep->declElWidth()
                                   + (nodep->declElWidth() - 1);
            if (VN_AS(nodep->lsbp(), Const)->num().isFourState()) {
                nodep->v3error("Selection index is constantly unknown or tristated: "
                               "lsb="
                               << nodep->lsbp()->name() << " width=" << nodep->widthConst());
                // Replacing nodep will make a mess above, so we replace the offender
                replaceZero(nodep->lsbp());
            } else if (nodep->declRange().ranged()
                       && (nodep->msbConst() > maxDeclBit || nodep->lsbConst() > maxDeclBit)) {
                // See also warning in V3Width
                // Must adjust by element width as declRange() is in number of elements
                string msbLsbProtected;
                if (nodep->declElWidth() == 0) {
                    msbLsbProtected = "(nodep->declElWidth() == 0) "
                                      + std::to_string(nodep->msbConst()) + ":"
                                      + std::to_string(nodep->lsbConst());
                } else {
                    msbLsbProtected = std::to_string(nodep->msbConst() / nodep->declElWidth())
                                      + ":"
                                      + std::to_string(nodep->lsbConst() / nodep->declElWidth());
                }
                nodep->v3warn(SELRANGE,
                              "Selection index out of range: "
                                  << msbLsbProtected << " outside "
                                  << nodep->declRange().hiMaxSelect() << ":0"
                                  << (nodep->declRange().lo() >= 0
                                          ? ""
                                          : (" (adjusted +" + cvtToStr(-nodep->declRange().lo())
                                             + " to account for negative lsb)")));
                UINFO(1, "    Related Raw index is " << nodep->msbConst() << ":"
                                                     << nodep->lsbConst());
                // Don't replace with zero, we'll do it later
            }
        }
        return false;  // Not a transform, so NOP
    }

    static bool operandsSame(AstNode* node1p, AstNode* node2p) {
        // For now we just detect constants & simple vars, though it could be more generic
        if (VN_IS(node1p, Const) && VN_IS(node2p, Const)) return node1p->sameGateTree(node2p);
        if (VN_IS(node1p, VarRef) && VN_IS(node2p, VarRef)) {
            // Avoid comparing widthMin's, which results in lost optimization attempts
            // If cleanup sameGateTree to be smarter, this can be restored.
            // return node1p->sameGateTree(node2p);
            return node1p->isSame(node2p);
        }
        // Pattern created by coverage-line; avoid compiler tautological-compare warning
        if (AstAnd* const and1p = VN_CAST(node1p, And)) {
            if (AstAnd* const and2p = VN_CAST(node2p, And)) {
                if (VN_IS(and1p->lhsp(), Const) && VN_IS(and1p->rhsp(), NodeVarRef)
                    && VN_IS(and2p->lhsp(), Const) && VN_IS(and2p->rhsp(), NodeVarRef))
                    return node1p->sameGateTree(node2p);
            }
        }
        return false;
    }
    bool ifSameAssign(const AstNodeIf* nodep) {
        const AstNodeAssign* const thensp = VN_CAST(nodep->thensp(), NodeAssign);
        const AstNodeAssign* const elsesp = VN_CAST(nodep->elsesp(), NodeAssign);
        if (!thensp || thensp->nextp()) return false;  // Must be SINGLE statement
        if (!elsesp || elsesp->nextp()) return false;
        if (thensp->type() != elsesp->type()) return false;  // Can't mix an assigndly with assign
        if (!thensp->lhsp()->sameGateTree(elsesp->lhsp())) return false;
        if (!thensp->rhsp()->gateTree()) return false;
        if (!elsesp->rhsp()->gateTree()) return false;
        if (m_underRecFunc) return false;  // This optimization may lead to infinite recursion
        return true;
    }
    bool operandIfIf(const AstNodeIf* nodep) {
        if (nodep->elsesp()) return false;
        const AstNodeIf* const lowerIfp = VN_CAST(nodep->thensp(), NodeIf);
        if (!lowerIfp || lowerIfp->nextp()) return false;
        if (nodep->type() != lowerIfp->type()) return false;
        if (AstNode::afterCommentp(lowerIfp->elsesp())) return false;
        return true;
    }
    bool ifConcatMergeableBiop(const AstNode* nodep) {
        return (VN_IS(nodep, And) || VN_IS(nodep, Or) || VN_IS(nodep, Xor));
    }
    bool ifAdjacentSel(const AstSel* lhsp, const AstSel* rhsp) {
        if (!v3Global.opt.fAssemble()) return false;  // opt disabled
        if (!lhsp || !rhsp) return false;
        const AstNode* const lfromp = lhsp->fromp();
        const AstNode* const rfromp = rhsp->fromp();
        if (!lfromp || !rfromp || !lfromp->sameGateTree(rfromp)) return false;
        const AstConst* const lstart = VN_CAST(lhsp->lsbp(), Const);
        const AstConst* const rstart = VN_CAST(rhsp->lsbp(), Const);
        if (!lstart || !rstart) return false;  // too complicated
        const int rend = (rstart->toSInt() + rhsp->widthConst());
        return (rend == lstart->toSInt());
    }
    bool ifMergeAdjacent(AstNodeExpr* lhsp, AstNodeExpr* rhsp) {
        // called by concatmergeable to determine if {lhsp, rhsp} make sense
        if (!v3Global.opt.fAssemble()) return false;  // opt disabled
        // two same varref
        if (operandsSame(lhsp, rhsp)) return true;
        const AstSel* lselp = VN_CAST(lhsp, Sel);
        const AstSel* rselp = VN_CAST(rhsp, Sel);
        // a[i:0] a
        if (lselp && !rselp && rhsp->sameGateTree(lselp->fromp()))
            rselp = new AstSel{rhsp->fileline(), rhsp->cloneTreePure(false), 0, rhsp->width()};
        // a[i:j] {a[j-1:k], b}
        if (lselp && !rselp && VN_IS(rhsp, Concat))
            return ifMergeAdjacent(lhsp, VN_CAST(rhsp, Concat)->lhsp());
        // a a[msb:j]
        if (rselp && !lselp && lhsp->sameGateTree(rselp->fromp()))
            lselp = new AstSel{lhsp->fileline(), lhsp->cloneTreePure(false), 0, lhsp->width()};
        // {b, a[j:k]} a[k-1:i]
        if (rselp && !lselp && VN_IS(lhsp, Concat))
            return ifMergeAdjacent(VN_CAST(lhsp, Concat)->rhsp(), rhsp);
        if (!lselp || !rselp) return false;

        // a[a:b] a[b-1:c] are adjacent
        AstNode* const lfromp = lselp->fromp();
        AstNode* const rfromp = rselp->fromp();
        if (!lfromp || !rfromp || !lfromp->sameGateTree(rfromp)) return false;
        AstConst* const lstart = VN_CAST(lselp->lsbp(), Const);
        AstConst* const rstart = VN_CAST(rselp->lsbp(), Const);
        if (!lstart || !rstart) return false;  // too complicated
        const int rend = (rstart->toSInt() + rselp->widthConst());
        // a[i:j] a[j-1:k]
        if (rend == lstart->toSInt()) return true;
        // a[i:0] a[msb:j]
        if (rend == rfromp->width() && lstart->toSInt() == 0) return true;
        return false;
    }
    bool concatMergeable(const AstNodeExpr* lhsp, const AstNodeExpr* rhsp, unsigned depth) {
        // determine if {a OP b, c OP d} => {a, c} OP {b, d} is advantageous
        if (!v3Global.opt.fAssemble()) return false;  // opt disabled
        if (lhsp->type() != rhsp->type()) return false;
        if (!ifConcatMergeableBiop(lhsp)) return false;
        if (depth > CONCAT_MERGABLE_MAX_DEPTH) return false;  // As worse case O(n^2) algorithm

        const AstNodeBiop* const lp = VN_CAST(lhsp, NodeBiop);
        const AstNodeBiop* const rp = VN_CAST(rhsp, NodeBiop);
        if (!lp || !rp) return false;
        // {a[]&b[], a[]&b[]}
        const bool lad = ifMergeAdjacent(lp->lhsp(), rp->lhsp());
        const bool rad = ifMergeAdjacent(lp->rhsp(), rp->rhsp());
        if (lad && rad) return true;
        // {a[] & b[]&c[], a[] & b[]&c[]}
        if (lad && concatMergeable(lp->rhsp(), rp->rhsp(), depth + 1)) return true;
        // {a[]&b[] & c[], a[]&b[] & c[]}
        if (rad && concatMergeable(lp->lhsp(), rp->lhsp(), depth + 1)) return true;
        // {(a[]&b[])&(c[]&d[]), (a[]&b[])&(c[]&d[])}
        if (concatMergeable(lp->lhsp(), rp->lhsp(), depth + 1)
            && concatMergeable(lp->rhsp(), rp->rhsp(), depth + 1)) {
            return true;
        }
        return false;
    }
    static bool operandsSameWidth(const AstNode* lhsp, const AstNode* rhsp) {
        return lhsp->width() == rhsp->width();
    }

    //----------------------------------------
    // Constant Replacement functions.
    // These all take a node, delete its tree, and replaces it with a constant

    void replaceNum(AstNode* oldp, const V3Number& num) {
        // Replace oldp node with a constant set to specified value
        UASSERT(oldp, "Null old");
        UASSERT_OBJ(!(VN_IS(oldp, Const) && !VN_AS(oldp, Const)->num().isFourState()), oldp,
                    "Already constant??");
        AstNode* const newp = new AstConst{oldp->fileline(), num};
        oldp->replaceWithKeepDType(newp);
        if (debug() > 5) oldp->dumpTree("-  const_old: ");
        if (debug() > 5) newp->dumpTree("-       _new: ");
        VL_DO_DANGLING(pushDeletep(oldp), oldp);
    }
    void replaceNum(AstNode* nodep, uint32_t val) {
        V3Number num{nodep, nodep->width(), val};
        VL_DO_DANGLING(replaceNum(nodep, num), nodep);
    }
    void replaceNumSigned(AstNodeBiop* nodep, uint32_t val) {
        // We allow both sides to be constant, as one may have come from
        // parameter propagation, etc.
        if (m_warn && !(VN_IS(nodep->lhsp(), Const) && VN_IS(nodep->rhsp(), Const))) {
            nodep->v3warn(UNSIGNED, "Comparison is constant due to unsigned arithmetic");
        }
        VL_DO_DANGLING(replaceNum(nodep, val), nodep);
    }
    void replaceNumLimited(AstNodeBiop* nodep, uint32_t val) {
        // Avoids gcc warning about same
        if (m_warn) nodep->v3warn(CMPCONST, "Comparison is constant due to limited range");
        VL_DO_DANGLING(replaceNum(nodep, val), nodep);
    }
    void replaceZero(AstNode* nodep) { VL_DO_DANGLING(replaceNum(nodep, 0), nodep); }
    void replaceZeroChkPure(AstNode* nodep, AstNodeExpr* checkp) {
        // For example, "0 * n" -> 0 if n has no side effects
        // Else strength reduce it to 0 & n.
        // If ever change the operation note AstAnd rule specially ignores this created pattern
        if (checkp->isPure()) {
            VL_DO_DANGLING(replaceNum(nodep, 0), nodep);
        } else {
            AstNode* const newp = new AstAnd{nodep->fileline(), new AstConst{nodep->fileline(), 0},
                                             checkp->unlinkFrBack()};
            nodep->replaceWithKeepDType(newp);
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        }
    }
    void replaceAllOnes(AstNode* nodep) {
        V3Number ones{nodep, nodep->width(), 0};
        ones.setMask(nodep->width());
        VL_DO_DANGLING(replaceNum(nodep, ones), nodep);
    }
    void replaceConst(AstNodeUniop* nodep) {
        V3Number numv{nodep, nodep->widthMinV()};
        nodep->numberOperate(numv, constNumV(nodep->lhsp()));
        const V3Number& num = toNumC(nodep, numv);
        UINFO(4, "UNICONST -> " << num);
        VL_DO_DANGLING(replaceNum(nodep, num), nodep);
    }
    void replaceConst(AstNodeBiop* nodep) {
        V3Number numv{nodep, nodep->widthMinV()};
        nodep->numberOperate(numv, constNumV(nodep->lhsp()), constNumV(nodep->rhsp()));
        const V3Number& num = toNumC(nodep, numv);
        UINFO(4, "BICONST -> " << num);
        VL_DO_DANGLING(replaceNum(nodep, num), nodep);
    }
    void replaceConst(AstNodeTriop* nodep) {
        V3Number numv{nodep, nodep->widthMinV()};
        nodep->numberOperate(numv, constNumV(nodep->lhsp()), constNumV(nodep->rhsp()),
                             constNumV(nodep->thsp()));
        const V3Number& num = toNumC(nodep, numv);
        UINFO(4, "TRICONST -> " << num);
        VL_DO_DANGLING(replaceNum(nodep, num), nodep);
    }
    void replaceConst(AstNodeQuadop* nodep) {
        V3Number numv{nodep, nodep->widthMinV()};
        nodep->numberOperate(numv, constNumV(nodep->lhsp()), constNumV(nodep->rhsp()),
                             constNumV(nodep->thsp()), constNumV(nodep->fhsp()));
        const V3Number& num = toNumC(nodep, numv);
        UINFO(4, "QUADCONST -> " << num);
        VL_DO_DANGLING(replaceNum(nodep, num), nodep);
    }

    void replaceConstString(AstNode* oldp, const string& num) {
        // Replace oldp node with a constant set to specified value
        UASSERT(oldp, "Null old");
        AstNode* const newp = new AstConst{oldp->fileline(), AstConst::String{}, num};
        if (debug() > 5) oldp->dumpTree("-  const_old: ");
        if (debug() > 5) newp->dumpTree("-       _new: ");
        oldp->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(oldp), oldp);
    }
    //----------------------------------------
    // Replacement functions.
    // These all take a node and replace it with something else

    void replaceWChild(AstNode* nodep, AstNodeExpr* childp) {
        // NODE(..., CHILD(...)) -> CHILD(...)
        childp->unlinkFrBackWithNext();
        // If replacing a SEL for example, the data type comes from the parent (is less wide).
        // This may adversely affect the operation of the node being replaced.
        nodep->replaceWithKeepDType(childp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceWChildBool(AstNode* nodep, AstNodeExpr* childp) {
        // NODE(..., CHILD(...)) -> REDOR(CHILD(...))
        childp->unlinkFrBack();
        if (childp->width1()) {
            nodep->replaceWithKeepDType(childp);
        } else {
            nodep->replaceWithKeepDType(new AstRedOr{childp->fileline(), childp});
        }
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    //! Replace a ternary node with its RHS after iterating
    //! Used with short-circuiting, where the RHS has not yet been iterated.
    void replaceWIteratedRhs(AstNodeTriop* nodep) {
        if (AstNode* const rhsp = nodep->rhsp()) iterateAndNextNull(rhsp);
        replaceWChild(nodep, nodep->rhsp());  // May have changed
    }

    //! Replace a ternary node with its THS after iterating
    //! Used with short-circuiting, where the THS has not yet been iterated.
    void replaceWIteratedThs(AstNodeTriop* nodep) {
        if (AstNode* const thsp = nodep->thsp()) iterateAndNextNull(thsp);
        replaceWChild(nodep, nodep->thsp());  // May have changed
    }
    void replaceWLhs(AstNodeUniop* nodep) {
        // Keep LHS, remove RHS
        replaceWChild(nodep, nodep->lhsp());
    }
    void replaceWLhs(AstNodeBiop* nodep) {
        // Keep LHS, remove RHS
        replaceWChild(nodep, nodep->lhsp());
    }
    void replaceWRhs(AstNodeBiop* nodep) {
        // Keep RHS, remove LHS
        replaceWChild(nodep, nodep->rhsp());
    }
    void replaceWLhsBool(AstNodeBiop* nodep) { replaceWChildBool(nodep, nodep->lhsp()); }
    void replaceWRhsBool(AstNodeBiop* nodep) { replaceWChildBool(nodep, nodep->rhsp()); }
    void replaceAsv(AstNodeBiop* nodep) {
        // BIASV(CONSTa, BIASV(CONSTb, c)) -> BIASV( BIASV_CONSTED(a,b), c)
        // BIASV(SAMEa,  BIASV(SAMEb, c))  -> BIASV( BIASV(SAMEa,SAMEb), c)
        // if (debug()) nodep->dumpTree("-  repAsvConst_old: ");
        AstNodeExpr* const ap = nodep->lhsp();
        AstNodeBiop* const rp = VN_AS(nodep->rhsp(), NodeBiop);
        AstNodeExpr* const bp = rp->lhsp();
        AstNodeExpr* const cp = rp->rhsp();
        ap->unlinkFrBack();
        bp->unlinkFrBack();
        cp->unlinkFrBack();
        rp->unlinkFrBack();
        nodep->lhsp(rp);
        nodep->rhsp(cp);
        rp->lhsp(ap);
        rp->rhsp(bp);
        rp->dtypeFrom(nodep);  // Upper widthMin more likely correct
        if (VN_IS(rp->lhsp(), Const) && VN_IS(rp->rhsp(), Const)) replaceConst(rp);
        // if (debug()) nodep->dumpTree("-  repAsvConst_new: ");
    }
    void replaceAsvLUp(AstNodeBiop* nodep) {
        // BIASV(BIASV(CONSTll,lr),r) -> BIASV(CONSTll,BIASV(lr,r))
        AstNodeBiop* const lp = VN_AS(nodep->lhsp()->unlinkFrBack(), NodeBiop);
        AstNodeExpr* const llp = lp->lhsp()->unlinkFrBack();
        AstNodeExpr* const lrp = lp->rhsp()->unlinkFrBack();
        AstNodeExpr* const rp = nodep->rhsp()->unlinkFrBack();
        nodep->lhsp(llp);
        nodep->rhsp(lp);
        lp->lhsp(lrp);
        lp->rhsp(rp);
        lp->dtypeFrom(nodep);  // Upper widthMin more likely correct
        // if (debug()) nodep->dumpTree("-  repAsvLUp_new: ");
    }
    void replaceAsvRUp(AstNodeBiop* nodep) {
        // BIASV(l,BIASV(CONSTrl,rr)) -> BIASV(CONSTrl,BIASV(l,rr))
        AstNodeExpr* const lp = nodep->lhsp()->unlinkFrBack();
        AstNodeBiop* const rp = VN_AS(nodep->rhsp()->unlinkFrBack(), NodeBiop);
        AstNodeExpr* const rlp = rp->lhsp()->unlinkFrBack();
        AstNodeExpr* const rrp = rp->rhsp()->unlinkFrBack();
        nodep->lhsp(rlp);
        nodep->rhsp(rp);
        rp->lhsp(lp);
        rp->rhsp(rrp);
        rp->dtypeFrom(nodep);  // Upper widthMin more likely correct
        // if (debug()) nodep->dumpTree("-  repAsvRUp_new: ");
    }
    void replaceAndOr(AstNodeBiop* nodep) {
        //  OR  (AND (CONSTll,lr), AND(CONSTrl==ll,rr))    -> AND (CONSTll, OR(lr,rr))
        //  OR  (AND (CONSTll,lr), AND(CONSTrl,    rr=lr)) -> AND (OR(ll,rl), rr)
        // nodep ^lp  ^llp   ^lrp  ^rp  ^rlp       ^rrp
        // (Or/And may also be reversed)
        AstNodeBiop* const lp = VN_AS(nodep->lhsp()->unlinkFrBack(), NodeBiop);
        AstNodeExpr* const llp = lp->lhsp()->unlinkFrBack();
        AstNodeExpr* const lrp = lp->rhsp()->unlinkFrBack();
        AstNodeBiop* const rp = VN_AS(nodep->rhsp()->unlinkFrBack(), NodeBiop);
        AstNodeExpr* const rlp = rp->lhsp()->unlinkFrBack();
        AstNodeExpr* const rrp = rp->rhsp()->unlinkFrBack();
        nodep->replaceWithKeepDType(lp);
        if (operandsSame(llp, rlp)) {
            lp->lhsp(llp);
            lp->rhsp(nodep);
            lp->dtypeFrom(nodep);
            nodep->lhsp(lrp);
            nodep->rhsp(rrp);
            VL_DO_DANGLING(pushDeletep(rp), rp);
            VL_DO_DANGLING(pushDeletep(rlp), rlp);
        } else if (operandsSame(lrp, rrp)) {
            lp->lhsp(nodep);
            lp->rhsp(rrp);
            lp->dtypeFrom(nodep);
            nodep->lhsp(llp);
            nodep->rhsp(rlp);
            VL_DO_DANGLING(pushDeletep(rp), rp);
            VL_DO_DANGLING(pushDeletep(lrp), lrp);
        } else {
            nodep->v3fatalSrc("replaceAndOr on something operandAndOrSame shouldn't have matched");
        }
        // if (debug()) nodep->dumpTree("-  repAndOr_new: ");
    }
    void replaceShiftSame(AstNodeBiop* nodep) {
        // Or(Shift(ll,CONSTlr),Shift(rl,CONSTrr==lr)) -> Shift(Or(ll,rl),CONSTlr)
        // (Or/And may also be reversed)
        AstNodeBiop* const lp = VN_AS(nodep->lhsp()->unlinkFrBack(), NodeBiop);
        AstNodeExpr* const llp = lp->lhsp()->unlinkFrBack();
        AstNodeExpr* const lrp = lp->rhsp()->unlinkFrBack();
        AstNodeBiop* const rp = VN_AS(nodep->rhsp()->unlinkFrBack(), NodeBiop);
        AstNodeExpr* const rlp = rp->lhsp()->unlinkFrBack();
        AstNodeExpr* const rrp = rp->rhsp()->unlinkFrBack();
        nodep->replaceWithKeepDType(lp);
        lp->lhsp(nodep);
        lp->rhsp(lrp);
        nodep->lhsp(llp);
        nodep->rhsp(rlp);
        nodep->dtypep(llp->dtypep());  // dtype of Biop is before shift.
        VL_DO_DANGLING(pushDeletep(rp), rp);
        VL_DO_DANGLING(pushDeletep(rrp), rrp);
        // if (debug()) nodep->dumpTree("-  repShiftSame_new: ");
    }
    void replaceConcatSel(AstConcat* nodep) {
        // {a[1], a[0]} -> a[1:0]
        AstSel* const lselp = VN_AS(nodep->lhsp()->unlinkFrBack(), Sel);
        AstSel* const rselp = VN_AS(nodep->rhsp()->unlinkFrBack(), Sel);
        const int lstart = lselp->lsbConst();
        const int lwidth = lselp->widthConst();
        const int rstart = rselp->lsbConst();
        const int rwidth = rselp->widthConst();

        UASSERT_OBJ((rstart + rwidth) == lstart, nodep,
                    "tried to merge two selects which are not adjacent");
        AstSel* const newselp = new AstSel{
            lselp->fromp()->fileline(), rselp->fromp()->unlinkFrBack(), rstart, lwidth + rwidth};
        UINFO(5, "merged two adjacent sel " << lselp << " and " << rselp << " to one " << newselp);

        nodep->replaceWithKeepDType(newselp);
        VL_DO_DANGLING(pushDeletep(lselp), lselp);
        VL_DO_DANGLING(pushDeletep(rselp), rselp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceConcatMerge(AstConcat* nodep) {
        // {llp OP lrp, rlp OP rrp} => {llp, rlp} OP {lrp, rrp}, where OP = AND/OR/XOR
        AstNodeBiop* const lp = VN_AS(nodep->lhsp(), NodeBiop);
        AstNodeBiop* const rp = VN_AS(nodep->rhsp(), NodeBiop);
        if (concatMergeable(lp, rp, 0)) {
            AstNodeExpr* const llp = lp->lhsp();
            AstNodeExpr* const lrp = lp->rhsp();
            AstNodeExpr* const rlp = rp->lhsp();
            AstNodeExpr* const rrp = rp->rhsp();
            AstConcat* const newlp = new AstConcat{rlp->fileline(), llp->cloneTreePure(false),
                                                   rlp->cloneTreePure(false)};
            AstConcat* const newrp = new AstConcat{rrp->fileline(), lrp->cloneTreePure(false),
                                                   rrp->cloneTreePure(false)};
            // use the lhs to replace the parent concat
            llp->replaceWith(newlp);
            VL_DO_DANGLING(pushDeletep(llp), llp);
            lrp->replaceWith(newrp);
            VL_DO_DANGLING(pushDeletep(lrp), lrp);
            lp->dtypeChgWidthSigned(newlp->width(), newlp->width(), VSigning::UNSIGNED);
            UINFO(5, "merged " << nodep);
            VL_DO_DANGLING(pushDeletep(rp->unlinkFrBack()), rp);
            nodep->replaceWithKeepDType(lp->unlinkFrBack());
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            iterate(lp->lhsp());
            iterate(lp->rhsp());
        } else {
            nodep->v3fatalSrc("tried to merge two Concat which are not adjacent");
        }
    }
    void replaceExtend(AstNode* nodep, AstNodeExpr* arg0p) {
        // -> EXTEND(nodep)
        // like a AstExtend{$rhsp}, but we need to set the width correctly from base node
        arg0p->unlinkFrBack();
        AstNodeExpr* const newp
            = (VN_IS(nodep, ExtendS)
                   ? static_cast<AstNodeExpr*>(new AstExtendS{nodep->fileline(), arg0p})
                   : static_cast<AstNodeExpr*>(new AstExtend{nodep->fileline(), arg0p}));
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replacePowShift(AstNodeBiop* nodep) {  // Pow or PowS
        UINFO(5, "POW(2,b)->SHIFTL(1,b) " << nodep);
        AstNodeExpr* const rhsp = nodep->rhsp()->unlinkFrBack();
        AstShiftL* const newp
            = new AstShiftL{nodep->fileline(), new AstConst{nodep->fileline(), 1}, rhsp};
        newp->lhsp()->dtypeFrom(nodep);
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceMulShift(AstMul* nodep) {  // Mul, but not MulS as not simple shift
        UINFO(5, "MUL(2^n,b)->SHIFTL(b,n) " << nodep);
        const int amount = VN_AS(nodep->lhsp(), Const)->num().mostSetBitP1() - 1;  // 2^n->n+1
        AstNodeExpr* const opp = nodep->rhsp()->unlinkFrBack();
        AstShiftL* const newp
            = new AstShiftL{nodep->fileline(), opp, new AstConst(nodep->fileline(), amount)};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceDivShift(AstDiv* nodep) {  // Mul, but not MulS as not simple shift
        UINFO(5, "DIV(b,2^n)->SHIFTR(b,n) " << nodep);
        const int amount = VN_AS(nodep->rhsp(), Const)->num().mostSetBitP1() - 1;  // 2^n->n+1
        AstNodeExpr* const opp = nodep->lhsp()->unlinkFrBack();
        AstShiftR* const newp
            = new AstShiftR{nodep->fileline(), opp, new AstConst(nodep->fileline(), amount)};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceModAnd(AstModDiv* nodep) {  // Mod, but not ModS as not simple shift
        UINFO(5, "MOD(b,2^n)->AND(b,2^n-1) " << nodep);
        const int amount = VN_AS(nodep->rhsp(), Const)->num().mostSetBitP1() - 1;  // 2^n->n+1
        V3Number mask{nodep, nodep->width()};
        mask.setMask(amount);
        AstNodeExpr* const opp = nodep->lhsp()->unlinkFrBack();
        AstAnd* const newp
            = new AstAnd{nodep->fileline(), opp, new AstConst{nodep->fileline(), mask}};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceShiftOp(AstNodeBiop* nodep) {
        UINFO(5, "SHIFT(AND(a,b),CONST)->AND(SHIFT(a,CONST),SHIFT(b,CONST)) " << nodep);
        const int width = nodep->width();
        const int widthMin = nodep->widthMin();
        VNRelinker handle;
        nodep->unlinkFrBack(&handle);
        AstNodeBiop* const lhsp = VN_AS(nodep->lhsp(), NodeBiop);
        lhsp->unlinkFrBack();
        AstNodeExpr* const shiftp = nodep->rhsp()->unlinkFrBack();
        AstNodeExpr* const ap = lhsp->lhsp()->unlinkFrBack();
        AstNodeExpr* const bp = lhsp->rhsp()->unlinkFrBack();
        AstNodeBiop* const shift1p = nodep;
        AstNodeBiop* const shift2p = nodep->cloneTree(true);
        shift1p->lhsp(ap);
        shift1p->rhsp(shiftp->cloneTreePure(true));
        shift2p->lhsp(bp);
        shift2p->rhsp(shiftp);
        AstNodeBiop* const newp = lhsp;
        newp->lhsp(shift1p);
        newp->rhsp(shift2p);
        newp->dtypeChgWidth(width, widthMin);  // The new AND must have width of the original SHIFT
        handle.relink(newp);
        iterate(newp);  // Further reduce, either node may have more reductions.
    }
    void replaceShiftShift(AstNodeBiop* nodep) {
        UINFO(4, "SHIFT(SHIFT(a,s1),s2)->SHIFT(a,ADD(s1,s2)) " << nodep);
        if (debug() >= 9) nodep->dumpTree("-  repShiftShift_old: ");
        AstNodeBiop* const lhsp = VN_AS(nodep->lhsp(), NodeBiop);
        lhsp->unlinkFrBack();
        AstNodeExpr* const ap = lhsp->lhsp()->unlinkFrBack();
        AstNodeExpr* const shift1p = lhsp->rhsp()->unlinkFrBack();
        AstNodeExpr* const shift2p = nodep->rhsp()->unlinkFrBack();
        // Shift1p and shift2p may have different sizes, both are
        // self-determined so sum with infinite width
        if (nodep->type() == lhsp->type()) {
            const int shift1 = VN_AS(shift1p, Const)->toUInt();
            const int shift2 = VN_AS(shift2p, Const)->toUInt();
            const int newshift = shift1 + shift2;
            VL_DO_DANGLING(pushDeletep(shift1p), shift1p);
            VL_DO_DANGLING(pushDeletep(shift2p), shift2p);
            nodep->lhsp(ap);
            nodep->rhsp(new AstConst(nodep->fileline(), newshift));
            iterate(nodep);  // Further reduce, either node may have more reductions.
        } else {
            // We know shift amounts are constant, but might be a mixed left/right shift
            int shift1 = VN_AS(shift1p, Const)->toUInt();
            if (VN_IS(lhsp, ShiftR)) shift1 = -shift1;
            int shift2 = VN_AS(shift2p, Const)->toUInt();
            if (VN_IS(nodep, ShiftR)) shift2 = -shift2;
            const int newshift = shift1 + shift2;
            VL_DO_DANGLING(pushDeletep(shift1p), shift1p);
            VL_DO_DANGLING(pushDeletep(shift2p), shift2p);
            AstNodeExpr* newp;
            V3Number mask1{nodep, nodep->width()};
            V3Number ones{nodep, nodep->width()};
            ones.setMask(nodep->width());
            if (shift1 < 0) {
                mask1.opShiftR(ones, V3Number(nodep, VL_IDATASIZE, -shift1));
            } else {
                mask1.opShiftL(ones, V3Number(nodep, VL_IDATASIZE, shift1));
            }
            V3Number mask{nodep, nodep->width()};
            if (shift2 < 0) {
                mask.opShiftR(mask1, V3Number(nodep, VL_IDATASIZE, -shift2));
            } else {
                mask.opShiftL(mask1, V3Number(nodep, VL_IDATASIZE, shift2));
            }
            if (newshift < 0) {
                newp = new AstShiftR{nodep->fileline(), ap,
                                     new AstConst(nodep->fileline(), -newshift)};
            } else {
                newp = new AstShiftL{nodep->fileline(), ap,
                                     new AstConst(nodep->fileline(), newshift)};
            }
            newp->dtypeFrom(nodep);
            newp = new AstAnd{nodep->fileline(), newp, new AstConst{nodep->fileline(), mask}};
            nodep->replaceWithKeepDType(newp);
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            // if (debug()) newp->dumpTree("-  repShiftShift_new: ");
            iterate(newp);  // Further reduce, either node may have more reductions.
        }
        VL_DO_DANGLING(pushDeletep(lhsp), lhsp);
    }

    bool replaceAssignMultiSel(AstNodeAssign* nodep) {
        // Multiple assignments to sequential bits can be concated
        // ASSIGN(SEL(a),aq), ASSIGN(SEL(a+1),bq) -> ASSIGN(SEL(a:b),CONCAT(aq,bq)
        // ie. assign var[2]=a, assign var[3]=b -> assign var[3:2]={b,a}

        // Skip if we're not const'ing an entire module (IE doing only one assign, etc)
        if (!m_modp) return false;
        AstSel* const sel1p = VN_CAST(nodep->lhsp(), Sel);
        if (!sel1p) return false;
        AstNodeAssign* const nextp = VN_CAST(nodep->nextp(), NodeAssign);
        if (!nextp) return false;
        if (nodep->type() != nextp->type()) return false;
        AstSel* const sel2p = VN_CAST(nextp->lhsp(), Sel);
        if (!sel2p) return false;
        AstVarRef* const varref1p = VN_CAST(sel1p->fromp(), VarRef);
        if (!varref1p) return false;
        AstVarRef* const varref2p = VN_CAST(sel2p->fromp(), VarRef);
        if (!varref2p) return false;
        if (!varref1p->sameGateTree(varref2p)) return false;
        AstConst* const con1p = VN_CAST(sel1p->lsbp(), Const);
        if (!con1p) return false;
        AstConst* const con2p = VN_CAST(sel2p->lsbp(), Const);
        if (!con2p) return false;
        // We need to make sure there's no self-references involved in either
        // assignment.  For speed, we only look 3 deep, then give up.
        if (!varNotReferenced(nodep->rhsp(), varref1p->varp())) return false;
        if (!varNotReferenced(nextp->rhsp(), varref2p->varp())) return false;
        // If a variable is marked split_var, access to the variable should not be merged.
        if (varref1p->varp()->attrSplitVar() || varref2p->varp()->attrSplitVar()) return false;
        // Swap?
        if ((con1p->toSInt() != con2p->toSInt() + sel2p->width())
            && (con2p->toSInt() != con1p->toSInt() + sel1p->width())) {
            return false;
        }
        const bool lsbFirstAssign = (con1p->toUInt() < con2p->toUInt());
        UINFO(4, "replaceAssignMultiSel " << nodep);
        UINFO(4, "                   && " << nextp);
        // if (debug()) nodep->dumpTree("-  comb1: ");
        // if (debug()) nextp->dumpTree("-  comb2: ");
        AstNodeExpr* const rhs1p = nodep->rhsp()->unlinkFrBack();
        AstNodeExpr* const rhs2p = nextp->rhsp()->unlinkFrBack();
        AstNodeAssign* newp;
        if (lsbFirstAssign) {
            newp = nodep->cloneType(new AstSel{sel1p->fileline(), varref1p->unlinkFrBack(),
                                               sel1p->lsbConst(), sel1p->width() + sel2p->width()},
                                    new AstConcat{rhs1p->fileline(), rhs2p, rhs1p});
        } else {
            newp = nodep->cloneType(new AstSel{sel1p->fileline(), varref1p->unlinkFrBack(),
                                               sel2p->lsbConst(), sel1p->width() + sel2p->width()},
                                    new AstConcat{rhs1p->fileline(), rhs1p, rhs2p});
        }
        // if (debug()) pnewp->dumpTree("-  conew: ");
        nodep->replaceWith(newp);  // dypep intentionally changing
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        VL_DO_DANGLING(pushDeletep(nextp->unlinkFrBack()), nextp);
        return true;
    }

    bool varNotReferenced(AstNode* nodep, AstVar* varp, int level = 0) {
        // Return true if varp never referenced under node.
        // Return false if referenced, or tree too deep to be worth it, or side effects
        if (!nodep) return true;
        if (level > 2) return false;
        if (!nodep->isPure()) return false;  // For example a $fgetc can't be reordered
        if (VN_IS(nodep, NodeVarRef) && VN_AS(nodep, NodeVarRef)->varp() == varp) return false;
        return (varNotReferenced(nodep->nextp(), varp, level + 1)
                && varNotReferenced(nodep->op1p(), varp, level + 1)
                && varNotReferenced(nodep->op2p(), varp, level + 1)
                && varNotReferenced(nodep->op3p(), varp, level + 1)
                && varNotReferenced(nodep->op4p(), varp, level + 1));
    }

    bool replaceNodeAssign(AstNodeAssign* nodep) {
        if (VN_IS(nodep->lhsp(), VarRef) && VN_IS(nodep->rhsp(), VarRef)
            && VN_AS(nodep->lhsp(), VarRef)->sameNoLvalue(VN_AS(nodep->rhsp(), VarRef))
            && !VN_IS(nodep, AssignDly)) {
            // X = X.  Quite pointless, though X <= X may override another earlier assignment
            if (VN_IS(nodep, AssignW)) {
                nodep->v3error("Wire inputs its own output, creating circular logic (wire x=x)");
                return false;  // Don't delete the assign, or V3Gate will freak out
            } else {
                VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
                return true;
            }
        } else if (m_doV && VN_IS(nodep->lhsp(), Concat)) {
            bool need_temp = false;
            bool need_temp_pure = !nodep->rhsp()->isPure();
            if (m_warn && !VN_IS(nodep, AssignDly)
                && !need_temp_pure) {  // Is same var on LHS and RHS?
                // Note only do this (need user4) when m_warn, which is
                // done as unique visitor
                // If the rhs is not pure, we need a temporary variable anyway
                const VNUser4InUse m_inuser4;
                nodep->lhsp()->foreach([](const AstVarRef* nodep) {
                    UASSERT_OBJ(nodep->varp(), nodep, "Unlinked VarRef");
                    nodep->varp()->user4(1);
                });
                nodep->rhsp()->foreach([&need_temp](const AstVarRef* nodep) {
                    UASSERT_OBJ(nodep->varp(), nodep, "Unlinked VarRef");
                    if (nodep->varp()->user4()) need_temp = true;
                });
            }
            if (need_temp_pure) {
                // if the RHS is impure we need to create a temporary variable for it, because
                // further handling involves copying of the RHS.
                UINFO(4, "  ASSITEMPPURE " << nodep);
                // ASSIGN(CONCAT(lc1,lc2),rhs) -> ASSIGN(temp,rhs),
                //                                ASSIGN(lc1,SEL(temp,{size1})),
                //                                ASSIGN(lc2,SEL(temp,{size2}))

                AstNodeExpr* const rhsp = nodep->rhsp()->unlinkFrBack();
                AstVar* const tempPurep = new AstVar{rhsp->fileline(), VVarType::BLOCKTEMP,
                                                     m_concswapNames.get(rhsp), rhsp->dtypep()};
                m_modp->addStmtsp(tempPurep);
                AstVarRef* const tempPureRefp
                    = new AstVarRef{rhsp->fileline(), tempPurep, VAccess::WRITE};
                AstNodeAssign* const asnp
                    = VN_IS(nodep, AssignDly)
                          ? new AstAssign(nodep->fileline(), tempPureRefp, rhsp)
                          : nodep->cloneType(tempPureRefp, rhsp);
                nodep->addHereThisAsNext(asnp);
                nodep->rhsp(new AstVarRef{rhsp->fileline(), tempPurep, VAccess::READ});
            } else if (need_temp) {
                // The first time we constify, there may be the same variable on the LHS
                // and RHS.  In that case, we must use temporaries, or {a,b}={b,a} will break.
                UINFO(4, "  ASSITEMP " << nodep);
                // ASSIGN(CONCAT(lc1,lc2),rhs) -> ASSIGN(temp1,SEL(rhs,{size})),
                //                                ASSIGN(temp2,SEL(newrhs,{size}))
                //                                ASSIGN(lc1,temp1),
                //                                ASSIGN(lc2,temp2)
            } else {
                UINFO(4, "  ASSI " << nodep);
                // ASSIGN(CONCAT(lc1,lc2),rhs) -> ASSIGN(lc1,SEL(rhs,{size})),
                //                                ASSIGN(lc2,SEL(newrhs,{size}))
            }
            if (debug() >= 9) nodep->dumpTree("-  Ass_old: ");
            // Unlink the stuff
            AstNodeExpr* const lc1p = VN_AS(nodep->lhsp(), Concat)->lhsp()->unlinkFrBack();
            AstNodeExpr* const lc2p = VN_AS(nodep->lhsp(), Concat)->rhsp()->unlinkFrBack();
            AstNodeExpr* const conp = VN_AS(nodep->lhsp(), Concat)->unlinkFrBack();
            AstNodeExpr* const rhsp = nodep->rhsp()->unlinkFrBack();
            AstNodeExpr* const rhs2p = rhsp->cloneTreePure(false);
            // Calc widths
            const int lsb2 = 0;
            const int msb2 = lsb2 + lc2p->width() - 1;
            const int lsb1 = msb2 + 1;
            const int msb1 = lsb1 + lc1p->width() - 1;
            UASSERT_OBJ(msb1 == (conp->width() - 1), nodep, "Width calc mismatch");
            // Form ranges
            AstSel* const sel1p = new AstSel{conp->fileline(), rhsp, lsb1, msb1 - lsb1 + 1};
            AstSel* const sel2p = new AstSel{conp->fileline(), rhs2p, lsb2, msb2 - lsb2 + 1};
            // Make new assigns of same flavor as old one
            //*** Not cloneTree; just one node.
            AstNodeAssign* newp = nullptr;
            if (!need_temp) {
                AstNodeAssign* const asn1ap = nodep->cloneType(lc1p, sel1p);
                AstNodeAssign* const asn2ap = nodep->cloneType(lc2p, sel2p);
                asn1ap->dtypeFrom(sel1p);
                asn2ap->dtypeFrom(sel2p);
                newp = AstNode::addNext(newp, asn1ap);
                newp = AstNode::addNext(newp, asn2ap);
            } else {
                UASSERT_OBJ(m_modp, nodep, "Not under module");
                UASSERT_OBJ(m_globalPass, nodep,
                            "Should not reach here when not invoked on whole AstNetlist");
                // We could create just one temp variable, but we'll get better optimization
                // if we make one per term.
                AstVar* const temp1p
                    = new AstVar{sel1p->fileline(), VVarType::BLOCKTEMP,
                                 m_concswapNames.get(sel1p), VFlagLogicPacked{}, msb1 - lsb1 + 1};
                AstVar* const temp2p
                    = new AstVar{sel2p->fileline(), VVarType::BLOCKTEMP,
                                 m_concswapNames.get(sel2p), VFlagLogicPacked{}, msb2 - lsb2 + 1};
                m_modp->addStmtsp(temp1p);
                m_modp->addStmtsp(temp2p);
                AstNodeAssign* const asn1ap = nodep->cloneType(
                    new AstVarRef{sel1p->fileline(), temp1p, VAccess::WRITE}, sel1p);
                AstNodeAssign* const asn2ap = nodep->cloneType(
                    new AstVarRef{sel2p->fileline(), temp2p, VAccess::WRITE}, sel2p);
                AstNodeAssign* const asn1bp = nodep->cloneType(
                    lc1p, new AstVarRef{sel1p->fileline(), temp1p, VAccess::READ});
                AstNodeAssign* const asn2bp = nodep->cloneType(
                    lc2p, new AstVarRef{sel2p->fileline(), temp2p, VAccess::READ});
                asn1ap->dtypeFrom(temp1p);
                asn1bp->dtypeFrom(temp1p);
                asn2ap->dtypeFrom(temp2p);
                asn2bp->dtypeFrom(temp2p);
                // This order matters
                newp = AstNode::addNext(newp, asn1ap);
                newp = AstNode::addNext(newp, asn2ap);
                newp = AstNode::addNext(newp, asn1bp);
                newp = AstNode::addNext(newp, asn2bp);
            }
            if (debug() >= 9 && newp) newp->dumpTreeAndNext(cout, "-     _new: ");
            nodep->addNextHere(newp);
            // Cleanup
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
            VL_DO_DANGLING(pushDeletep(conp), conp);
            // Further reduce, either node may have more reductions.
            return true;
        } else if (m_doV && VN_IS(nodep->rhsp(), StreamR)) {
            // The right-streaming operator on rhs of assignment does not
            // change the order of bits. Eliminate stream but keep its lhsp.
            // Add a cast if needed.
            AstStreamR* const streamp = VN_AS(nodep->rhsp(), StreamR)->unlinkFrBack();
            AstNodeExpr* srcp = streamp->lhsp()->unlinkFrBack();
            AstNodeDType* const srcDTypep = srcp->dtypep()->skipRefp();
            const AstNodeDType* const dstDTypep = nodep->lhsp()->dtypep()->skipRefp();
            if (VN_IS(srcDTypep, QueueDType) || VN_IS(srcDTypep, DynArrayDType)) {
                if (VN_IS(dstDTypep, QueueDType) || VN_IS(dstDTypep, DynArrayDType)) {
                    int srcElementBits = 0;
                    if (AstNodeDType* const elemDtp = srcDTypep->subDTypep()) {
                        srcElementBits = elemDtp->width();
                    }
                    int dstElementBits = 0;
                    if (AstNodeDType* const elemDtp = dstDTypep->subDTypep()) {
                        dstElementBits = elemDtp->width();
                    }
                    srcp = new AstCvtArrayToArray{
                        srcp->fileline(), srcp,          nodep->dtypep(), false, 1,
                        dstElementBits,   srcElementBits};
                } else {
                    srcp = new AstCvtArrayToPacked{srcp->fileline(), srcp, nodep->dtypep()};
                }
            } else if (VN_IS(srcDTypep, UnpackArrayDType)) {
                srcp = new AstCvtArrayToPacked{srcp->fileline(), srcp, srcDTypep};
                // Handling the case where lhs is wider than rhs by inserting zeros. StreamL does
                // not require this, since the left streaming operator implicitly handles this.
                const int packedBits = nodep->lhsp()->widthMin();
                const int unpackBits
                    = srcDTypep->arrayUnpackedElements() * srcDTypep->subDTypep()->widthMin();
                const uint32_t offset = packedBits > unpackBits ? packedBits - unpackBits : 0;
                srcp = new AstShiftL{srcp->fileline(), srcp,
                                     new AstConst{srcp->fileline(), offset}, packedBits};
            }
            nodep->rhsp(srcp);
            VL_DO_DANGLING(pushDeletep(streamp), streamp);
            // Further reduce, any of the nodes may have more reductions.
            return true;
        } else if (m_doV && VN_IS(nodep->lhsp(), StreamL)) {
            // Push the stream operator to the rhs of the assignment statement
            AstNodeExpr* streamp = nodep->lhsp()->unlinkFrBack();
            AstNodeExpr* const dstp = VN_AS(streamp, StreamL)->lhsp()->unlinkFrBack();
            AstNodeDType* const dstDTypep = dstp->dtypep()->skipRefp();
            AstNodeExpr* const srcp = nodep->rhsp()->unlinkFrBack();
            AstNodeDType* const srcDTypep = srcp->dtypep()->skipRefp();
            const int sWidth = srcp->width();
            const int dWidth = dstp->width();
            // Connect the rhs to the stream operator and update its width
            VN_AS(streamp, StreamL)->lhsp(srcp);
            if (VN_IS(srcDTypep, DynArrayDType) || VN_IS(srcDTypep, QueueDType)
                || VN_IS(srcDTypep, UnpackArrayDType)) {
                streamp->dtypeSetStream();
            } else {
                streamp->dtypeSetLogicUnsized(srcp->width(), srcp->widthMin(), VSigning::UNSIGNED);
            }
            if (VN_IS(dstDTypep, UnpackArrayDType)) {
                streamp = new AstCvtPackedToArray{nodep->fileline(), streamp, dstDTypep};
            } else {
                UASSERT(sWidth >= dWidth, "sWidth >= dWidth should have caused an error earlier");
                if (dWidth == 0) {
                    streamp = new AstCvtPackedToArray{nodep->fileline(), streamp, dstDTypep};
                } else if (sWidth >= dWidth) {
                    streamp = new AstSel{streamp->fileline(), streamp, sWidth - dWidth, dWidth};
                }
            }
            nodep->lhsp(dstp);
            nodep->rhsp(streamp);
            nodep->dtypep(dstDTypep);
            return true;
        } else if (m_doV && VN_IS(nodep->lhsp(), StreamR)) {
            // The right stream operator on lhs of assignment statement does
            // not reorder bits. However, if the rhs is wider than the lhs,
            // then we select bits from the left-most, not the right-most.
            AstNodeExpr* const streamp = nodep->lhsp()->unlinkFrBack();
            AstNodeExpr* const dstp = VN_AS(streamp, StreamR)->lhsp()->unlinkFrBack();
            AstNodeDType* const dstDTypep = dstp->dtypep()->skipRefp();
            AstNodeExpr* srcp = nodep->rhsp()->unlinkFrBack();
            const int sWidth = srcp->width();
            const int dWidth = dstp->width();
            if (VN_IS(dstDTypep, UnpackArrayDType)) {
                const int dstBitWidth
                    = dWidth * VN_AS(dstDTypep, UnpackArrayDType)->arrayUnpackedElements();
                // Handling the case where rhs is wider than lhs. StreamL does not require this
                // since the combination of the left streaming operation and the implicit
                // truncation in VL_ASSIGN_UNPACK automatically selects the left-most bits.
                if (sWidth > dstBitWidth) {
                    srcp
                        = new AstSel{streamp->fileline(), srcp, sWidth - dstBitWidth, dstBitWidth};
                }
                srcp = new AstCvtPackedToArray{nodep->fileline(), srcp, dstDTypep};
            } else {
                UASSERT(sWidth >= dWidth, "sWidth >= dWidth should have caused an error earlier");
                if (dWidth == 0) {
                    srcp = new AstCvtPackedToArray{nodep->fileline(), srcp, dstDTypep};
                } else if (sWidth >= dWidth) {
                    srcp = new AstSel{streamp->fileline(), srcp, sWidth - dWidth, dWidth};
                }
            }
            nodep->lhsp(dstp);
            nodep->rhsp(srcp);
            nodep->dtypep(dstDTypep);
            VL_DO_DANGLING(pushDeletep(streamp), streamp);
            // Further reduce, any of the nodes may have more reductions.
            return true;
        } else if (m_doV && VN_IS(nodep->rhsp(), StreamL)) {
            AstStreamL* streamp = VN_AS(nodep->rhsp(), StreamL);
            AstNodeExpr* srcp = streamp->lhsp();
            AstNodeDType* const srcDTypep = srcp->dtypep()->skipRefp();
            AstNodeDType* const dstDTypep = nodep->lhsp()->dtypep()->skipRefp();
            if ((VN_IS(srcDTypep, QueueDType) || VN_IS(srcDTypep, DynArrayDType)
                 || VN_IS(srcDTypep, UnpackArrayDType))) {
                if (VN_IS(dstDTypep, QueueDType) || VN_IS(dstDTypep, DynArrayDType)) {
                    int blockSize = 1;
                    if (const AstConst* const constp = VN_CAST(streamp->rhsp(), Const)) {
                        blockSize = constp->toSInt();
                        if (VL_UNLIKELY(blockSize <= 0)) {
                            // Not reachable due to higher level checks when parsing stream
                            // operators commented out to not fail v3error-coverage-checks.
                            // nodep->v3error("Stream block size must be positive, got " <<
                            // blockSize); nodep->v3error("Stream block size must be positive, got
                            // " << blockSize);
                            blockSize = 1;
                        }
                    }
                    // Not reachable due to higher level checks when parsing stream operators
                    // commented out to not fail v3error-coverage-checks.
                    // else {
                    // nodep->v3error("Stream block size must be constant (got " <<
                    // streamp->rhsp()->prettyTypeName() << ")");
                    // }
                    int srcElementBits = 0;
                    if (AstNodeDType* const elemDtp = srcDTypep->subDTypep()) {
                        srcElementBits = elemDtp->width();
                    }
                    int dstElementBits = 0;
                    if (AstNodeDType* const elemDtp = dstDTypep->subDTypep()) {
                        dstElementBits = elemDtp->width();
                    }
                    streamp->unlinkFrBack();
                    srcp = new AstCvtArrayToArray{
                        srcp->fileline(), srcp->unlinkFrBack(), dstDTypep,     true,
                        blockSize,        dstElementBits,       srcElementBits};
                    nodep->rhsp(srcp);
                    VL_DO_DANGLING(pushDeletep(streamp), streamp);
                } else {
                    streamp->lhsp(new AstCvtArrayToPacked{srcp->fileline(), srcp->unlinkFrBack(),
                                                          dstDTypep});
                    streamp->dtypeFrom(dstDTypep);
                }
            }
        } else if (m_doV && replaceAssignMultiSel(nodep)) {
            return true;
        }
        return false;
    }

    // Boolean replacements
    bool operandBoolShift(const AstNode* nodep) {
        // boolean test of AND(const,SHIFTR(x,const)) -> test of AND(SHIFTL(x,const), x)
        if (!VN_IS(nodep, And)) return false;
        if (!VN_IS(VN_AS(nodep, And)->lhsp(), Const)) return false;
        if (!VN_IS(VN_AS(nodep, And)->rhsp(), ShiftR)) return false;
        const AstShiftR* const shiftp = VN_AS(VN_AS(nodep, And)->rhsp(), ShiftR);
        if (!VN_IS(shiftp->rhsp(), Const)) return false;
        if (static_cast<uint32_t>(nodep->width()) <= VN_AS(shiftp->rhsp(), Const)->toUInt()) {
            return false;
        }
        return true;
    }
    void replaceBoolShift(AstNode* nodep) {
        if (debug() >= 9) nodep->dumpTree("-  bshft_old: ");
        AstConst* const andConstp = VN_AS(VN_AS(nodep, And)->lhsp(), Const);
        AstNodeExpr* const fromp
            = VN_AS(VN_AS(nodep, And)->rhsp(), ShiftR)->lhsp()->unlinkFrBack();
        AstConst* const shiftConstp
            = VN_AS(VN_AS(VN_AS(nodep, And)->rhsp(), ShiftR)->rhsp(), Const);
        V3Number val{andConstp, andConstp->width()};
        val.opShiftL(andConstp->num(), shiftConstp->num());
        AstAnd* const newp
            = new AstAnd{nodep->fileline(), new AstConst{nodep->fileline(), val}, fromp};
        // widthMin no longer applicable if different C-expanded width
        newp->dtypeSetLogicSized(nodep->width(), VSigning::UNSIGNED);
        nodep->replaceWith(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        if (debug() >= 9) newp->dumpTree("-       _new: ");
    }

    void replaceWithSimulation(AstNode* nodep) {
        SimulateVisitor simvis;
        // Run it - may be unoptimizable due to large for loop, etc
        simvis.mainParamEmulate(nodep);
        if (!simvis.optimizable()) {
            const AstNode* errorp = simvis.whyNotNodep();
            if (!errorp) errorp = nodep;
            nodep->v3error("Expecting expression to be constant, but can't determine constant for "
                           << nodep->prettyTypeName() << '\n'
                           << errorp->warnOther() << "... Location of non-constant "
                           << errorp->prettyTypeName() << ": " << simvis.whyNotMessage());
            VL_DO_DANGLING(replaceZero(nodep), nodep);
        } else {
            // Fetch the result
            AstNode* const valuep = simvis.fetchValueNull(nodep);  // valuep is owned by Simulate
            UASSERT_OBJ(valuep, nodep, "No value returned from simulation");
            // Replace it
            AstNode* const newp = valuep->cloneTree(false);
            newp->fileline(nodep->fileline());
            nodep->replaceWithKeepDType(newp);
            UINFO(4, "Simulate->" << newp);
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        }
    }

    //----------------------------------------

    // VISITORS
    void visit(AstNetlist* nodep) override {
        // Iterate modules backwards, in bottom-up order.  That's faster
        iterateChildrenBackwardsConst(nodep);
    }
    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        m_modp = nodep;
        m_concswapNames.reset();
        iterateChildren(nodep);
    }
    void visit(AstCFunc* nodep) override {
        // No ASSIGNW removals under funcs, we've long eliminated INITIALs
        // (We should perhaps rename the assignw's to just assigns)
        VL_RESTORER(m_wremove);
        m_wremove = false;
        iterateChildren(nodep);
    }
    void visit(AstCLocalScope* nodep) override {
        iterateChildren(nodep);
        if (!nodep->stmtsp()) {
            nodep->unlinkFrBack();
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        }
    }
    void visit(AstScope* nodep) override {
        // No ASSIGNW removals under scope, we've long eliminated INITIALs
        VL_RESTORER(m_wremove);
        VL_RESTORER(m_scopep);
        m_wremove = false;
        m_scopep = nodep;
        iterateChildren(nodep);
    }

    void swapSides(AstNodeBiCom* nodep) {
        // COMMUTATIVE({a},CONST) -> COMMUTATIVE(CONST,{a})
        // This simplifies later optimizations
        AstNodeExpr* const lhsp = nodep->lhsp()->unlinkFrBackWithNext();
        AstNodeExpr* const rhsp = nodep->rhsp()->unlinkFrBackWithNext();
        nodep->lhsp(rhsp);
        nodep->rhsp(lhsp);
        iterate(nodep);  // Again?
    }

    bool containsMemberAccessRecurse(const AstNode* const nodep) {
        if (!nodep) return false;
        const auto it = m_containsMemberAccess.lower_bound(nodep);
        if (it != m_containsMemberAccess.end() && it->first == nodep) return it->second;
        bool result = false;
        if (VN_IS(nodep, MemberSel) || VN_IS(nodep, MethodCall) || VN_IS(nodep, CMethodCall)) {
            result = true;
        } else if (const AstNodeFTaskRef* const funcRefp = VN_CAST(nodep, NodeFTaskRef)) {
            if (containsMemberAccessRecurse(funcRefp->taskp())) result = true;
        } else if (const AstNodeCCall* const funcRefp = VN_CAST(nodep, NodeCCall)) {
            if (containsMemberAccessRecurse(funcRefp->funcp())) result = true;
        } else if (const AstNodeFTask* const funcp = VN_CAST(nodep, NodeFTask)) {
            // Assume that it has a member access
            if (funcp->recursive()) result = true;
        } else if (const AstCFunc* const funcp = VN_CAST(nodep, CFunc)) {
            if (funcp->recursive()) result = true;
        }
        if (!result) {
            result = containsMemberAccessRecurse(nodep->op1p())
                     || containsMemberAccessRecurse(nodep->op2p())
                     || containsMemberAccessRecurse(nodep->op3p())
                     || containsMemberAccessRecurse(nodep->op4p());
        }
        if (!result && !VN_IS(nodep, NodeFTask)
            && !VN_IS(nodep, CFunc)  // don't enter into next function
            && containsMemberAccessRecurse(nodep->nextp())) {
            result = true;
        }
        m_containsMemberAccess.insert(it, std::make_pair(nodep, result));
        return result;
    }

    bool matchBiopToBitwise(AstNodeBiop* const nodep) {
        if (!m_convertLogicToBit) return false;
        if (!nodep->lhsp()->width1()) return false;
        if (!nodep->rhsp()->width1()) return false;
        if (!nodep->isPure()) return false;
        if (containsMemberAccessRecurse(nodep)) return false;
        return true;
    }
    bool matchConcatRand(AstConcat* nodep) {
        //    CONCAT(RAND, RAND) - created by Chisel code
        AstRand* const aRandp = VN_CAST(nodep->lhsp(), Rand);
        AstRand* const bRandp = VN_CAST(nodep->rhsp(), Rand);
        if (!aRandp || !bRandp) return false;
        if (!aRandp->combinable(bRandp)) return false;
        UINFO(4, "Concat(Rand,Rand) => Rand: " << nodep);
        nodep->replaceWithKeepDType(aRandp->unlinkFrBack());
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }
    bool matchSelRand(AstSel* nodep) {
        //    SEL(RAND) - created by Chisel code
        AstRand* const aRandp = VN_CAST(nodep->fromp(), Rand);
        if (!aRandp) return false;
        if (aRandp->seedp()) return false;
        UINFO(4, "Sel(Rand) => Rand: " << nodep);
        nodep->replaceWithKeepDType(aRandp->unlinkFrBack());
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }
    int operandConcatMove(AstConcat* nodep) {
        //    CONCAT under concat  (See moveConcat)
        // Return value: true indicates to do it; 2 means move to LHS
        const AstConcat* const abConcp = VN_CAST(nodep->lhsp(), Concat);
        const AstConcat* const bcConcp = VN_CAST(nodep->rhsp(), Concat);
        if (!abConcp && !bcConcp) return 0;
        if (bcConcp) {
            AstNodeExpr* const ap = nodep->lhsp();
            AstNodeExpr* const bp = bcConcp->lhsp();
            // If a+b == 32,64,96 etc, then we want to have a+b together on LHS
            if (VL_BITBIT_I(ap->width() + bp->width()) == 0) return 2;  // Transform 2: to abConc
        } else {  // abConcp
            // Unless lhs is already 32 bits due to above, reorder it
            if (VL_BITBIT_I(nodep->lhsp()->width()) != 0) return 1;  // Transform 1: to bcConc
        }
        return 0;  // ok
    }
    void moveConcat(AstConcat* nodep) {
        //    1: CONCAT(CONCAT({a},{b}),{c})  -> CONCAT({a},CONCAT({b}, {c}))
        // or 2: CONCAT({a}, CONCAT({b},{c})) -> CONCAT(CONCAT({a},{b}),{c})
        // Because the lhs of a concat needs a shift but the rhs doesn't,
        // putting additional CONCATs on the RHS leads to fewer assembler operations.
        // However, we'll end up with lots of wide moves if we make huge trees
        // like that, so on 32 bit boundaries, we'll do the opposite form.
        UINFO(4, "Move concat: " << nodep);
        if (operandConcatMove(nodep) > 1) {
            AstNodeExpr* const ap = nodep->lhsp()->unlinkFrBack();
            AstConcat* const bcConcp = VN_AS(nodep->rhsp(), Concat);
            bcConcp->unlinkFrBack();
            AstNodeExpr* const bp = bcConcp->lhsp()->unlinkFrBack();
            AstNodeExpr* const cp = bcConcp->rhsp()->unlinkFrBack();
            AstConcat* const abConcp = new AstConcat{bcConcp->fileline(), ap, bp};
            nodep->lhsp(abConcp);
            nodep->rhsp(cp);
            // If bp was a concat, then we have this exact same form again!
            // Recurse rather then calling node->iterate to prevent 2^n recursion!
            if (operandConcatMove(abConcp)) moveConcat(abConcp);
            VL_DO_DANGLING(pushDeletep(bcConcp), bcConcp);
        } else {
            AstConcat* const abConcp = VN_AS(nodep->lhsp(), Concat);
            abConcp->unlinkFrBack();
            AstNodeExpr* const ap = abConcp->lhsp()->unlinkFrBack();
            AstNodeExpr* const bp = abConcp->rhsp()->unlinkFrBack();
            AstNodeExpr* const cp = nodep->rhsp()->unlinkFrBack();
            AstConcat* const bcConcp = new AstConcat{abConcp->fileline(), bp, cp};
            nodep->lhsp(ap);
            nodep->rhsp(bcConcp);
            if (operandConcatMove(bcConcp)) moveConcat(bcConcp);
            VL_DO_DANGLING(pushDeletep(abConcp), abConcp);
        }
    }

    // Special cases
    void visit(AstConst*) override {}  // Already constant

    void visit(AstCell* nodep) override {
        if (m_params) {
            iterateAndNextNull(nodep->paramsp());
        } else {
            iterateChildren(nodep);
        }
    }
    void visit(AstClassOrPackageRef* nodep) override { iterateChildren(nodep); }
    void visit(AstPin* nodep) override { iterateChildren(nodep); }

    void replaceLogEq(AstLogEq* nodep) {
        // LOGEQ(a,b) => AstLogAnd{AstLogOr{AstLogNot{a},b},AstLogOr{AstLogNot{b},a}}
        AstNodeExpr* const lhsp = nodep->lhsp()->unlinkFrBack();
        AstNodeExpr* const rhsp = nodep->rhsp()->unlinkFrBack();
        // Do exactly as IEEE says, might result in extra terms, so in future may do differently
        AstLogAnd* const newp = new AstLogAnd{
            nodep->fileline(),
            new AstLogOr{nodep->fileline(), new AstLogNot{nodep->fileline(), lhsp}, rhsp},
            new AstLogOr{nodep->fileline(),
                         new AstLogNot{nodep->fileline(), rhsp->cloneTreePure(false)},
                         lhsp->cloneTreePure(false)}};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    void replaceSelSel(AstSel* nodep) {
        // SEL(SEL({x},a,b),c,d) => SEL({x},a+c,d)
        AstSel* const belowp = VN_AS(nodep->fromp(), Sel);
        AstNodeExpr* const fromp = belowp->fromp()->unlinkFrBack();
        AstNodeExpr* const lsb1p = nodep->lsbp()->unlinkFrBack();
        AstNodeExpr* const lsb2p = belowp->lsbp()->unlinkFrBack();
        // Eliminate lower range
        UINFO(4, "Elim Lower range: " << nodep);
        AstNodeExpr* newlsbp;
        if (VN_IS(lsb1p, Const) && VN_IS(lsb2p, Const)) {
            newlsbp = new AstConst{lsb1p->fileline(),
                                   VN_AS(lsb1p, Const)->toUInt() + VN_AS(lsb2p, Const)->toUInt()};
            VL_DO_DANGLING(pushDeletep(lsb1p), lsb1p);
            VL_DO_DANGLING(pushDeletep(lsb2p), lsb2p);
        } else {
            // Width is important, we need the width of the fromp's expression, not the
            // potentially smaller lsb1p's width, but don't insert a redundant AstExtend.
            // Note that due to some sloppiness in earlier passes, lsb1p might actually be wider,
            // so extend to the wider type.
            AstNodeExpr* const widep = lsb1p->width() > lsb2p->width() ? lsb1p : lsb2p;
            AstNodeExpr* const lhsp = widep->width() > lsb2p->width()
                                          ? new AstExtend{lsb2p->fileline(), lsb2p}
                                          : lsb2p;
            AstNodeExpr* const rhsp = widep->width() > lsb1p->width()
                                          ? new AstExtend{lsb1p->fileline(), lsb1p}
                                          : lsb1p;
            lhsp->dtypeFrom(widep);
            rhsp->dtypeFrom(widep);
            newlsbp = new AstAdd{lsb1p->fileline(), lhsp, rhsp};
            newlsbp->dtypeFrom(widep);
        }
        AstSel* const newp = new AstSel{nodep->fileline(), fromp, newlsbp, nodep->widthConst()};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    void replaceSelConcat(AstSel* nodep) {
        // SEL(CONCAT(a,b),c,d) => SEL(a or b, . .)
        AstConcat* const conp = VN_AS(nodep->fromp(), Concat);
        AstNodeExpr* const conLhsp = conp->lhsp();
        AstNodeExpr* const conRhsp = conp->rhsp();
        if (static_cast<int>(nodep->lsbConst()) >= conRhsp->width()) {
            conLhsp->unlinkFrBack();
            AstSel* const newp
                = new AstSel{nodep->fileline(), conLhsp, nodep->lsbConst() - conRhsp->width(),
                             nodep->widthConst()};
            nodep->replaceWithKeepDType(newp);
        } else if (static_cast<int>(nodep->msbConst()) < conRhsp->width()) {
            conRhsp->unlinkFrBack();
            AstSel* const newp
                = new AstSel{nodep->fileline(), conRhsp, nodep->lsbConst(), nodep->widthConst()};
            nodep->replaceWithKeepDType(newp);
        } else {
            // Yuk, split between the two
            conRhsp->unlinkFrBack();
            conLhsp->unlinkFrBack();
            AstConcat* const newp
                = new AstConcat{nodep->fileline(),
                                new AstSel{nodep->fileline(), conLhsp, 0,
                                           nodep->msbConst() - conRhsp->width() + 1},
                                new AstSel{nodep->fileline(), conRhsp, nodep->lsbConst(),
                                           conRhsp->width() - nodep->lsbConst()}};
            nodep->replaceWithKeepDType(newp);
        }
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    bool operandSelReplicate(AstSel* nodep) {
        // SEL(REPLICATE(from,rep),lsb,width) => SEL(from,0,width) as long
        // as SEL's width <= b's width
        AstReplicate* const repp = VN_AS(nodep->fromp(), Replicate);
        AstNodeExpr* const fromp = repp->srcp();
        AstConst* const lsbp = VN_CAST(nodep->lsbp(), Const);
        if (!lsbp) return false;
        UASSERT_OBJ(fromp->width(), nodep, "Not widthed");
        if ((lsbp->toUInt() / fromp->width())
            != ((lsbp->toUInt() + nodep->width() - 1) / fromp->width())) {
            return false;
        }
        //
        fromp->unlinkFrBack();
        AstSel* const newp = new AstSel{
            nodep->fileline(), fromp,
            new AstConst{lsbp->fileline(), lsbp->toUInt() % fromp->width()}, nodep->widthConst()};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }
    bool operandRepRep(AstReplicate* nodep) {
        // REPLICATE(REPLICATE2(from2,cnt2),cnt1) => REPLICATE(from2,(cnt1+cnt2))
        AstReplicate* const rep2p = VN_AS(nodep->srcp(), Replicate);
        AstNodeExpr* const from2p = rep2p->srcp();
        AstConst* const cnt1p = VN_CAST(nodep->countp(), Const);
        if (!cnt1p) return false;
        AstConst* const cnt2p = VN_CAST(rep2p->countp(), Const);
        if (!cnt2p) return false;
        //
        from2p->unlinkFrBack();
        cnt1p->unlinkFrBack();
        cnt2p->unlinkFrBack();
        AstReplicate* const newp
            = new AstReplicate{nodep->fileline(), from2p, cnt1p->toUInt() * cnt2p->toUInt()};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }
    bool operandConcatSame(AstConcat* nodep) {
        // CONCAT(fromp,fromp) -> REPLICATE(fromp,1+1)
        // CONCAT(REP(fromp,cnt1),fromp) -> REPLICATE(fromp,cnt1+1)
        // CONCAT(fromp,REP(fromp,cnt1)) -> REPLICATE(fromp,1+cnt1)
        // CONCAT(REP(fromp,cnt1),REP(fromp,cnt2)) -> REPLICATE(fromp,cnt1+cnt2)
        AstNodeExpr* from1p = nodep->lhsp();
        uint32_t cnt1 = 1;
        AstNodeExpr* from2p = nodep->rhsp();
        uint32_t cnt2 = 1;
        if (VN_IS(from1p, Replicate)) {
            AstConst* const cnt1p = VN_CAST(VN_CAST(from1p, Replicate)->countp(), Const);
            if (!cnt1p) return false;
            from1p = VN_AS(from1p, Replicate)->srcp();
            cnt1 = cnt1p->toUInt();
        }
        if (VN_IS(from2p, Replicate)) {
            AstConst* const cnt2p = VN_CAST(VN_CAST(from2p, Replicate)->countp(), Const);
            if (!cnt2p) return false;
            from2p = VN_AS(from2p, Replicate)->srcp();
            cnt2 = cnt2p->toUInt();
        }
        if (!operandsSame(from1p, from2p)) return false;
        //
        from1p->unlinkFrBack();
        AstReplicate* const newp = new AstReplicate{nodep->fileline(), from1p, cnt1 + cnt2};
        nodep->replaceWithKeepDType(newp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
        return true;
    }
    void replaceSelIntoBiop(AstSel* nodep) {
        // SEL(BUFIF1(a,b),1,bit) => BUFIF1(SEL(a,1,bit),SEL(b,1,bit))
        AstNodeBiop* const fromp = VN_AS(nodep->fromp()->unlinkFrBack(), NodeBiop);
        UASSERT_OBJ(fromp, nodep, "Called on non biop");
        AstNodeExpr* const lsbp = nodep->lsbp()->unlinkFrBack();
        //
        AstNodeExpr* const bilhsp = fromp->lhsp()->unlinkFrBack();
        AstNodeExpr* const birhsp = fromp->rhsp()->unlinkFrBack();
        //
        fromp->lhsp(
            new AstSel{nodep->fileline(), bilhsp, lsbp->cloneTreePure(true), nodep->widthConst()});
        fromp->rhsp(new AstSel{nodep->fileline(), birhsp, lsbp, nodep->widthConst()});
        nodep->replaceWithKeepDType(fromp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void replaceSelIntoUniop(AstSel* nodep) {
        // SEL(NOT(a),1,bit) => NOT(SEL(a,bit))
        AstNodeUniop* const fromp = VN_AS(nodep->fromp()->unlinkFrBack(), NodeUniop);
        UASSERT_OBJ(fromp, nodep, "Called on non biop");
        AstNodeExpr* const lsbp = nodep->lsbp()->unlinkFrBack();
        //
        AstNodeExpr* const bilhsp = fromp->lhsp()->unlinkFrBack();
        //
        fromp->lhsp(new AstSel{nodep->fileline(), bilhsp, lsbp, nodep->widthConst()});
        nodep->replaceWithKeepDType(fromp);
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }

    void visit(AstAttrOf* nodep) override {
        VL_RESTORER(m_attrp);
        m_attrp = nodep;
        iterateChildren(nodep);
    }

    void visit(AstArraySel* nodep) override {
        iterateAndNextNull(nodep->bitp());
        if (VN_IS(nodep->bitp(), Const)
            && VN_IS(nodep->fromp(), VarRef)
            // Need to make sure it's an array object so don't mis-allow a constant (bug509.)
            && VN_AS(nodep->fromp(), VarRef)->varp()
            && VN_IS(VN_AS(nodep->fromp(), VarRef)->varp()->valuep(), InitArray)) {
            m_selp = nodep;  // Ask visit(AstVarRef) to replace varref with const
        }
        iterateAndNextNull(nodep->fromp());
        if (VN_IS(nodep->fromp(), Const)) {  // It did.
            if (!m_selp) {
                nodep->v3error("Illegal assignment of constant to unpacked array");
            } else {
                AstNode* const fromp = nodep->fromp()->unlinkFrBack();
                nodep->replaceWithKeepDType(fromp);
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            }
        }
        m_selp = nullptr;
    }
    void visit(AstCAwait* nodep) override {
        m_hasJumpDelay = true;
        iterateChildren(nodep);
    }
    void visit(AstNodeVarRef* nodep) override {
        iterateChildren(nodep);
        UASSERT_OBJ(nodep->varp(), nodep, "Not linked");
        bool did = false;
        if (m_doV && nodep->varp()->valuep() && !m_attrp) {
            // if (debug()) valuep->dumpTree("-  visitvaref: ");
            iterateAndNextNull(nodep->varp()->valuep());  // May change nodep->varp()->valuep()
            AstNode* const valuep = nodep->varp()->valuep();
            if (nodep->access().isReadOnly()
                && ((!m_params  // Can reduce constant wires into equations
                     && m_doNConst
                     && v3Global.opt.fConst()
                     // Default value, not a "known" constant for this usage
                     && !nodep->varp()->isClassMember() && !nodep->varp()->sensIfacep()
                     && !(nodep->varp()->isFuncLocal() && nodep->varp()->isNonOutput())
                     && !nodep->varp()->noSubst() && !nodep->varp()->isSigPublic())
                    || nodep->varp()->isParam())) {
                if (operandConst(valuep)) {
                    const V3Number& num = VN_AS(valuep, Const)->num();
                    // UINFO(2, "constVisit " << cvtToHex(valuep) << " " << num);
                    VL_DO_DANGLING(replaceNum(nodep, num), nodep);
                    did = true;
                } else if (m_selp && VN_IS(valuep, InitArray)) {
                    AstInitArray* const initarp = VN_AS(valuep, InitArray);
                    const uint32_t bit = m_selp->bitConst();
                    AstNode* const itemp = initarp->getIndexDefaultedValuep(bit);
                    if (VN_IS(itemp, Const)) {
                        const V3Number& num = VN_AS(itemp, Const)->num();
                        // UINFO(2, "constVisit " << cvtToHex(valuep) << " " << num);
                        VL_DO_DANGLING(replaceNum(nodep, num), nodep);
                        did = true;
                    }
                } else if (m_params && VN_IS(valuep, InitArray)) {
                    // Allow parameters to pass arrays
                    // Earlier recursion of InitArray made sure each array value is constant
                    // This exception is fairly fragile, i.e. doesn't
                    // support arrays of arrays or other stuff
                    AstNode* const newp = valuep->cloneTree(false);
                    nodep->replaceWithKeepDType(newp);
                    VL_DO_DANGLING(pushDeletep(nodep), nodep);
                    did = true;
                } else if (nodep->varp()->isParam() && VN_IS(valuep, Unbounded)) {
                    AstNode* const newp = valuep->cloneTree(false);
                    nodep->replaceWithKeepDType(newp);
                    VL_DO_DANGLING(pushDeletep(nodep), nodep);
                    did = true;
                }
            }
        }
        if (!did && m_required) {
            nodep->v3error("Expecting expression to be constant, but variable isn't const: "
                           << nodep->varp()->prettyNameQ());
        }
    }
    void visit(AstExprStmt* nodep) override {
        iterateChildren(nodep);
        if (!AstNode::afterCommentp(nodep->stmtsp())) {
            UINFO(8, "ExprStmt(...) " << nodep << " " << nodep->resultp());
            nodep->replaceWith(nodep->resultp()->unlinkFrBack());
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            // Removing the ExprStmt might have made something impure above now pure
        }
    }
    void visit(AstEnumItemRef* nodep) override {
        iterateChildren(nodep);
        UASSERT_OBJ(nodep->itemp(), nodep, "Not linked");
        bool did = false;
        if (nodep->itemp()->valuep()) {
            // if (debug()) nodep->itemp()->valuep()->dumpTree("-  visitvaref: ");
            if (nodep->itemp()->user4()) {
                nodep->v3error("Recursive enum value: " << nodep->itemp()->prettyNameQ());
            } else {
                nodep->itemp()->user4(true);
                iterateAndNextNull(nodep->itemp()->valuep());
                nodep->itemp()->user4(false);
            }
            if (AstConst* const valuep = VN_CAST(nodep->itemp()->valuep(), Const)) {
                const V3Number& num = valuep->num();
                VL_DO_DANGLING(replaceNum(nodep, num), nodep);
                did = true;
            }
        }
        if (!did && m_required) {
            nodep->v3error("Expecting expression to be constant, but enum value isn't const: "
                           << nodep->itemp()->prettyNameQ());
        }
    }

    //  void visit(AstCvtPackString* nodep) override {
    // Not constant propagated (for today) because AstNodeExpr::isOpaque is set
    // Someday if lower is constant, convert to quoted "string".

    bool onlySenItemInSenTree(AstSenItem* nodep) {
        // Only one if it's not in a list
        return (!nodep->nextp() && nodep->backp()->nextp() != nodep);
    }
    void visit(AstSenItem* nodep) override {
        iterateChildren(nodep);
        if (m_doNConst
            && !v3Global.opt.timing().isSetTrue()  // If --timing, V3Sched would turn this into an
                                                   // infinite loop. See #5080
            && (VN_IS(nodep->sensp(), Const) || VN_IS(nodep->sensp(), EnumItemRef)
                || (nodep->varrefp() && nodep->varrefp()->varp()->isParam()))) {
            // Constants in sensitivity lists may be removed (we'll simplify later)
            if (nodep->isClocked()) {  // A constant can never get a pos/negedge
                if (onlySenItemInSenTree(nodep)) {
                    if (nodep->edgeType() == VEdgeType::ET_CHANGED) {
                        // TODO: This really is dodgy, as strictly compliant simulators will not
                        //       execute this block, but but t_func_check relies on it
                        nodep->replaceWith(
                            new AstSenItem{nodep->fileline(), AstSenItem::Initial{}});
                    } else {
                        nodep->replaceWith(new AstSenItem{nodep->fileline(), AstSenItem::Never{}});
                    }
                    VL_DO_DANGLING(pushDeletep(nodep), nodep);
                } else {
                    VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
                }
            } else {  // Otherwise it may compute a result that needs to settle out
                nodep->replaceWith(new AstSenItem{nodep->fileline(), AstSenItem::Combo{}});
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            }
        } else if (m_doNConst && VN_IS(nodep->sensp(), Not)) {
            // V3Gate may propagate NOTs into clocks... Just deal with it
            AstNode* const sensp = nodep->sensp();
            AstNode* lastSensp = sensp;
            bool invert = false;
            while (VN_IS(lastSensp, Not)) {
                lastSensp = VN_AS(lastSensp, Not)->lhsp();
                invert = !invert;
            }
            UINFO(8, "senItem(NOT...) " << nodep << " " << invert);
            if (invert) nodep->edgeType(nodep->edgeType().invert());
            sensp->replaceWith(lastSensp->unlinkFrBack());
            VL_DO_DANGLING(pushDeletep(sensp), sensp);
        }
    }

    class SenItemCmp final {
        static int cmp(const AstNodeExpr* ap, const AstNodeExpr* bp) {
            const VNType aType = ap->type();
            const VNType bType = bp->type();
            if (aType != bType) return static_cast<int>(bType) - static_cast<int>(aType);

            if (const AstVarRef* const aRefp = VN_CAST(ap, VarRef)) {
                const AstVarRef* const bRefp = VN_AS(bp, VarRef);
                // Looks visually better if we keep sorted by name
                if (aRefp->name() < bRefp->name()) return -1;
                if (aRefp->name() > bRefp->name()) return 1;
                // But might be same name with different scopes
                if (aRefp->varScopep() < bRefp->varScopep()) return -1;
                if (aRefp->varScopep() > bRefp->varScopep()) return 1;
                // Or rarely, different data types
                if (aRefp->dtypep() < bRefp->dtypep()) return -1;
                if (aRefp->dtypep() > bRefp->dtypep()) return 1;
                return 0;
            }

            if (const AstConst* const aConstp = VN_CAST(ap, Const)) {
                const AstConst* const bConstp = VN_AS(bp, Const);
                if (aConstp->num().isLtXZ(bConstp->num())) return -1;
                if (bConstp->num().isLtXZ(aConstp->num())) return 1;
                return 0;
            }

            if (const AstNodeBiop* const aBiOpp = VN_CAST(ap, NodeBiop)) {
                const AstNodeBiop* const bBiOpp = VN_AS(bp, NodeBiop);
                // Compare RHSs first as LHS might be const, but the variable term should become
                // adjacent for optimization if identical.
                if (const int c = cmp(aBiOpp->rhsp(), bBiOpp->rhsp())) return c;
                return cmp(aBiOpp->lhsp(), bBiOpp->lhsp());
            }

            if (const AstCMethodHard* const aCallp = VN_CAST(ap, CMethodHard)) {
                const AstCMethodHard* const bCallp = VN_AS(bp, CMethodHard);
                if (aCallp->name() < bCallp->name()) return -1;
                if (aCallp->name() > bCallp->name()) return 1;
                if (const int c = cmp(aCallp->fromp(), bCallp->fromp())) return c;
                AstNodeExpr* aPinsp = aCallp->pinsp();
                AstNodeExpr* bPinsp = bCallp->pinsp();
                while (aPinsp && bPinsp) {
                    if (const int c = cmp(aPinsp, bPinsp)) return c;
                    aPinsp = VN_AS(aPinsp->nextp(), NodeExpr);
                    bPinsp = VN_AS(bPinsp->nextp(), NodeExpr);
                }
                return aPinsp ? -1 : bPinsp ? 1 : 0;
            }

            return 0;
        }

    public:
        bool operator()(const AstSenItem* lhsp, const AstSenItem* rhsp) const {
            AstNodeExpr* const lSensp = lhsp->sensp();
            AstNodeExpr* const rSensp = rhsp->sensp();
            if (lSensp && rSensp) {
                // If both terms have sensitivity expressions, recursively compare them
                if (const int c = cmp(lSensp, rSensp)) return c < 0;
            } else if (lSensp || rSensp) {
                // Terms with sensitivity expressions come after those without
                return rSensp;
            }
            // Finally sort by edge, AFTER variable, as we want multiple edges for same var
            // adjacent. note the SenTree optimizer requires this order (more general first,
            // less general last)
            return lhsp->edgeType() < rhsp->edgeType();
        }
    };

    void visit(AstSenTree* nodep) override {
        iterateChildren(nodep);
        if (m_doExpensive) {
            // if (debug()) nodep->dumpTree("-  ssin: ");
            // Optimize ideas for the future:
            //   SENTREE(... SENGATE(x,a), SENGATE(SENITEM(x),b) ...)  => SENGATE(x,OR(a,b))

            //   SENTREE(... SENITEM(x),   SENGATE(SENITEM(x),*) ...)  => SENITEM(x)
            // Do we need the SENITEM's to be identical?  No because we're
            // ORing between them; we just need to ensure that the result is at
            // least as frequently activating.  So we
            // SENGATE(SENITEM(x)) -> SENITEM(x), then let it collapse with the
            // other SENITEM(x).

            // Mark x in SENITEM(x)
            for (AstSenItem* senp = nodep->sensesp(); senp; senp = VN_AS(senp->nextp(), SenItem)) {
                if (senp->varrefp() && senp->varrefp()->varScopep()) {
                    senp->varrefp()->varScopep()->user4(1);
                }
            }

            // Pass 1: Sort the sensitivity items so "posedge a or b" and "posedge b or a" and
            // similar, optimizable expressions end up next to each other.
            for (AstSenItem *nextp, *senp = nodep->sensesp(); senp; senp = nextp) {
                nextp = VN_AS(senp->nextp(), SenItem);
                // cppcheck-suppress unassignedVariable  // cppcheck bug
                const SenItemCmp cmp;
                if (nextp && !cmp(senp, nextp)) {
                    // Something's out of order, sort it
                    senp = nullptr;
                    std::vector<AstSenItem*> vec;
                    for (AstSenItem* senp = nodep->sensesp(); senp;
                         senp = VN_AS(senp->nextp(), SenItem)) {
                        vec.push_back(senp);
                    }
                    stable_sort(vec.begin(), vec.end(), SenItemCmp());
                    for (const auto& ip : vec) ip->unlinkFrBack();
                    for (const auto& ip : vec) nodep->addSensesp(ip);
                    break;
                }
            }

            // Pass 2, remove duplicates and simplify adjacent terms if possible
            for (AstSenItem *senp = nodep->sensesp(), *nextp; senp; senp = nextp) {
                nextp = VN_AS(senp->nextp(), SenItem);
                if (!nextp) break;
                AstSenItem* const lItemp = senp;
                AstSenItem* const rItemp = nextp;
                AstNodeExpr* const lSenp = lItemp->sensp();
                AstNodeExpr* const rSenp = rItemp->sensp();
                if (!lSenp || !rSenp) continue;

                if (lSenp->sameGateTree(rSenp)) {
                    // POSEDGE or NEGEDGE -> BOTHEDGE. (We've sorted POSEDGE, before NEGEDGE, so we
                    // do not need to test for the opposite orders.)
                    if (lItemp->edgeType() == VEdgeType::ET_POSEDGE
                        && rItemp->edgeType() == VEdgeType::ET_NEGEDGE) {
                        // Make both terms BOTHEDGE, the second will be removed below
                        lItemp->edgeType(VEdgeType::ET_BOTHEDGE);
                        rItemp->edgeType(VEdgeType::ET_BOTHEDGE);
                    }

                    // Remove identical expressions
                    if (lItemp->edgeType() == rItemp->edgeType()) {
                        VL_DO_DANGLING(pushDeletep(rItemp->unlinkFrBack()), rItemp);
                        nextp = lItemp;
                    }

                    continue;
                }

                // Not identical terms, check if they can be combined
                if (lSenp->width() != rSenp->width()) continue;
                if (AstAnd* const lAndp = VN_CAST(lSenp, And)) {
                    if (AstAnd* const rAndp = VN_CAST(rSenp, And)) {
                        if (AstConst* const lConstp = VN_CAST(lAndp->lhsp(), Const)) {
                            if (AstConst* const rConstp = VN_CAST(rAndp->lhsp(), Const)) {
                                if (lAndp->rhsp()->sameTree(rAndp->rhsp())) {
                                    const V3Number lNum{lConstp->num()};
                                    lConstp->num().opOr(lNum, rConstp->num());
                                    // Remove redundant term
                                    VL_DO_DANGLING(pushDeletep(rItemp->unlinkFrBack()), rItemp);
                                    nextp = lItemp;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    //-----
    // Zero elimination
    void visit(AstNodeAssign* nodep) override {
        iterateChildren(nodep);
        if (nodep->timingControlp()) m_hasJumpDelay = true;
        if (m_doNConst && replaceNodeAssign(nodep)) return;
    }
    void visit(AstAssignAlias* nodep) override {
        // Don't perform any optimizations, keep the alias around
    }
    void visit(AstAssignVarScope* nodep) override {
        // Don't perform any optimizations, the node won't be linked yet
    }
    void visit(AstAssignW* nodep) override {
        iterateChildren(nodep);
        if (m_doNConst && replaceNodeAssign(nodep)) return;
        AstNodeVarRef* const varrefp = VN_CAST(
            nodep->lhsp(),
            VarRef);  // Not VarXRef, as different refs may set different values to each hierarchy
        if (m_wremove && !m_params && m_doNConst && m_modp && operandConst(nodep->rhsp())
            && !VN_AS(nodep->rhsp(), Const)->num().isFourState()
            && varrefp  // Don't do messes with BITREFs/ARRAYREFs
            && !varrefp->varp()->hasStrengthAssignment()  // Strengths are resolved in V3Tristate
            && !varrefp->varp()->valuep()  // Not already constified
            && !varrefp->varScopep()  // Not scoped (or each scope may have different initial val.)
            && !varrefp->varp()->isForced()  // Not forced (not really a constant)
        ) {
            // ASSIGNW (VARREF, const) -> INITIAL ( ASSIGN (VARREF, const) )
            UINFO(4, "constAssignW " << nodep);
            // Make a initial assignment
            AstNodeExpr* const exprp = nodep->rhsp()->unlinkFrBack();
            varrefp->unlinkFrBack();
            AstInitial* const newinitp = new AstInitial{
                nodep->fileline(), new AstAssign{nodep->fileline(), varrefp, exprp}};
            nodep->replaceWith(newinitp);
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            // Set the initial value right in the variable so we can constant propagate
            AstNode* const initvaluep = exprp->cloneTree(false);
            varrefp->varp()->valuep(initvaluep);
        }
    }
    void visit(AstCvtArrayToArray* nodep) override {
        iterateChildren(nodep);
        // Handle the case where we have a stream operation inside a cast conversion
        // To avoid infinite recursion, mark the node as processed by setting user1.
        if (!nodep->user1()) {
            nodep->user1(true);
            // Check for both StreamL and StreamR operations
            AstNodeStream* streamp = nullptr;
            bool isReverse = false;
            if (AstStreamL* const streamLp = VN_CAST(nodep->fromp(), StreamL)) {
                streamp = streamLp;
                isReverse = true;  // StreamL reverses the operation
            } else if (AstStreamR* const streamRp = VN_CAST(nodep->fromp(), StreamR)) {
                streamp = streamRp;
                isReverse = false;  // StreamR doesn't reverse the operation
            }
            if (streamp) {
                AstNodeExpr* srcp = streamp->lhsp();
                AstNodeDType* const srcDTypep = srcp->dtypep()->skipRefp();
                AstNodeDType* const dstDTypep = nodep->dtypep()->skipRefp();
                if (VN_IS(srcDTypep, QueueDType) && VN_IS(dstDTypep, QueueDType)) {
                    int blockSize = 1;
                    if (AstConst* const constp = VN_CAST(streamp->rhsp(), Const)) {
                        blockSize = constp->toSInt();
                        if (VL_UNLIKELY(blockSize <= 0)) {
                            // Not reachable due to higher level checks when parsing stream
                            // operators commented out to not fail v3error-coverage-checks.
                            // nodep->v3error("Stream block size must be positive, got " <<
                            // blockSize);
                            blockSize = 1;
                        }
                    }
                    // Not reachable due to higher level checks when parsing stream operators
                    // commented out to not fail v3error-coverage-checks.
                    // else {
                    //    nodep->v3error("Stream block size must be constant (got " <<
                    //    streamp->rhsp()->prettyTypeName() << ")");
                    // }
                    int srcElementBits = 0;
                    if (AstNodeDType* const elemDtp = srcDTypep->subDTypep()) {
                        srcElementBits = elemDtp->width();
                    }
                    int dstElementBits = 0;
                    if (AstNodeDType* const elemDtp = dstDTypep->subDTypep()) {
                        dstElementBits = elemDtp->width();
                    }
                    streamp->unlinkFrBack();
                    AstNodeExpr* newp = new AstCvtArrayToArray{
                        srcp->fileline(), srcp->unlinkFrBack(), dstDTypep,     isReverse,
                        blockSize,        dstElementBits,       srcElementBits};
                    nodep->replaceWith(newp);
                    VL_DO_DANGLING(pushDeletep(streamp), streamp);
                    VL_DO_DANGLING(pushDeletep(nodep), nodep);
                    return;
                }
            }
        }
    }
    void visit(AstRelease* nodep) override {
        if (AstConcat* const concatp = VN_CAST(nodep->lhsp(), Concat)) {
            FileLine* const flp = nodep->fileline();
            AstRelease* const newLp = new AstRelease{flp, concatp->lhsp()->unlinkFrBack()};
            AstRelease* const newRp = new AstRelease{flp, concatp->rhsp()->unlinkFrBack()};
            nodep->replaceWith(newLp);
            newLp->addNextHere(newRp);
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            visit(newLp);
            visit(newRp);
        }
    }

    void visit(AstNodeIf* nodep) override {
        iterateChildren(nodep);
        if (m_doNConst) {
            if (const AstConst* const constp = VN_CAST(nodep->condp(), Const)) {
                AstNode* keepp = nullptr;
                if (constp->isZero()) {
                    UINFO(4, "IF(0,{any},{x}) => {x}: " << nodep);
                    keepp = nodep->elsesp();
                } else if (!m_doV || constp->isNeqZero()) {  // Might be X in Verilog
                    UINFO(4, "IF(!0,{x},{any}) => {x}: " << nodep);
                    keepp = nodep->thensp();
                } else {
                    UINFO(4, "IF condition is X, retaining: " << nodep);
                    return;
                }
                if (keepp) {
                    keepp->unlinkFrBackWithNext();
                    nodep->replaceWith(keepp);
                } else {
                    nodep->unlinkFrBack();
                }
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            } else if (!AstNode::afterCommentp(nodep->thensp())
                       && !AstNode::afterCommentp(nodep->elsesp())) {
                if (!nodep->condp()->isPure()) {
                    // Condition has side effect - leave - perhaps in
                    // future simplify to remove all but side effect terms
                } else {
                    // Empty block, remove it
                    VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
                }
            } else if (!AstNode::afterCommentp(nodep->thensp())) {
                UINFO(4, "IF({x}) nullptr {...} => IF(NOT{x}}: " << nodep);
                AstNodeExpr* const condp = nodep->condp();
                AstNode* const elsesp = nodep->elsesp();
                condp->unlinkFrBackWithNext();
                elsesp->unlinkFrBackWithNext();
                if (nodep->thensp()) {  // Must have been comment
                    pushDeletep(nodep->thensp()->unlinkFrBackWithNext());
                }
                nodep->condp(new AstLogNot{condp->fileline(),
                                           condp});  // LogNot, as C++ optimization also possible
                nodep->addThensp(elsesp);
            } else if (((VN_IS(nodep->condp(), Not) && nodep->condp()->width() == 1)
                        || VN_IS(nodep->condp(), LogNot))
                       && nodep->thensp() && nodep->elsesp()) {
                UINFO(4, "IF(NOT {x})  => IF(x) swapped if/else" << nodep);
                AstNodeExpr* const condp
                    = VN_AS(nodep->condp(), NodeUniop)->lhsp()->unlinkFrBackWithNext();
                AstNode* const thensp = nodep->thensp()->unlinkFrBackWithNext();
                AstNode* const elsesp = nodep->elsesp()->unlinkFrBackWithNext();
                AstIf* const ifp = new AstIf{nodep->fileline(), condp, elsesp, thensp};
                ifp->isBoundsCheck(nodep->isBoundsCheck());  // Copy bounds check info
                ifp->branchPred(nodep->branchPred().invert());
                nodep->replaceWith(ifp);
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            } else if (ifSameAssign(nodep)) {
                UINFO(4,
                      "IF({a}) ASSIGN({b},{c}) else ASSIGN({b},{d}) => ASSIGN({b}, {a}?{c}:{d})");
                AstNodeAssign* const thensp = VN_AS(nodep->thensp(), NodeAssign);
                AstNodeAssign* const elsesp = VN_AS(nodep->elsesp(), NodeAssign);
                thensp->unlinkFrBack();
                AstNodeExpr* const condp = nodep->condp()->unlinkFrBack();
                AstNodeExpr* const truep = thensp->rhsp()->unlinkFrBack();
                AstNodeExpr* const falsep = elsesp->rhsp()->unlinkFrBack();
                thensp->rhsp(new AstCond{truep->fileline(), condp, truep, falsep});
                nodep->replaceWith(thensp);
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            } else if (false  // Disabled, as vpm assertions are faster
                              // without due to short-circuiting
                       && operandIfIf(nodep)) {
                UINFO(9, "IF({a}) IF({b}) => IF({a} && {b})");
                AstNodeIf* const lowerIfp = VN_AS(nodep->thensp(), NodeIf);
                AstNodeExpr* const condp = nodep->condp()->unlinkFrBack();
                AstNode* const lowerThensp = lowerIfp->thensp()->unlinkFrBackWithNext();
                AstNodeExpr* const lowerCondp = lowerIfp->condp()->unlinkFrBackWithNext();
                nodep->condp(new AstLogAnd{lowerIfp->fileline(), condp, lowerCondp});
                lowerIfp->replaceWith(lowerThensp);
                VL_DO_DANGLING(pushDeletep(lowerIfp), lowerIfp);
            } else {
                // Optimizations that don't reform the IF itself
                if (operandBoolShift(nodep->condp())) replaceBoolShift(nodep->condp());
            }
        }
    }

    void visit(AstDisplay* nodep) override {
        // DISPLAY(SFORMAT(text1)),DISPLAY(SFORMAT(text2)) -> DISPLAY(SFORMAT(text1+text2))
        iterateChildren(nodep);
        if (stmtDisplayDisplay(nodep)) return;
    }
    bool stmtDisplayDisplay(AstDisplay* nodep) {
        // DISPLAY(SFORMAT(text1)),DISPLAY(SFORMAT(text2)) -> DISPLAY(SFORMAT(text1+text2))
        if (!m_modp) return false;  // Don't optimize under single statement
        AstDisplay* const prevp = VN_CAST(nodep->backp(), Display);
        if (!prevp) return false;
        if (!((prevp->displayType() == nodep->displayType())
              || (prevp->displayType() == VDisplayType::DT_WRITE
                  && nodep->displayType() == VDisplayType::DT_DISPLAY)
              || (prevp->displayType() == VDisplayType::DT_DISPLAY
                  && nodep->displayType() == VDisplayType::DT_WRITE)))
            return false;
        if ((prevp->filep() && !nodep->filep()) || (!prevp->filep() && nodep->filep())
            || (prevp->filep() && nodep->filep() && !prevp->filep()->sameTree(nodep->filep())))
            return false;
        if (!prevp->fmtp() || prevp->fmtp()->nextp() || !nodep->fmtp() || nodep->fmtp()->nextp())
            return false;
        AstSFormatF* const pformatp = prevp->fmtp();
        if (!pformatp) return false;
        AstSFormatF* const nformatp = nodep->fmtp();
        if (!nformatp) return false;
        // We don't merge scopeNames as can have only one and might be different scopes (late in
        // process) Also rare for real code to print %m multiple times in same message
        if (nformatp->scopeNamep() && pformatp->scopeNamep()) return false;
        // We don't early merge arguments as might need to later print warnings with
        // right line numbers, nor scopeNames as might be different scopes (late in process)
        if (!m_doCpp && pformatp->exprsp()) return false;
        if (!m_doCpp && nformatp->exprsp()) return false;
        if (pformatp->exprsp() && !pformatp->exprsp()->isPureAndNext()) return false;
        if (nformatp->exprsp() && !nformatp->exprsp()->isPureAndNext()) return false;
        // Avoid huge merges
        static constexpr int DISPLAY_MAX_MERGE_LENGTH = 500;
        if (pformatp->text().length() + nformatp->text().length() > DISPLAY_MAX_MERGE_LENGTH)
            return false;
        //
        UINFO(9, "DISPLAY(SF({a})) DISPLAY(SF({b})) -> DISPLAY(SF({a}+{b}))");
        // Convert DT_DISPLAY to DT_WRITE as may allow later optimizations
        if (prevp->displayType() == VDisplayType::DT_DISPLAY) {
            prevp->displayType(VDisplayType::DT_WRITE);
            pformatp->text(pformatp->text() + "\n");
        }
        // We can't replace prev() as the edit tracking iterators will get confused.
        // So instead we edit the prev note itself.
        if (prevp->addNewline()) pformatp->text(pformatp->text() + "\n");
        pformatp->text(pformatp->text() + nformatp->text());
        if (!prevp->addNewline() && nodep->addNewline()) pformatp->text(pformatp->text() + "\n");
        if (nformatp->exprsp()) pformatp->addExprsp(nformatp->exprsp()->unlinkFrBackWithNext());
        if (AstScopeName* const scopeNamep = nformatp->scopeNamep()) {
            scopeNamep->unlinkFrBackWithNext();
            pformatp->scopeNamep(scopeNamep);
        }
        VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
        return true;
    }
    void visit(AstSFormatF* nodep) override {
        // Substitute constants into displays.  The main point of this is to
        // simplify assertion methodologies which call functions with display's.
        // This eliminates a pile of wide temps, and makes the C a whole lot more readable.
        iterateChildren(nodep);
        bool anyconst = false;
        for (AstNode* argp = nodep->exprsp(); argp; argp = argp->nextp()) {
            if (VN_IS(argp, Const)) {
                anyconst = true;
                break;
            }
        }
        if (m_doNConst && anyconst) {
            // UINFO(9, "  Display in  " << nodep->text());
            string newFormat;
            string fmt;
            bool inPct = false;
            AstNode* argp = nodep->exprsp();
            const string text = nodep->text();
            for (const char ch : text) {
                if (!inPct && ch == '%') {
                    inPct = true;
                    fmt = ch;
                } else if (inPct && (std::isdigit(ch) || ch == '.' || ch == '-')) {
                    fmt += ch;
                } else if (inPct) {
                    inPct = false;
                    fmt += ch;
                    switch (std::tolower(ch)) {
                    case '%': break;  // %% - still %%
                    case 'm': break;  // %m - still %m - auto insert "name"
                    case 'l': break;  // %l - still %l - auto insert "library"
                    case 't':  // FALLTHRU
                    case '^':  // %t/%^ - don't know $timeformat so can't constify
                        if (argp) argp = argp->nextp();
                        break;
                    default:  // Most operators, just move to next argument
                        if (argp) {
                            AstNode* const nextp = argp->nextp();
                            if (VN_IS(argp, Const)) {  // Convert it
                                const string out = constNumV(argp).displayed(nodep, fmt);
                                UINFO(9, "     DispConst: " << fmt << " -> " << out << "  for "
                                                            << argp);
                                // fmt = out w/ replace % with %% as it must be literal.
                                fmt = VString::quotePercent(out);
                                VL_DO_DANGLING(pushDeletep(argp->unlinkFrBack()), argp);
                            }
                            argp = nextp;
                        }
                        break;
                    }  // switch
                    newFormat += fmt;
                } else {
                    newFormat += ch;
                }
            }
            if (newFormat != nodep->text()) {
                nodep->text(newFormat);
                UINFO(9, "  Display out " << nodep);
            }
        }
        if (!nodep->exprsp() && nodep->name().find('%') == string::npos && !nodep->hidden()) {
            // Just a simple constant string - the formatting is pointless
            VL_DO_DANGLING(replaceConstString(nodep, nodep->name()), nodep);
        }
    }
    void visit(AstNodeFTask* nodep) override {
        VL_RESTORER(m_underRecFunc);
        if (nodep->recursive()) m_underRecFunc = true;
        iterateChildren(nodep);
    }

    void visit(AstNodeCCall* nodep) override {
        iterateChildren(nodep);
        m_hasJumpDelay = true;  // As don't analyze inside tasks for timing controls
    }
    void visit(AstNodeFTaskRef* nodep) override {
        // Note excludes AstFuncRef as other visitor below
        iterateChildren(nodep);
        m_hasJumpDelay = true;  // As don't analyze inside tasks for timing controls
    }
    void visit(AstFuncRef* nodep) override {
        visit(static_cast<AstNodeFTaskRef*>(nodep));
        if (m_params) {  // Only parameters force us to do constant function call propagation
            replaceWithSimulation(nodep);
        }
    }
    void visit(AstArg* nodep) override {
        // replaceWithSimulation on the Arg's parent FuncRef replaces these
        iterateChildren(nodep);
    }
    void visit(AstWhile* nodep) override {
        const bool oldHasJumpDelay = m_hasJumpDelay;
        m_hasJumpDelay = false;
        { iterateChildren(nodep); }
        const bool thisWhileHasJumpDelay = m_hasJumpDelay;
        m_hasJumpDelay = thisWhileHasJumpDelay || oldHasJumpDelay;
        if (m_doNConst) {
            if (nodep->condp()->isZero()) {
                UINFO(4, "WHILE(0) => nop " << nodep);
                nodep->v3warn(UNUSEDLOOP,
                              "Loop condition is always false; body will never execute");
                nodep->fileline()->modifyWarnOff(V3ErrorCode::UNUSEDLOOP, true);
                nodep->unlinkFrBack();
                VL_DO_DANGLING(pushDeletep(nodep), nodep);
            } else if (nodep->condp()->isNeqZero()) {
                if (!thisWhileHasJumpDelay) {
                    nodep->v3warn(INFINITELOOP, "Infinite loop (condition always true)");
                    nodep->fileline()->modifyWarnOff(V3ErrorCode::INFINITELOOP,
                                                     true);  // Complain just once
                }
            } else if (operandBoolShift(nodep->condp())) {
                replaceBoolShift(nodep->condp());
            }
        }
    }
    void visit(AstInitArray* nodep) override { iterateChildren(nodep); }
    void visit(AstInitItem* nodep) override { iterateChildren(nodep); }
    void visit(AstUnbounded* nodep) override { iterateChildren(nodep); }
    // These are converted by V3Param.  Don't constify as we don't want the
    // from() VARREF to disappear, if any.
    // If output of a presel didn't get consted, chances are V3Param didn't visit properly
    void visit(AstNodePreSel*) override {}

    // Ignored, can eliminate early
    void visit(AstSysIgnore* nodep) override {
        iterateChildren(nodep);
        if (m_doNConst) VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
    }

    void visit(AstStmtExpr* nodep) override {
        iterateChildren(nodep);
        if (!nodep->exprp() || VN_IS(nodep->exprp(), Const)) {
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
            return;
        }
        // TODO if there's an ExprStmt underneath just keep lower statements
        // (No current test case needs this)
    }

    // Simplify
    void visit(AstBasicDType* nodep) override {
        iterateChildren(nodep);
        nodep->cvtRangeConst();
    }

    //-----
    // Jump elimination

    void visit(AstJumpGo* nodep) override {
        // Any statements following the JumpGo (at this statement level) never execute, delete
        if (nodep->nextp()) pushDeletep(nodep->nextp()->unlinkFrBackWithNext());

        // JumpGo as last statement in target JumpBlock (including last in a last sub-list),
        // is a no-op, remove it.
        for (AstNode* abovep = nodep->abovep(); abovep; abovep = abovep->abovep()) {
            if (abovep == nodep->blockp()) {
                VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
                return;
            }
            // Stop if not doing expensive, or if the above node is not the last in its list,
            // ... or if it's not an 'if' TODO: it would be enough if it was not a branch.
            if (!m_doExpensive || abovep->nextp() || !VN_IS(abovep, If)) break;
        }
        // Mark JumpBlock as used
        m_usedJumpBlocks.emplace(nodep->blockp());
        m_hasJumpDelay = true;
    }

    void visit(AstJumpBlock* nodep) override {
        iterateChildren(nodep);

        // Remove if empty
        if (!nodep->stmtsp()) {
            UINFO(4, "JUMPLABEL => empty " << nodep);
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
            return;
        }

        // If no JumpGo points to this node, replace it with its body
        if (!m_usedJumpBlocks.count(nodep)) {
            UINFO(4, "JUMPLABEL => unused " << nodep);
            nodep->replaceWith(nodep->stmtsp()->unlinkFrBackWithNext());
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
        }
    }

    //-----
    // Below lines are magic expressions processed by astgen
    //  TREE_SKIP_VISIT("AstNODETYPE")    # Rename normal visit to visitGen and don't iterate
    //-----
    // clang-format off
    TREE_SKIP_VISIT("ArraySel");
    TREE_SKIP_VISIT("CAwait");

    //-----
    //  "AstNODETYPE {             # bracket not paren
    //                $accessor_name, ...
    //                             # .castFoo is the test VN_IS(object,Foo)
    //                             # ,, gets replaced with a , rather than &&
    //               }"            # bracket not paren
    //    ,"what to call"
    //
    // Where "what_to_call" is:
    //          "function to call"
    //          "AstREPLACEMENT_TYPE{ $accessor }"
    //          "!              # Print line number when matches, so can see operations
    //          "NEVER"         # Print error message
    //          "DONE"          # Process of matching did the transform already

    // In the future maybe support more complicated match & replace:
    //   ("AstOr  {%a, AstAnd{AstNot{%b}, %c}} if %a.width1 if %a==%b", "AstOr{%a,%c}; %b.delete");
    // Lhs/rhs would be implied; for non math operations you'd need $lhsp etc.

    //    v--- * * This op done on Verilog or C+++ mode, in all non-m_doConst stages
    //    v--- *1* These ops are always first, as we warn before replacing
    //    v--- *C* This op is a (C)++ op, only in m_doCpp mode
    //    v--- *V* This op is a (V)erilog op, only in m_doV mode
    //    v--- *A* This op works on (A)ll constant children, allowed in m_doConst mode
    //    v--- *S* This op specifies a type should use (S)hort-circuiting of its lhs op

    TREEOP1("AstSel{warnSelect(nodep)}",        "NEVER");
    // Generic constants on both side.  Do this first to avoid other replacements
    TREEOPA("AstNodeBiop {$lhsp.castConst, $rhsp.castConst, nodep->isPredictOptimizable()}",  "replaceConst(nodep)");
    TREEOPA("AstNodeUniop{$lhsp.castConst, !nodep->isOpaque(), nodep->isPredictOptimizable()}",  "replaceConst(nodep)");
    TREEOPA("AstNodeQuadop{$lhsp.castConst, $rhsp.castConst, $thsp.castConst, $fhsp.castConst}",  "replaceConst(nodep)");
    // Zero on one side or the other
    TREEOP ("AstAdd   {$lhsp.isZero, $rhsp}",   "replaceWRhs(nodep)");
    TREEOP ("AstAnd   {$lhsp.isZero, $rhsp, $rhsp.isPure}",   "replaceZero(nodep)");  // Can't use replaceZeroChkPure as we make this pattern in ChkPure
    // This visit function here must allow for short-circuiting.
    TREEOPS("AstLogAnd   {$lhsp.isZero}",       "replaceZero(nodep)");
    TREEOP ("AstLogAnd{$lhsp.isZero, $rhsp}",   "replaceZero(nodep)");
    // This visit function here must allow for short-circuiting.
    TREEOPS("AstLogOr   {$lhsp.isOne}",         "replaceNum(nodep, 1)");
    TREEOP ("AstLogOr {$lhsp.isZero, $rhsp}",   "replaceWRhsBool(nodep)");
    TREEOP ("AstDiv   {$lhsp.isZero, $rhsp}",   "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstDivS  {$lhsp.isZero, $rhsp}",   "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstMul   {$lhsp.isZero, $rhsp}",   "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstMulS  {$lhsp.isZero, $rhsp}",   "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstPow   {$rhsp.isZero}",          "replaceNum(nodep, 1)");  // Overrides lhs zero rule
    TREEOP ("AstPowSS {$rhsp.isZero}",          "replaceNum(nodep, 1)");  // Overrides lhs zero rule
    TREEOP ("AstPowSU {$rhsp.isZero}",          "replaceNum(nodep, 1)");  // Overrides lhs zero rule
    TREEOP ("AstPowUS {$rhsp.isZero}",          "replaceNum(nodep, 1)");  // Overrides lhs zero rule
    TREEOP ("AstOr    {$lhsp.isZero, $rhsp}",   "replaceWRhs(nodep)");
    TREEOP ("AstShiftL    {$lhsp.isZero, $rhsp}",  "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstShiftLOvr {$lhsp.isZero, $rhsp}",  "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstShiftR    {$lhsp.isZero, $rhsp}",  "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstShiftROvr {$lhsp.isZero, $rhsp}",  "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstShiftRS   {$lhsp.isZero, $rhsp}",  "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstShiftRSOvr{$lhsp.isZero, $rhsp}",  "replaceZeroChkPure(nodep,$rhsp)");
    TREEOP ("AstXor   {$lhsp.isZero, $rhsp}",   "replaceWRhs(nodep)");
    TREEOP ("AstSub   {$lhsp.isZero, $rhsp}",   "AstNegate{$rhsp}");
    TREEOP ("AstAdd   {$lhsp, $rhsp.isZero}",   "replaceWLhs(nodep)");
    TREEOP ("AstAnd   {$lhsp, $rhsp.isZero}",   "replaceZeroChkPure(nodep,$lhsp)");
    TREEOP ("AstLogAnd{$lhsp, $rhsp.isZero}",   "replaceZeroChkPure(nodep,$lhsp)");
    TREEOP ("AstLogOr {$lhsp, $rhsp.isZero}",   "replaceWLhsBool(nodep)");
    TREEOP ("AstMul   {$lhsp, $rhsp.isZero}",   "replaceZeroChkPure(nodep,$lhsp)");
    TREEOP ("AstMulS  {$lhsp, $rhsp.isZero}",   "replaceZeroChkPure(nodep,$lhsp)");
    TREEOP ("AstOr    {$lhsp, $rhsp.isZero}",   "replaceWLhs(nodep)");
    TREEOP ("AstShiftL    {$lhsp, $rhsp.isZero}",  "replaceWLhs(nodep)");
    TREEOP ("AstShiftLOvr {$lhsp, $rhsp.isZero}",  "replaceWLhs(nodep)");
    TREEOP ("AstShiftR    {$lhsp, $rhsp.isZero}",  "replaceWLhs(nodep)");
    TREEOP ("AstShiftROvr {$lhsp, $rhsp.isZero}",  "replaceWLhs(nodep)");
    TREEOP ("AstShiftRS   {$lhsp, $rhsp.isZero}",  "replaceWLhs(nodep)");
    TREEOP ("AstShiftRSOvr{$lhsp, $rhsp.isZero}",  "replaceWLhs(nodep)");
    TREEOP ("AstSub   {$lhsp, $rhsp.isZero}",   "replaceWLhs(nodep)");
    TREEOP ("AstXor   {$lhsp, $rhsp.isZero}",   "replaceWLhs(nodep)");
    // Non-zero on one side or the other
    TREEOP ("AstAnd   {$lhsp.isAllOnes, $rhsp}",        "replaceWRhs(nodep)");
    TREEOP ("AstLogAnd{$lhsp.isNeqZero, $rhsp}",        "replaceWRhsBool(nodep)");
    TREEOP ("AstOr    {$lhsp.isAllOnes, $rhsp, $rhsp.isPure}",        "replaceWLhs(nodep)");  // ->allOnes
    TREEOP ("AstLogOr {$lhsp.isNeqZero, $rhsp}",        "replaceNum(nodep,1)");
    TREEOP ("AstAnd   {$lhsp, $rhsp.isAllOnes}",        "replaceWLhs(nodep)");
    TREEOP ("AstLogAnd{$lhsp, $rhsp.isNeqZero}",        "replaceWLhsBool(nodep)");
    TREEOP ("AstOr    {$lhsp, $rhsp.isAllOnes, $lhsp.isPure}",        "replaceWRhs(nodep)");  // ->allOnes
    TREEOP ("AstLogOr {$lhsp, $rhsp.isNeqZero, $lhsp.isPure, nodep->isPure()}",        "replaceNum(nodep,1)");
    TREEOP ("AstXor   {$lhsp.isAllOnes, $rhsp}",        "AstNot{$rhsp}");
    TREEOP ("AstMul   {$lhsp.isOne, $rhsp}",    "replaceWRhs(nodep)");
    TREEOP ("AstMulS  {$lhsp.isOne, $rhsp}",    "replaceWRhs(nodep)");
    TREEOP ("AstDiv   {$lhsp, $rhsp.isOne}",    "replaceWLhs(nodep)");
    TREEOP ("AstDivS  {$lhsp, $rhsp.isOne}",    "replaceWLhs(nodep)");
    TREEOP ("AstMul   {operandIsPowTwo($lhsp), operandsSameWidth($lhsp,,$rhsp)}", "replaceMulShift(nodep)");  // a*2^n -> a<<n
    TREEOP ("AstDiv   {$lhsp, operandIsPowTwo($rhsp)}", "replaceDivShift(nodep)");  // a/2^n -> a>>n
    TREEOP ("AstModDiv{$lhsp, operandIsPowTwo($rhsp)}", "replaceModAnd(nodep)");  // a % 2^n -> a&(2^n-1)
    TREEOP ("AstPow   {operandIsTwo($lhsp), !$rhsp.isZero}",    "replacePowShift(nodep)");  // 2**a == 1<<a
    TREEOP ("AstPowSU {operandIsTwo($lhsp), !$rhsp.isZero}",    "replacePowShift(nodep)");  // 2**a == 1<<a
    TREEOP ("AstSub   {$lhsp.castAdd, operandSubAdd(nodep)}", "AstAdd{AstSub{$lhsp->castAdd()->lhsp(),$rhsp}, $lhsp->castAdd()->rhsp()}");  // ((a+x)-y) -> (a+(x-y))
    TREEOPC("AstAnd   {$lhsp.isOne, matchRedundantClean(nodep)}", "DONE")  // 1 & (a == b) -> (IData)(a == b)
    // Trinary ops
    // Note V3Case::Sel requires Cond to always be conditionally executed in C to prevent core dump!
    TREEOP ("AstNodeCond{$condp.isZero,       $thenp, $elsep}", "replaceWChild(nodep,$elsep)");
    TREEOP ("AstNodeCond{$condp.isNeqZero,    $thenp, $elsep}", "replaceWChild(nodep,$thenp)");
    TREEOPA("AstNodeCond{$condp.isZero,       $thenp.castConst, $elsep.castConst}", "replaceWChild(nodep,$elsep)");
    TREEOPA("AstNodeCond{$condp.isNeqZero,    $thenp.castConst, $elsep.castConst}", "replaceWChild(nodep,$thenp)");
    TREEOP ("AstNodeCond{$condp, operandsSame($thenp,,$elsep)}","replaceWChild(nodep,$thenp)");
    // This visit function here must allow for short-circuiting.
    TREEOPS("AstCond{$condp.isZero}",           "replaceWIteratedThs(nodep)");
    TREEOPS("AstCond{$condp.isNeqZero}",        "replaceWIteratedRhs(nodep)");
    TREEOP ("AstCond{$condp.castNot, $thenp, $elsep}", "AstCond{$condp->castNot()->lhsp(), $elsep, $thenp}");
    TREEOP ("AstNodeCond{$condp.width1, $thenp.width1, $thenp.isAllOnes, $elsep}", "AstLogOr {$condp, $elsep}");  // a?1:b == a||b
    TREEOP ("AstNodeCond{$condp.width1, $thenp.width1, $thenp, $elsep.isZero, !$elsep.isClassHandleValue}", "AstLogAnd{$condp, $thenp}");  // a?b:0 == a&&b
    TREEOP ("AstNodeCond{$condp.width1, $thenp.width1, $thenp, $elsep.isAllOnes}", "AstLogOr {AstNot{$condp}, $thenp}");  // a?b:1 == ~a||b
    TREEOP ("AstNodeCond{$condp.width1, $thenp.width1, $thenp.isZero, !$thenp.isClassHandleValue, $elsep}", "AstLogAnd{AstNot{$condp}, $elsep}");  // a?0:b == ~a&&b
    TREEOP ("AstNodeCond{!$condp.width1, operandBoolShift(nodep->condp())}", "replaceBoolShift(nodep->condp())");
    // Prefer constants on left, since that often needs a shift, it lets
    // constant red remove the shift
    TREEOP ("AstNodeBiCom{!$lhsp.castConst, $rhsp.castConst}",  "swapSides(nodep)");
    TREEOP ("AstNodeBiComAsv{operandAsvConst(nodep)}",  "replaceAsv(nodep)");
    TREEOP ("AstNodeBiComAsv{operandAsvSame(nodep)}",   "replaceAsv(nodep)");
    TREEOP ("AstNodeBiComAsv{operandAsvLUp(nodep)}",    "replaceAsvLUp(nodep)");
    TREEOP ("AstNodeBiComAsv{operandAsvRUp(nodep)}",    "replaceAsvRUp(nodep)");
    TREEOP ("AstLt   {!$lhsp.castConst,$rhsp.castConst}",       "AstGt  {$rhsp,$lhsp}");
    TREEOP ("AstLtS  {!$lhsp.castConst,$rhsp.castConst}",       "AstGtS {$rhsp,$lhsp}");
    TREEOP ("AstLte  {!$lhsp.castConst,$rhsp.castConst}",       "AstGte {$rhsp,$lhsp}");
    TREEOP ("AstLteS {!$lhsp.castConst,$rhsp.castConst}",       "AstGteS{$rhsp,$lhsp}");
    TREEOP ("AstGt   {!$lhsp.castConst,$rhsp.castConst}",       "AstLt  {$rhsp,$lhsp}");
    TREEOP ("AstGtS  {!$lhsp.castConst,$rhsp.castConst}",       "AstLtS {$rhsp,$lhsp}");
    TREEOP ("AstGte  {!$lhsp.castConst,$rhsp.castConst}",       "AstLte {$rhsp,$lhsp}");
    TREEOP ("AstGteS {!$lhsp.castConst,$rhsp.castConst}",       "AstLteS{$rhsp,$lhsp}");
    //    v--- *1* as These ops are always first, as we warn before replacing
    TREEOP1("AstLt   {$lhsp, $rhsp.isZero}",            "replaceNumSigned(nodep,0)");
    TREEOP1("AstGte  {$lhsp, $rhsp.isZero}",            "replaceNumSigned(nodep,1)");
    TREEOP1("AstGt   {$lhsp.isZero, $rhsp}",            "replaceNumSigned(nodep,0)");
    TREEOP1("AstLte  {$lhsp.isZero, $rhsp}",            "replaceNumSigned(nodep,1)");
    TREEOP1("AstGt   {$lhsp, $rhsp.isAllOnes, $lhsp->width()==$rhsp->width()}",  "replaceNumLimited(nodep,0)");
    TREEOP1("AstLte  {$lhsp, $rhsp.isAllOnes, $lhsp->width()==$rhsp->width()}",  "replaceNumLimited(nodep,1)");
    TREEOP1("AstLt   {$lhsp.isAllOnes, $rhsp, $lhsp->width()==$rhsp->width()}",  "replaceNumLimited(nodep,0)");
    TREEOP1("AstGte  {$lhsp.isAllOnes, $rhsp, $lhsp->width()==$rhsp->width()}",  "replaceNumLimited(nodep,1)");
    // Two level bubble pushing
    TREEOP ("AstNot   {$lhsp.castNot,  $lhsp->width()==VN_AS($lhsp,,Not)->lhsp()->width()}", "replaceWChild(nodep, $lhsp->castNot()->lhsp())");  // NOT(NOT(x))->x
    TREEOP ("AstLogNot{$lhsp.castLogNot}",              "replaceWChild(nodep, $lhsp->castLogNot()->lhsp())");  // LOGNOT(LOGNOT(x))->x
    TREEOPV("AstNot   {$lhsp.castEqCase, $lhsp.width1}","AstNeqCase{$lhsp->castEqCase()->lhsp(),$lhsp->castEqCase()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castEqCase}",              "AstNeqCase{$lhsp->castEqCase()->lhsp(),$lhsp->castEqCase()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castNeqCase, $lhsp.width1}","AstEqCase{$lhsp->castNeqCase()->lhsp(),$lhsp->castNeqCase()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castNeqCase}",             "AstEqCase {$lhsp->castNeqCase()->lhsp(),$lhsp->castNeqCase()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castEqWild, $lhsp.width1}","AstNeqWild{$lhsp->castEqWild()->lhsp(),$lhsp->castEqWild()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castEqWild}",              "AstNeqWild{$lhsp->castEqWild()->lhsp(),$lhsp->castEqWild()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castNeqWild, $lhsp.width1}","AstEqWild{$lhsp->castNeqWild()->lhsp(),$lhsp->castNeqWild()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castNeqWild}",             "AstEqWild {$lhsp->castNeqWild()->lhsp(),$lhsp->castNeqWild()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castEq, $lhsp.width1}",    "AstNeq {$lhsp->castEq()->lhsp(),$lhsp->castEq()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castEq}",                  "AstNeq {$lhsp->castEq()->lhsp(),$lhsp->castEq()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castNeq, $lhsp.width1}",   "AstEq  {$lhsp->castNeq()->lhsp(),$lhsp->castNeq()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castNeq}",                 "AstEq  {$lhsp->castNeq()->lhsp(),$lhsp->castNeq()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castLt, $lhsp.width1}",    "AstGte {$lhsp->castLt()->lhsp(),$lhsp->castLt()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castLt}",                  "AstGte {$lhsp->castLt()->lhsp(),$lhsp->castLt()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castLtS, $lhsp.width1}",   "AstGteS{$lhsp->castLtS()->lhsp(),$lhsp->castLtS()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castLtS}",                 "AstGteS{$lhsp->castLtS()->lhsp(),$lhsp->castLtS()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castLte, $lhsp.width1}",   "AstGt  {$lhsp->castLte()->lhsp(),$lhsp->castLte()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castLte}",                 "AstGt  {$lhsp->castLte()->lhsp(),$lhsp->castLte()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castLteS, $lhsp.width1}",  "AstGtS {$lhsp->castLteS()->lhsp(),$lhsp->castLteS()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castLteS}",                "AstGtS {$lhsp->castLteS()->lhsp(),$lhsp->castLteS()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castGt, $lhsp.width1}",    "AstLte {$lhsp->castGt()->lhsp(),$lhsp->castGt()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castGt}",                  "AstLte {$lhsp->castGt()->lhsp(),$lhsp->castGt()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castGtS, $lhsp.width1}",   "AstLteS{$lhsp->castGtS()->lhsp(),$lhsp->castGtS()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castGtS}",                 "AstLteS{$lhsp->castGtS()->lhsp(),$lhsp->castGtS()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castGte, $lhsp.width1}",   "AstLt  {$lhsp->castGte()->lhsp(),$lhsp->castGte()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castGte}",                 "AstLt  {$lhsp->castGte()->lhsp(),$lhsp->castGte()->rhsp()}");
    TREEOPV("AstNot   {$lhsp.castGteS, $lhsp.width1}",  "AstLtS {$lhsp->castGteS()->lhsp(),$lhsp->castGteS()->rhsp()}");
    TREEOP ("AstLogNot{$lhsp.castGteS}",                "AstLtS {$lhsp->castGteS()->lhsp(),$lhsp->castGteS()->rhsp()}");
    // Not common, but avoids compiler warnings about over shifting
    TREEOP ("AstShiftL   {operandHugeShiftL(nodep)}",   "replaceZero(nodep)");
    TREEOP ("AstShiftLOvr{operandHugeShiftL(nodep)}",   "replaceZero(nodep)");
    TREEOP ("AstShiftR   {operandHugeShiftR(nodep)}",   "replaceZero(nodep)");
    TREEOP ("AstShiftROvr{operandHugeShiftR(nodep)}",   "replaceZero(nodep)");
    TREEOP ("AstShiftL{operandShiftOp(nodep)}",         "replaceShiftOp(nodep)");
    TREEOP ("AstShiftR{operandShiftOp(nodep)}",         "replaceShiftOp(nodep)");
    TREEOP ("AstShiftL{operandShiftShift(nodep)}",      "replaceShiftShift(nodep)");
    TREEOP ("AstShiftR{operandShiftShift(nodep)}",      "replaceShiftShift(nodep)");
    TREEOP ("AstWordSel{operandWordOOB(nodep)}",        "replaceZero(nodep)");
    // Compress out EXTENDs to appease loop unroller
    TREEOPV("AstEq    {$rhsp.castExtend,operandBiExtendConstShrink(nodep)}",    "DONE");
    TREEOPV("AstNeq   {$rhsp.castExtend,operandBiExtendConstShrink(nodep)}",    "DONE");
    TREEOPV("AstGt    {$rhsp.castExtend,operandBiExtendConstShrink(nodep)}",    "DONE");
    TREEOPV("AstGte   {$rhsp.castExtend,operandBiExtendConstShrink(nodep)}",    "DONE");
    TREEOPV("AstLt    {$rhsp.castExtend,operandBiExtendConstShrink(nodep)}",    "DONE");
    TREEOPV("AstLte   {$rhsp.castExtend,operandBiExtendConstShrink(nodep)}",    "DONE");
    TREEOPV("AstEq    {$rhsp.castExtend,operandBiExtendConstOver(nodep)}",      "replaceZero(nodep)");
    TREEOPV("AstNeq   {$rhsp.castExtend,operandBiExtendConstOver(nodep)}",      "replaceNum(nodep,1)");
    TREEOPV("AstGt    {$rhsp.castExtend,operandBiExtendConstOver(nodep)}",      "replaceNum(nodep,1)");
    TREEOPV("AstGte   {$rhsp.castExtend,operandBiExtendConstOver(nodep)}",      "replaceNum(nodep,1)");
    TREEOPV("AstLt    {$rhsp.castExtend,operandBiExtendConstOver(nodep)}",      "replaceZero(nodep)");
    TREEOPV("AstLte   {$rhsp.castExtend,operandBiExtendConstOver(nodep)}",      "replaceZero(nodep)");
    // Identical operands on both sides
    // AstLogAnd/AstLogOr already converted to AstAnd/AstOr for these rules
    // AstAdd->ShiftL(#,1) but uncommon
    TREEOP ("AstAnd    {operandsSame($lhsp,,$rhsp)}",   "replaceWLhs(nodep)");
    TREEOP ("AstDiv    {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstDivS   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstOr     {operandsSame($lhsp,,$rhsp)}",   "replaceWLhs(nodep)");
    TREEOP ("AstSub    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstXor    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstEq     {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");  // We let X==X -> 1, although in a true 4-state sim it's X.
    TREEOP ("AstEqD    {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");  // We let X==X -> 1, although in a true 4-state sim it's X.
    TREEOP ("AstEqN    {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");  // We let X==X -> 1, although in a true 4-state sim it's X.
    TREEOP ("AstEqCase {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstEqWild {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstGt     {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstGtD    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstGtN    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstGtS    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstGte    {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstGteD   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstGteN   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstGteS   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstLt     {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstLtD    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstLtN    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstLtS    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstLte    {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstLteD   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstLteN   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstLteS   {operandsSame($lhsp,,$rhsp)}",   "replaceNum(nodep,1)");
    TREEOP ("AstNeq    {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstNeqD   {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstNeqN   {operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstNeqCase{operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstNeqWild{operandsSame($lhsp,,$rhsp)}",   "replaceZero(nodep)");
    TREEOP ("AstLogAnd {operandsSame($lhsp,,$rhsp)}",   "replaceWLhsBool(nodep)");
    TREEOP ("AstLogOr  {operandsSame($lhsp,,$rhsp)}",   "replaceWLhsBool(nodep)");
    ///=== Verilog operators
    // Comparison against 1'b0/1'b1; must be careful about widths.
    // These use Not, so must be Verilog only
    TREEOPV("AstEq    {$rhsp.width1, $lhsp.isZero,    $rhsp}",  "AstNot{$rhsp}");
    TREEOPV("AstEq    {$lhsp.width1, $lhsp, $rhsp.isZero}",     "AstNot{$lhsp}");
    TREEOPV("AstEq    {$rhsp.width1, $lhsp.isAllOnes, $rhsp}",  "replaceWRhs(nodep)");
    TREEOPV("AstEq    {$lhsp.width1, $lhsp, $rhsp.isAllOnes}",  "replaceWLhs(nodep)");
    TREEOPV("AstNeq   {$rhsp.width1, $lhsp.isZero,    $rhsp}",  "replaceWRhs(nodep)");
    TREEOPV("AstNeq   {$lhsp.width1, $lhsp, $rhsp.isZero}",     "replaceWLhs(nodep)");
    TREEOPV("AstNeq   {$rhsp.width1, $lhsp.isAllOnes, $rhsp}",  "AstNot{$rhsp}");
    TREEOPV("AstNeq   {$lhsp.width1, $lhsp, $rhsp.isAllOnes}",  "AstNot{$lhsp}");
    TREEOPV("AstLt    {$rhsp.width1, $lhsp.isZero,    $rhsp}",  "replaceWRhs(nodep)");  // Because not signed #s
    TREEOPV("AstGt    {$lhsp.width1, $lhsp, $rhsp.isZero}",     "replaceWLhs(nodep)");  // Because not signed #s
    // Useful for CONDs added around ARRAYSEL's in V3Case step
    TREEOPV("AstLte   {$lhsp->width()==$rhsp->width(), $rhsp.isAllOnes}", "replaceNum(nodep,1)");
    // Simplify reduction operators
    // This also gets &{...,0,....} => const 0  (Common for unused_ok signals)
    TREEOPV("AstRedAnd{$lhsp, $lhsp.width1}",   "replaceWLhs(nodep)");
    TREEOPV("AstRedOr {$lhsp, $lhsp.width1}",   "replaceWLhs(nodep)");
    TREEOPV("AstRedXor{$lhsp, $lhsp.width1}",   "replaceWLhs(nodep)");
    TREEOPV("AstRedAnd{$lhsp.castConcat}",      "AstAnd{AstRedAnd{$lhsp->castConcat()->lhsp()}, AstRedAnd{$lhsp->castConcat()->rhsp()}}");  // &{a,b} => {&a}&{&b}
    TREEOPV("AstRedOr {$lhsp.castConcat}",      "AstOr {AstRedOr {$lhsp->castConcat()->lhsp()}, AstRedOr {$lhsp->castConcat()->rhsp()}}");  // |{a,b} => {|a}|{|b}
    TREEOPV("AstRedXor{$lhsp.castConcat}",      "AstXor{AstRedXor{$lhsp->castConcat()->lhsp()}, AstRedXor{$lhsp->castConcat()->rhsp()}}");  // ^{a,b} => {^a}^{^b}
    TREEOPV("AstRedAnd{$lhsp.castExtend, $lhsp->width() > VN_AS($lhsp,,Extend)->lhsp()->width()}", "replaceZero(nodep)");  // &{0,...} => 0  Prevents compiler limited range error
    TREEOPV("AstRedOr {$lhsp.castExtend}",      "AstRedOr {$lhsp->castExtend()->lhsp()}");
    TREEOPV("AstRedXor{$lhsp.castExtend}",      "AstRedXor{$lhsp->castExtend()->lhsp()}");
    TREEOP ("AstRedXor{$lhsp.castXor, VN_IS(VN_AS($lhsp,,Xor)->lhsp(),,Const)}", "AstXor{AstRedXor{$lhsp->castXor()->lhsp()}, AstRedXor{$lhsp->castXor()->rhsp()}}");  // ^(const ^ a) => (^const)^(^a)
    TREEOPC("AstAnd {$lhsp.castConst, $rhsp.castRedXor, matchBitOpTree(nodep)}", "DONE");
    TREEOPV("AstOneHot{$lhsp.width1}",          "replaceWLhs(nodep)");
    TREEOPV("AstOneHot0{$lhsp.width1}",         "replaceNum(nodep,1)");
    // Binary AND/OR is faster than logical and/or (usually)
    TREEOPV("AstLogAnd{matchBiopToBitwise(nodep)}", "AstAnd{$lhsp,$rhsp}");
    TREEOPV("AstLogOr {matchBiopToBitwise(nodep)}", "AstOr{$lhsp,$rhsp}");
    TREEOPV("AstLogNot{$lhsp.width1}",  "AstNot{$lhsp}");
    // CONCAT(CONCAT({a},{b}),{c}) -> CONCAT({a},CONCAT({b},{c}))
    // CONCAT({const},CONCAT({const},{c})) -> CONCAT((constifiedCONC{const|const},{c}))
    TREEOPV("AstConcat{matchConcatRand(nodep)}",      "DONE");
    TREEOPV("AstConcat{operandConcatMove(nodep)}",      "moveConcat(nodep)");
    TREEOPV("AstConcat{$lhsp.isZero, $rhsp}",           "replaceExtend(nodep, nodep->rhsp())");
    // CONCAT(a[1],a[0]) -> a[1:0]
    TREEOPV("AstConcat{$lhsp.castSel, $rhsp.castSel, ifAdjacentSel(VN_AS($lhsp,,Sel),,VN_AS($rhsp,,Sel))}",  "replaceConcatSel(nodep)");
    TREEOPV("AstConcat{ifConcatMergeableBiop($lhsp), concatMergeable($lhsp,,$rhsp,,0)}", "replaceConcatMerge(nodep)");
    // Common two-level operations that can be simplified
    TREEOP ("AstAnd {$lhsp.castConst,matchAndCond(nodep)}", "DONE");
    TREEOP ("AstAnd {$lhsp.castConst, $rhsp.castOr, matchMaskedOr(nodep)}", "DONE");
    TREEOPC("AstAnd {$lhsp.castConst, matchMaskedShift(nodep)}", "DONE");
    TREEOP ("AstAnd {$lhsp.castOr, $rhsp.castOr, operandAndOrSame(nodep)}", "replaceAndOr(nodep)");
    TREEOP ("AstOr  {$lhsp.castAnd,$rhsp.castAnd,operandAndOrSame(nodep)}", "replaceAndOr(nodep)");
    TREEOP ("AstOr  {matchOrAndNot(nodep)}",            "DONE");
    TREEOP ("AstAnd {operandShiftSame(nodep)}",         "replaceShiftSame(nodep)");
    TREEOP ("AstOr  {operandShiftSame(nodep)}",         "replaceShiftSame(nodep)");
    TREEOP ("AstXor {operandShiftSame(nodep)}",         "replaceShiftSame(nodep)");
    TREEOPC("AstAnd {matchBitOpTree(nodep)}", "DONE");
    TREEOPC("AstOr  {matchBitOpTree(nodep)}", "DONE");
    TREEOPC("AstXor {matchBitOpTree(nodep)}", "DONE");
    // Note can't simplify a extend{extends}, extends{extend}, as the sign
    // bits end up in the wrong places
    TREEOPV("AstExtend{operandsSameWidth(nodep,,$lhsp)}",  "replaceWLhs(nodep)");
    TREEOPV("AstExtend{$lhsp.castExtend}",  "replaceExtend(nodep, VN_AS(nodep->lhsp(), Extend)->lhsp())");
    TREEOPV("AstExtendS{$lhsp.castExtendS}", "replaceExtend(nodep, VN_AS(nodep->lhsp(), ExtendS)->lhsp())");
    TREEOPV("AstReplicate{$srcp, $countp.isOne, $srcp->width()==nodep->width()}", "replaceWLhs(nodep)");  // {1{lhs}}->lhs
    TREEOPV("AstReplicateN{$lhsp, $rhsp.isOne, $lhsp->width()==nodep->width()}", "replaceWLhs(nodep)");  // {1{lhs}}->lhs
    TREEOPV("AstReplicate{$srcp.castReplicate, operandRepRep(nodep)}", "DONE");  // {2{3{lhs}}}->{6{lhs}}
    TREEOPV("AstConcat{operandConcatSame(nodep)}", "DONE");  // {a,a}->{2{a}}, {a,2{a}}->{3{a}, etc
    // Next rule because AUTOINST puts the width of bits in
    // to pins, even when the widths are exactly the same across the hierarchy.
    TREEOPV("AstSel{matchSelRand(nodep)}",      "DONE");
    TREEOPV("AstSel{operandSelExtend(nodep)}",  "DONE");
    TREEOPV("AstSel{operandSelFull(nodep)}",    "replaceWChild(nodep, nodep->fromp())");
    TREEOPV("AstSel{$fromp.castSel}",           "replaceSelSel(nodep)");
    TREEOPV("AstSel{$fromp.castAdd, operandSelBiLower(nodep)}", "DONE");
    TREEOPV("AstSel{$fromp.castAnd, operandSelBiLower(nodep)}", "DONE");
    TREEOPV("AstSel{$fromp.castOr,  operandSelBiLower(nodep)}", "DONE");
    TREEOPV("AstSel{$fromp.castSub, operandSelBiLower(nodep)}", "DONE");
    TREEOPV("AstSel{$fromp.castXor, operandSelBiLower(nodep)}", "DONE");
    TREEOPV("AstSel{$fromp.castShiftR, operandSelShiftLower(nodep)}",   "DONE");
    TREEOPA("AstSel{$fromp.castConst, $lsbp.castConst, }",   "replaceConst(nodep)");
    TREEOPV("AstSel{$fromp.castConcat, $lsbp.castConst, }",  "replaceSelConcat(nodep)");
    TREEOPV("AstSel{$fromp.castReplicate, $lsbp.castConst, operandSelReplicate(nodep) }",    "DONE");
    // V3Tristate requires selects below BufIf1.
    // Also do additional operators that are bit-independent, but only definite
    // win if bit select is a constant (otherwise we may need to compute bit index several times)
    TREEOPV("AstSel{$fromp.castBufIf1}",                "replaceSelIntoBiop(nodep)");
    TREEOPV("AstSel{$fromp.castNot}",                   "replaceSelIntoUniop(nodep)");
    TREEOPV("AstSel{$fromp.castAnd,$lsbp.castConst}",   "replaceSelIntoBiop(nodep)");
    TREEOPV("AstSel{$fromp.castOr,$lsbp.castConst}",    "replaceSelIntoBiop(nodep)");
    TREEOPV("AstSel{$fromp.castXor,$lsbp.castConst}",   "replaceSelIntoBiop(nodep)");
    // This visit function here must allow for short-circuiting.
    TREEOPS("AstLogIf{$lhsp.isZero}",  "replaceNum(nodep, 1)");
    TREEOPV("AstLogIf{$lhsp, $rhsp}",  "AstLogOr{AstLogNot{$lhsp},$rhsp}");
    TREEOPV("AstLogEq{$lhsp, $rhsp}",  "replaceLogEq(nodep)");
    // Strings
    TREEOPA("AstPutcN{$lhsp.castConst, $rhsp.castConst, $thsp.castConst}",  "replaceConst(nodep)");
    TREEOPA("AstSubstrN{$lhsp.castConst, $rhsp.castConst, $thsp.castConst}",  "replaceConst(nodep)");
    TREEOPA("AstCvtPackString{$lhsp.castConst}", "replaceConstString(nodep, VN_AS(nodep->lhsp(), Const)->num().toString())");
    // Custom
    // Implied by AstIsUnbounded::numberOperate: V("AstIsUnbounded{$lhsp.castConst}", "replaceNum(nodep, 0)");
    TREEOPV("AstIsUnbounded{$lhsp.castUnbounded}", "replaceNum(nodep, 1)");
    // clang-format on

    // Possible futures:
    // (a?(b?y:x):y) -> (a&&!b)?x:y
    // (a?(b?x:y):y) -> (a&&b)?x:y
    // (a?x:(b?x:y)) -> (a||b)?x:y
    // (a?x:(b?y:x)) -> (a||!b)?x:y

    // Note we can't convert EqCase/NeqCase to Eq/Neq here because that would break 3'b1x1==3'b101

    //-----
    void visit(AstNode* nodep) override {
        // Default: Just iterate
        if (m_required) {
            if (VN_IS(nodep, NodeDType) || VN_IS(nodep, Range) || VN_IS(nodep, SliceSel)) {
                // Ignore dtypes for parameter type pins
            } else {
                nodep->v3error("Expecting expression to be constant, but can't convert a "
                               << nodep->prettyTypeName() << " to constant.");
            }
        } else {
            if (nodep->isTimingControl()) m_hasJumpDelay = true;
            // Calculate the width of this operation
            if (m_params && !nodep->width()) nodep = V3Width::widthParamsEdit(nodep);
            iterateChildren(nodep);
        }
    }

public:
    // Processing Mode Enum
    enum ProcMode : uint8_t {
        PROC_PARAMS_NOWARN,
        PROC_PARAMS,
        PROC_GENERATE,
        PROC_LIVE,
        PROC_V_WARN,
        PROC_V_NOWARN,
        PROC_V_EXPENSIVE,
        PROC_CPP
    };

    // CONSTRUCTORS
    ConstVisitor(ProcMode pmode, bool globalPass)
        : m_globalPass{globalPass}
        , m_concswapNames{globalPass ? ("__Vconcswap_" + cvtToStr(s_globalPassNum++)) : ""} {
        // clang-format off
        switch (pmode) {
        case PROC_PARAMS_NOWARN:  m_doV = true;  m_doNConst = true; m_params = true;
                                  m_required = false; break;
        case PROC_PARAMS:         m_doV = true;  m_doNConst = true; m_params = true;
                                  m_required = true; break;
        case PROC_GENERATE:       m_doV = true;  m_doNConst = true; m_params = true;
                                  m_required = true; m_doGenerate = true; break;
        case PROC_LIVE:           break;
        case PROC_V_WARN:         m_doV = true;  m_doNConst = true; m_warn = true; m_convertLogicToBit = true; break;
        case PROC_V_NOWARN:       m_doV = true;  m_doNConst = true; break;
        case PROC_V_EXPENSIVE:    m_doV = true;  m_doNConst = true; m_doExpensive = true; break;
        case PROC_CPP:            m_doV = false; m_doNConst = true; m_doCpp = true; break;
        default:                  v3fatalSrc("Bad case"); break;
        }
        // clang-format on
    }
    ~ConstVisitor() override {
        if (m_doCpp) {
            if (m_globalPass) {
                V3Stats::addStat("Optimizations, Const bit op reduction", m_statBitOpReduction);
            } else {
                V3Stats::addStatSum("Optimizations, Const bit op reduction", m_statBitOpReduction);
            }
        }
    }

    AstNode* mainAcceptEdit(AstNode* nodep) {
        VIsCached::clearCacheTree();  // Avoid using any stale isPure
        // Operate starting at a random place
        return iterateSubtreeReturnEdits(nodep);
    }
};

uint32_t ConstVisitor::s_globalPassNum = 0;

//######################################################################
// Const class functions

//! Force this cell node's parameter list to become a constant
//! @return  Pointer to the edited node.
AstNode* V3Const::constifyParamsEdit(AstNode* nodep) {
    // if (debug() > 0) nodep->dumpTree("-  forceConPRE : ");
    // Resize even if the node already has a width, because buried in the tree
    // we may have a node we just created with signing, etc, that isn't sized yet.

    // Make sure we've sized everything first
    nodep = V3Width::widthParamsEdit(nodep);
    ConstVisitor visitor{ConstVisitor::PROC_PARAMS, /* globalPass: */ false};
    if (AstVar* const varp = VN_CAST(nodep, Var)) {
        // If a var wants to be constified, it's really a param, and
        // we want the value to be constant.  We aren't passed just the
        // init value because we need widthing above to handle the var's type.
        if (varp->valuep()) visitor.mainAcceptEdit(varp->valuep());
    } else {
        nodep = visitor.mainAcceptEdit(nodep);
    }
    // Because we do edits, nodep links may get trashed and core dump if have next line
    // if (debug() > 0) nodep->dumpTree("-  forceConDONE: ");
    return nodep;
}

//! Constify this cell node's parameter list if possible
//! @return  Pointer to the edited node.
AstNode* V3Const::constifyParamsNoWarnEdit(AstNode* nodep) {
    // if (debug() > 0) nodep->dumpTree("-  forceConPRE : ");
    // Resize even if the node already has a width, because buried in the tree
    // we may have a node we just created with signing, etc, that isn't sized yet.

    // Make sure we've sized everything first
    nodep = V3Width::widthParamsEdit(nodep);
    ConstVisitor visitor{ConstVisitor::PROC_PARAMS_NOWARN, /* globalPass: */ false};
    if (AstVar* const varp = VN_CAST(nodep, Var)) {
        // If a var wants to be constified, it's really a param, and
        // we want the value to be constant.  We aren't passed just the
        // init value because we need widthing above to handle the var's type.
        if (varp->valuep()) visitor.mainAcceptEdit(varp->valuep());
    } else {
        nodep = visitor.mainAcceptEdit(nodep);
    }
    // Because we do edits, nodep links may get trashed and core dump this.
    // if (debug() > 0) nodep->dumpTree("-  forceConDONE: ");
    return nodep;
}

//! Force this cell node's parameter list to become a constant inside generate.
//! If we are inside a generated "if", "case" or "for", we don't want to
//! trigger warnings when we deal with the width. It is possible that these
//! are spurious, existing within sub-expressions that will not actually be
//! generated. Since such occurrences, must be constant, in order to be
//! something a generate block can depend on, we can wait until later to do the
//! width check.
//! @return  Pointer to the edited node.
AstNode* V3Const::constifyGenerateParamsEdit(AstNode* nodep) {
    // if (debug() > 0) nodep->dumpTree("-  forceConPRE:: ");
    // Resize even if the node already has a width, because buried in the tree
    // we may have a node we just created with signing, etc, that isn't sized
    // yet.

    // Make sure we've sized everything first
    nodep = V3Width::widthGenerateParamsEdit(nodep);
    ConstVisitor visitor{ConstVisitor::PROC_GENERATE, /* globalPass: */ false};
    if (AstVar* const varp = VN_CAST(nodep, Var)) {
        // If a var wants to be constified, it's really a param, and
        // we want the value to be constant.  We aren't passed just the
        // init value because we need widthing above to handle the var's type.
        if (varp->valuep()) visitor.mainAcceptEdit(varp->valuep());
    } else {
        nodep = visitor.mainAcceptEdit(nodep);
    }
    // Because we do edits, nodep links may get trashed and core dump this.
    // if (debug() > 0) nodep->dumpTree("-  forceConDONE: ");
    return nodep;
}

void V3Const::constifyAllLint(AstNetlist* nodep) {
    // Only call from Verilator.cpp, as it uses user#'s
    UINFO(2, __FUNCTION__ << ":");
    {
        ConstVisitor visitor{ConstVisitor::PROC_V_WARN, /* globalPass: */ true};
        (void)visitor.mainAcceptEdit(nodep);
    }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("const", 0, dumpTreeEitherLevel() >= 3);
}

void V3Const::constifyCpp(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    {
        ConstVisitor visitor{ConstVisitor::PROC_CPP, /* globalPass: */ true};
        (void)visitor.mainAcceptEdit(nodep);
    }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("const_cpp", 0, dumpTreeEitherLevel() >= 3);
}

AstNode* V3Const::constifyEdit(AstNode* nodep) {
    ConstVisitor visitor{ConstVisitor::PROC_V_NOWARN, /* globalPass: */ false};
    nodep = visitor.mainAcceptEdit(nodep);
    return nodep;
}

AstNode* V3Const::constifyEditCpp(AstNode* nodep) {
    ConstVisitor visitor{ConstVisitor::PROC_CPP, /* globalPass: */ false};
    nodep = visitor.mainAcceptEdit(nodep);
    return nodep;
}

void V3Const::constifyAllLive(AstNetlist* nodep) {
    // Only call from Verilator.cpp, as it uses user#'s
    // This only pushes constants up, doesn't make any other edits
    // IE doesn't prune dead statements, as we need to do some usability checks after this
    UINFO(2, __FUNCTION__ << ":");
    {
        ConstVisitor visitor{ConstVisitor::PROC_LIVE, /* globalPass: */ true};
        (void)visitor.mainAcceptEdit(nodep);
    }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("const", 0, dumpTreeEitherLevel() >= 3);
}

void V3Const::constifyAll(AstNetlist* nodep) {
    // Only call from Verilator.cpp, as it uses user#'s
    UINFO(2, __FUNCTION__ << ":");
    {
        ConstVisitor visitor{ConstVisitor::PROC_V_EXPENSIVE, /* globalPass: */ true};
        (void)visitor.mainAcceptEdit(nodep);
    }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("const", 0, dumpTreeEitherLevel() >= 3);
}

AstNode* V3Const::constifyExpensiveEdit(AstNode* nodep) {
    ConstVisitor visitor{ConstVisitor::PROC_V_EXPENSIVE, /* globalPass: */ false};
    nodep = visitor.mainAcceptEdit(nodep);
    return nodep;
}
