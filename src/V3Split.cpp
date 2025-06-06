// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Break always into separate statements to reduce temps
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
// V3Split implements two separate transformations:
//  splitAlwaysAll() splits large always blocks into smaller always blocks
//  when possible (but does not change the order of statements relative
//  to one another.)
//
//  splitReorderAll() reorders statements within individual blocks
//  to avoid delay vars when possible. It no longer splits always blocks.
//
// Both use a common base class, and common graph-building code to reflect
// data dependencies within an always block (the "scoreboard".)
//
// The scoreboard tracks data deps as follows:
//
//      ALWAYS
//              ASSIGN ({var} <= {cons})
//              Record as generating var_DLY (independent of use of var), consumers
//              ASSIGN ({var} = {cons}
//              Record generator and consumer
//      Any var that is only consumed can be ignored.
//      Then we split into separate ALWAYS blocks.
//
// The scoreboard includes innards of if/else nodes also.  Splitting is no
// longer limited to top-level statements, we can split within if-else
// blocks. We want to be able to split this:
//
//    always @ (...) begin
//      if (reset) begin
//        a <= 0;
//        b <= 0;
//         // ... ten thousand more
//      end
//      else begin
//        a <= a_in;
//        b <= b_in;
//         // ... ten thousand more
//      end
//    end
//
// ...into a separate block for each of a, b, and so on.  Even though this
// requires duplicating the conditional many times, it's usually
// better. Later modules (V3Gate, V3Order) run faster if they aren't
// handling enormous blocks with long lists of inputs and outputs.
//
// Furthermore, the optional reorder routine can optimize this:
//      NODEASSIGN/NODEIF/WHILE
//              S1: ASSIGN {v1} <= 0.   // Duplicate of below
//              S2: ASSIGN {v1} <= {v0}
//              S3: IF (...,
//                      X1: ASSIGN {v2} <= {v1}
//                      X2: ASSIGN {v3} <= {v2}
//      We'd like to swap S2 and S3, and X1 and X2.
//
//  Create a graph in split assignment order.
//      v3 -breakable-> v3Dly --> X2 --> v2 -brk-> v2Dly -> X1 -> v1
//      Likewise on each "upper" statement vertex
//              v3Dly & v2Dly -> S3 -> v1 & v2
//              v1 -brk-> v1Dly -> S2 -> v0
//                        v1Dly -> S1 -> {empty}
//  Multiple assignments to the same variable must remain in order
//
//  Also vars must not be "public" and we also scoreboard nodep->isPure()
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Split.h"

#include "V3Graph.h"
#include "V3Stats.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Support classes

class SplitNodeVertex VL_NOT_FINAL : public V3GraphVertex {
    VL_RTTI_IMPL(SplitNodeVertex, V3GraphVertex)
    AstNode* const m_nodep;

protected:
    SplitNodeVertex(V3Graph* graphp, AstNode* nodep)
        : V3GraphVertex{graphp}
        , m_nodep{nodep} {}
    ~SplitNodeVertex() override = default;
    // ACCESSORS
    // Do not make accessor for nodep(),  It may change due to
    // reordering a lower block, but we don't repair it
    string name() const override { return cvtToHex(m_nodep) + ' ' + m_nodep->prettyTypeName(); }
    FileLine* fileline() const override { return nodep()->fileline(); }

public:
    virtual AstNode* nodep() const { return m_nodep; }
};

class SplitPliVertex final : public SplitNodeVertex {
    VL_RTTI_IMPL(SplitPliVertex, SplitNodeVertex)
public:
    explicit SplitPliVertex(V3Graph* graphp, AstNode* nodep)
        : SplitNodeVertex{graphp, nodep} {}
    ~SplitPliVertex() override = default;
    string name() const override VL_MT_STABLE { return "*PLI*"; }
    string dotColor() const override { return "green"; }
};

class SplitLogicVertex final : public SplitNodeVertex {
    VL_RTTI_IMPL(SplitLogicVertex, SplitNodeVertex)
public:
    SplitLogicVertex(V3Graph* graphp, AstNode* nodep)
        : SplitNodeVertex{graphp, nodep} {}
    ~SplitLogicVertex() override = default;
    string dotColor() const override { return "yellow"; }
};

class SplitVarStdVertex final : public SplitNodeVertex {
    VL_RTTI_IMPL(SplitVarStdVertex, SplitNodeVertex)
public:
    SplitVarStdVertex(V3Graph* graphp, AstNode* nodep)
        : SplitNodeVertex{graphp, nodep} {}
    ~SplitVarStdVertex() override = default;
    string dotColor() const override { return "skyblue"; }
};

class SplitVarPostVertex final : public SplitNodeVertex {
    VL_RTTI_IMPL(SplitVarPostVertex, SplitNodeVertex)
public:
    SplitVarPostVertex(V3Graph* graphp, AstNode* nodep)
        : SplitNodeVertex{graphp, nodep} {}
    ~SplitVarPostVertex() override = default;
    string name() const override { return "POST "s + SplitNodeVertex::name(); }
    string dotColor() const override { return "CadetBlue"; }
};

//######################################################################
// Edge types

class SplitEdge VL_NOT_FINAL : public V3GraphEdge {
    VL_RTTI_IMPL(SplitEdge, V3GraphEdge)
    uint32_t m_ignoreInStep = 0;  // Step number that if set to, causes this edge to be ignored
    static uint32_t s_stepNum;  // Global step number
protected:
    static constexpr int WEIGHT_NORMAL = 10;
    SplitEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top, int weight,
              bool cutable = CUTABLE)
        : V3GraphEdge{graphp, fromp, top, weight, cutable} {}
    ~SplitEdge() override = default;

public:
    // Iterator for graph functions
    static void incrementStep() { ++s_stepNum; }
    bool ignoreThisStep() const { return m_ignoreInStep == s_stepNum; }
    void setIgnoreThisStep() { m_ignoreInStep = s_stepNum; }
    virtual bool followScoreboard() const = 0;
    static bool followScoreboard(const V3GraphEdge* edgep) {
        const SplitEdge* const oedgep = static_cast<const SplitEdge*>(edgep);
        if (oedgep->ignoreThisStep()) return false;
        return oedgep->followScoreboard();
    }
    static bool followCyclic(const V3GraphEdge* edgep) {
        const SplitEdge* const oedgep = static_cast<const SplitEdge*>(edgep);
        return (!oedgep->ignoreThisStep());
    }
    string dotStyle() const override {
        return ignoreThisStep() ? "dotted" : V3GraphEdge::dotStyle();
    }
};
uint32_t SplitEdge::s_stepNum = 0;

class SplitPostEdge final : public SplitEdge {
    VL_RTTI_IMPL(SplitPostEdge, SplitEdge)
public:
    SplitPostEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top)
        : SplitEdge{graphp, fromp, top, WEIGHT_NORMAL} {}
    ~SplitPostEdge() override = default;
    bool followScoreboard() const override { return false; }
    string dotColor() const override { return "khaki"; }
};

class SplitLVEdge final : public SplitEdge {
    VL_RTTI_IMPL(SplitLVEdge, SplitEdge)
public:
    SplitLVEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top)
        : SplitEdge{graphp, fromp, top, WEIGHT_NORMAL} {}
    ~SplitLVEdge() override = default;
    bool followScoreboard() const override { return true; }
    string dotColor() const override { return "yellowGreen"; }
};

class SplitRVEdge final : public SplitEdge {
    VL_RTTI_IMPL(SplitRVEdge, SplitEdge)
public:
    SplitRVEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top)
        : SplitEdge{graphp, fromp, top, WEIGHT_NORMAL} {}
    ~SplitRVEdge() override = default;
    bool followScoreboard() const override { return true; }
    string dotColor() const override { return "green"; }
};

class SplitScorebdEdge final : public SplitEdge {
    VL_RTTI_IMPL(SplitScorebdEdge, SplitEdge)
public:
    SplitScorebdEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top)
        : SplitEdge{graphp, fromp, top, WEIGHT_NORMAL} {}
    ~SplitScorebdEdge() override = default;
    bool followScoreboard() const override { return true; }
    string dotColor() const override { return "blue"; }
};

class SplitStrictEdge final : public SplitEdge {
    VL_RTTI_IMPL(SplitStrictEdge, SplitEdge)
    // A strict order, based on the original statement order in the graph
    // The only non-cutable edge type
public:
    SplitStrictEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top)
        : SplitEdge{graphp, fromp, top, WEIGHT_NORMAL, NOT_CUTABLE} {}
    ~SplitStrictEdge() override = default;
    bool followScoreboard() const override { return true; }
    string dotColor() const override { return "blue"; }
};

//######################################################################
// Split class functions

class SplitReorderBaseVisitor VL_NOT_FINAL : public VNVisitor {
    // NODE STATE
    // AstVarScope::user1p      -> Var SplitNodeVertex* for usage var, 0=not set yet
    // AstVarScope::user2p      -> Var SplitNodeVertex* for delayed assignment var, 0=not set yet
    // Ast*::user3p             -> Statement SplitLogicVertex* (temporary only)
    // Ast*::user4              -> Current ordering number (reorderBlock usage)
    const VNUser1InUse m_inuser1;
    const VNUser2InUse m_inuser2;
    const VNUser3InUse m_inuser3;
    const VNUser4InUse m_inuser4;

protected:
    // STATE
    string m_noReorderWhy;  // Reason we can't reorder
    std::vector<SplitLogicVertex*> m_stmtStackps;  // Current statements being tracked
    SplitPliVertex* m_pliVertexp;  // Element specifying PLI ordering
    V3Graph m_graph;  // Scoreboard of var usages/dependencies
    bool m_inDly;  // Inside ASSIGNDLY

    // CONSTRUCTORS
public:
    SplitReorderBaseVisitor() { scoreboardClear(); }
    ~SplitReorderBaseVisitor() override = default;

    // METHODS
protected:
    void scoreboardClear() {
        // VV*****  We reset user1p() and user2p on each block!!!
        m_inDly = false;
        m_graph.clear();
        m_stmtStackps.clear();
        m_pliVertexp = nullptr;
        m_noReorderWhy = "";
        AstNode::user1ClearTree();
        AstNode::user2ClearTree();
        AstNode::user3ClearTree();
        AstNode::user4ClearTree();
    }

private:
    void scoreboardPli(AstNode* nodep) {
        // Order all PLI statements with other PLI statements
        // This ensures $display's and such remain in proper order
        // We don't prevent splitting out other non-pli statements, however.
        if (!m_pliVertexp) {
            m_pliVertexp = new SplitPliVertex{&m_graph, nodep};  // m_graph.clear() will delete it
        }
        for (const auto& vtxp : m_stmtStackps) {
            // Both ways...
            new SplitScorebdEdge{&m_graph, vtxp, m_pliVertexp};
            new SplitScorebdEdge{&m_graph, m_pliVertexp, vtxp};
        }
    }
    void scoreboardPushStmt(AstNode* nodep) {
        // UINFO(9, "    push " << nodep);
        SplitLogicVertex* const vertexp = new SplitLogicVertex{&m_graph, nodep};
        m_stmtStackps.push_back(vertexp);
        UASSERT_OBJ(!nodep->user3p(), nodep, "user3p should not be used; cleared in processBlock");
        nodep->user3p(vertexp);
    }
    void scoreboardPopStmt() {
        // UINFO(9, "    pop");
        UASSERT(!m_stmtStackps.empty(), "Stack underflow");
        m_stmtStackps.pop_back();
    }

protected:
    void scanBlock(AstNode* nodep) {
        // Iterate across current block, making the scoreboard
        for (AstNode* nextp = nodep; nextp; nextp = nextp->nextp()) {
            scoreboardPushStmt(nextp);
            iterate(nextp);
            scoreboardPopStmt();
        }
    }

    void pruneDepsOnInputs() {
        for (V3GraphVertex& vertex : m_graph.vertices()) {
            if (vertex.outEmpty() && vertex.is<SplitVarStdVertex>()) {
                if (debug() >= 9) {
                    const SplitVarStdVertex& sVtx = static_cast<SplitVarStdVertex&>(vertex);
                    UINFO(0, "Will prune deps on var " << sVtx.nodep());
                    sVtx.nodep()->dumpTree("-  ");
                }
                for (V3GraphEdge& edge : vertex.inEdges()) {
                    SplitEdge& oedge = static_cast<SplitEdge&>(edge);
                    oedge.setIgnoreThisStep();
                }
            }
        }
    }

    virtual void makeRvalueEdges(SplitVarStdVertex* vstdp) = 0;

    // VISITORS
    void visit(AstAlways* nodep) override = 0;
    void visit(AstNodeIf* nodep) override = 0;

    // We don't do AstNodeFor/AstWhile loops, due to the standard question
    // of what is before vs. after

    void visit(AstAssignDly* nodep) override {
        VL_RESTORER(m_inDly);
        m_inDly = true;
        UINFO(4, "    ASSIGNDLY " << nodep);
        iterateChildren(nodep);
    }
    void visit(AstVarRef* nodep) override {
        if (!m_stmtStackps.empty()) {
            AstVarScope* const vscp = nodep->varScopep();
            UASSERT_OBJ(vscp, nodep, "Not linked");
            if (!nodep->varp()->isConst()) {  // Constant lookups can be ignored
                // ---
                // NOTE: Formerly at this location we would avoid
                // splitting or reordering if the variable is public.
                //
                // However, it should be perfectly safe to split an
                // always block containing a public variable.
                // Neither operation should perturb PLI's view of
                // the variable.
                //
                // Former code:
                //
                //   if (nodep->varp()->isSigPublic()) {
                //       // Public signals shouldn't be changed,
                //       // pli code might be messing with them
                //       scoreboardPli(nodep);
                //   }
                // ---

                // Create vertexes for variable
                if (!vscp->user1p()) {
                    SplitVarStdVertex* const vstdp = new SplitVarStdVertex{&m_graph, vscp};
                    vscp->user1p(vstdp);
                }
                SplitVarStdVertex* const vstdp
                    = reinterpret_cast<SplitVarStdVertex*>(vscp->user1p());

                // SPEEDUP: We add duplicate edges, that should be fixed
                if (m_inDly && nodep->access().isWriteOrRW()) {
                    UINFO(4, "     VARREFDLY: " << nodep);
                    // Delayed variable is different from non-delayed variable
                    if (!vscp->user2p()) {
                        SplitVarPostVertex* const vpostp = new SplitVarPostVertex{&m_graph, vscp};
                        vscp->user2p(vpostp);
                        new SplitPostEdge{&m_graph, vstdp, vpostp};
                    }
                    SplitVarPostVertex* const vpostp
                        = reinterpret_cast<SplitVarPostVertex*>(vscp->user2p());
                    // Add edges
                    for (SplitLogicVertex* vxp : m_stmtStackps) {
                        new SplitLVEdge{&m_graph, vpostp, vxp};
                    }
                } else {  // Nondelayed assignment
                    if (nodep->access().isWriteOrRW()) {
                        // Non-delay; need to maintain existing ordering
                        // with all consumers of the signal
                        UINFO(4, "     VARREFLV: " << nodep);
                        for (SplitLogicVertex* ivxp : m_stmtStackps) {
                            new SplitLVEdge{&m_graph, vstdp, ivxp};
                        }
                    } else {
                        UINFO(4, "     VARREF:   " << nodep);
                        makeRvalueEdges(vstdp);
                    }
                }
            }
        }
    }

    void visit(AstJumpGo* nodep) override {
        // Jumps will disable reordering at all levels
        // This is overly pessimistic; we could treat jumps as barriers, and
        // reorder everything between jumps/labels, however jumps are rare
        // in always, so the performance gain probably isn't worth the work.
        UINFO(9, "         NoReordering " << nodep);
        m_noReorderWhy = "JumpGo";
        iterateChildren(nodep);
    }

    //--------------------
    // Default
    void visit(AstNode* nodep) override {
        // **** SPECIAL default type that sets PLI_ORDERING
        if (!m_stmtStackps.empty() && !nodep->isPure()) {
            UINFO(9, "         NotSplittable " << nodep);
            scoreboardPli(nodep);
        }
        if (nodep->isTimingControl()) {
            UINFO(9, "         NoReordering " << nodep);
            m_noReorderWhy = "TimingControl";
        }
        iterateChildren(nodep);
    }

private:
    VL_UNCOPYABLE(SplitReorderBaseVisitor);
};

class ReorderVisitor final : public SplitReorderBaseVisitor {
    // CONSTRUCTORS
public:
    explicit ReorderVisitor(AstNetlist* nodep) { iterate(nodep); }
    ~ReorderVisitor() override = default;

    // METHODS
protected:
    void makeRvalueEdges(SplitVarStdVertex* vstdp) override {
        for (SplitLogicVertex* vxp : m_stmtStackps) new SplitRVEdge{&m_graph, vxp, vstdp};
    }

    void cleanupBlockGraph(AstNode* nodep) {
        // Transform the graph into what we need
        UINFO(5, "ReorderBlock " << nodep);
        m_graph.removeRedundantEdgesMax(&V3GraphEdge::followAlwaysTrue);

        if (dumpGraphLevel() >= 9) m_graph.dumpDotFilePrefixed("reorderg_nodup", false);

        // Mark all the logic for this step
        // Vertex::m_user begin: true indicates logic for this step
        m_graph.userClearVertices();
        for (AstNode* nextp = nodep; nextp; nextp = nextp->nextp()) {
            SplitLogicVertex* const vvertexp
                = reinterpret_cast<SplitLogicVertex*>(nextp->user3p());
            vvertexp->user(true);
        }

        // If a var vertex has only inputs, it's a input-only node,
        // and can be ignored for coloring **this block only**
        SplitEdge::incrementStep();
        pruneDepsOnInputs();

        // For reordering this single block only, mark all logic
        // vertexes not involved with this step as unimportant
        for (V3GraphVertex& vertex : m_graph.vertices()) {
            if (!vertex.user()) {
                if (vertex.is<SplitLogicVertex>()) {
                    for (V3GraphEdge& edge : vertex.inEdges()) {
                        SplitEdge& oedge = static_cast<SplitEdge&>(edge);
                        oedge.setIgnoreThisStep();
                    }
                    for (V3GraphEdge& edge : vertex.outEdges()) {
                        SplitEdge& oedge = static_cast<SplitEdge&>(edge);
                        oedge.setIgnoreThisStep();
                    }
                }
            }
        }

        // Weak coloring to determine what needs to remain in order
        // This follows all step-relevant edges excluding PostEdges, which are done later
        m_graph.weaklyConnected(&SplitEdge::followScoreboard);

        // Add hard orderings between all nodes of same color, in the order they appeared
        std::unordered_map<uint32_t, SplitLogicVertex*> lastOfColor;
        for (AstNode* nextp = nodep; nextp; nextp = nextp->nextp()) {
            SplitLogicVertex* const vvertexp
                = reinterpret_cast<SplitLogicVertex*>(nextp->user3p());
            const uint32_t color = vvertexp->color();
            UASSERT_OBJ(color, nextp, "No node color assigned");
            if (lastOfColor[color]) {
                new SplitStrictEdge{&m_graph, lastOfColor[color], vvertexp};
            }
            lastOfColor[color] = vvertexp;
        }

        // And a real ordering to get the statements into something reasonable
        // We don't care if there's cutable violations here...
        // Non-cutable violations should be impossible; as those edges are program-order
        if (dumpGraphLevel() >= 9) m_graph.dumpDotFilePrefixed("splitg_preo", false);
        m_graph.acyclic(&SplitEdge::followCyclic);
        m_graph.rank(&SplitEdge::followCyclic);  // Or order(), but that's more expensive
        if (dumpGraphLevel() >= 9) m_graph.dumpDotFilePrefixed("splitg_opt", false);
    }

    void reorderBlock(AstNode* nodep) {
        // Reorder statements in the completed graph

        // Map the rank numbers into nodes they associate with
        std::multimap<uint32_t, AstNode*> rankMap;
        int currOrder = 0;  // Existing sequence number of assignment
        for (AstNode* nextp = nodep; nextp; nextp = nextp->nextp()) {
            const SplitLogicVertex* const vvertexp
                = reinterpret_cast<SplitLogicVertex*>(nextp->user3p());
            rankMap.emplace(vvertexp->rank(), nextp);
            nextp->user4(++currOrder);  // Record current ordering
        }

        // Is the current ordering OK?
        bool leaveAlone = true;
        int newOrder = 0;  // New sequence number of assignment
        for (auto it = rankMap.cbegin(); it != rankMap.cend(); ++it) {
            const AstNode* const nextp = it->second;
            if (++newOrder != nextp->user4()) leaveAlone = false;
        }
        if (leaveAlone) {
            UINFO(6, "   No changes");
        } else {
            VNRelinker replaceHandle;  // Where to add the list
            AstNode* newListp = nullptr;
            for (auto it = rankMap.cbegin(); it != rankMap.cend(); ++it) {
                AstNode* const nextp = it->second;
                UINFO(6, "   New order: " << nextp);
                if (nextp == nodep) {
                    nodep->unlinkFrBack(&replaceHandle);
                } else {
                    nextp->unlinkFrBack();
                }
                if (newListp) {
                    newListp = newListp->addNext(nextp);
                } else {
                    newListp = nextp;
                }
            }
            replaceHandle.relink(newListp);
        }
    }

    void processBlock(AstNode* nodep) {
        if (!nodep) return;  // Empty lists are ignorable
        // Pass the first node in a list of block items, we'll process them
        // Check there's >= 2 sub statements, else nothing to analyze
        // Save recursion state
        AstNode* firstp = nodep;  // We may reorder, and nodep is no longer first.
        void* const oldBlockUser3 = nodep->user3p();  // May be overloaded in below loop, save it
        nodep->user3p(nullptr);
        UASSERT_OBJ(nodep->firstAbovep(), nodep,
                    "Node passed is in next list; should have processed all list at once");
        // Process it
        if (!nodep->nextp()) {
            // Just one, so can't reorder.  Just look for more blocks/statements.
            iterate(nodep);
        } else {
            UINFO(9, "  processBlock " << nodep);
            // Process block and followers
            scanBlock(nodep);
            if (m_noReorderWhy != "") {  // Jump or something nasty
                UINFO(9, "  NoReorderBlock because " << m_noReorderWhy);
            } else {
                // Reorder statements in this block
                cleanupBlockGraph(nodep);
                reorderBlock(nodep);
                // Delete old vertexes and edges only applying to this block
                // First, walk back to first in list
                while (firstp->backp()->nextp() == firstp) firstp = firstp->backp();
                for (AstNode* nextp = firstp; nextp; nextp = nextp->nextp()) {
                    SplitLogicVertex* const vvertexp
                        = reinterpret_cast<SplitLogicVertex*>(nextp->user3p());
                    vvertexp->unlinkDelete(&m_graph);
                }
            }
        }
        // Again, nodep may no longer be first.
        firstp->user3p(oldBlockUser3);
    }

    void visit(AstAlways* nodep) override {
        UINFO(4, "   ALW   " << nodep);
        if (debug() >= 9) nodep->dumpTree("-  alwIn:: ");
        scoreboardClear();
        processBlock(nodep->stmtsp());
        if (debug() >= 9) nodep->dumpTree("-  alwOut: ");
    }

    void visit(AstNodeIf* nodep) override {
        UINFO(4, "     IF " << nodep);
        iterateAndNextNull(nodep->condp());
        processBlock(nodep->thensp());
        processBlock(nodep->elsesp());
    }

private:
    VL_UNCOPYABLE(ReorderVisitor);
};

using ColorSet = std::unordered_set<uint32_t>;
using AlwaysVec = std::vector<AstAlways*>;

class IfColorVisitor final : public VNVisitorConst {
    // MEMBERS
    ColorSet m_colors;  // All colors in the original always block

    std::vector<AstNodeIf*> m_ifStack;  // Stack of nested if-statements we're currently processing

    std::unordered_map<AstNodeIf*, ColorSet>
        m_ifColors;  // Map each if-statement to the set of colors (split blocks)
    // that will get a copy of that if-statement

    // CONSTRUCTORS
public:
    // Visit through *nodep and map each AstNodeIf within to the set of
    // colors it will participate in. Also find the whole set of colors.
    explicit IfColorVisitor(AstAlways* nodep) { iterateConst(nodep); }
    ~IfColorVisitor() override = default;

    // METHODS
    const ColorSet& colors() const { return m_colors; }
    const ColorSet& colors(AstNodeIf* nodep) const {
        const auto it = m_ifColors.find(nodep);
        UASSERT_OBJ(it != m_ifColors.end(), nodep, "Node missing from split color() map");
        return it->second;
    }

private:
    void trackNode(AstNode* nodep) {
        if (nodep->user3p()) {
            const SplitLogicVertex* const vertexp
                = reinterpret_cast<SplitLogicVertex*>(nodep->user3p());
            const uint32_t color = vertexp->color();
            m_colors.insert(color);
            UINFO(8, "  SVL " << vertexp << " has color " << color);

            // Record that all containing ifs have this color.
            for (auto it = m_ifStack.cbegin(); it != m_ifStack.cend(); ++it) {
                m_ifColors[*it].insert(color);
            }
        }
    }

protected:
    void visit(AstNodeIf* nodep) override {
        m_ifStack.push_back(nodep);
        trackNode(nodep);
        iterateChildrenConst(nodep);
        m_ifStack.pop_back();
    }
    void visit(AstNode* nodep) override {
        trackNode(nodep);
        iterateChildrenConst(nodep);
    }

private:
    VL_UNCOPYABLE(IfColorVisitor);
};

class EmitSplitVisitor final : public VNVisitor {
    // MEMBERS
    const AstAlways* const m_origAlwaysp;  // Block that *this will split
    const IfColorVisitor* const m_ifColorp;  // Digest of results of prior coloring

    // Map each color to our current place within the color's new always
    std::unordered_map<uint32_t, AstNode*> m_addAfter;

    AlwaysVec* const m_newBlocksp;  // Split always blocks we have generated

    // CONSTRUCTORS
public:
    // EmitSplitVisitor visits through always block *nodep
    // and generates its split blocks, writing the split blocks
    // into *newBlocksp.
    EmitSplitVisitor(AstAlways* nodep, const IfColorVisitor* ifColorp, AlwaysVec* newBlocksp)
        : m_origAlwaysp{nodep}
        , m_ifColorp{ifColorp}
        , m_newBlocksp{newBlocksp} {
        UINFO(6, "  splitting always " << nodep);
    }

    ~EmitSplitVisitor() override = default;

    // METHODS
    void go() {
        // Create a new always for each color
        const ColorSet& colors = m_ifColorp->colors();
        for (unsigned int color : colors) {
            // We don't need to clone m_origAlwaysp->sensesp() here;
            // V3Activate already moved it to a parent node.
            AstAlways* const alwaysp
                = new AstAlways{m_origAlwaysp->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr};
            // Put a placeholder node into stmtp to track our position.
            // We'll strip these out after the blocks are fully cloned.
            AstSplitPlaceholder* const placeholderp = makePlaceholderp();
            alwaysp->addStmtsp(placeholderp);
            m_addAfter[color] = placeholderp;
            m_newBlocksp->push_back(alwaysp);
        }
        // Scan the body of the always. We'll handle if/else
        // specially, everything else is a leaf node that we can
        // just clone into one of the split always blocks.
        iterateAndNextNull(m_origAlwaysp->stmtsp());
    }

protected:
    AstSplitPlaceholder* makePlaceholderp() {
        return new AstSplitPlaceholder{m_origAlwaysp->fileline()};
    }

    void visit(AstNode* nodep) override {
        // Anything that's not an if/else we assume is a leaf
        // (that is, something we won't split.) Don't visit further
        // into the leaf.
        //
        // A leaf might contain another if, for example a WHILE loop
        // could contain an if. We can't split WHILE loops, so we
        // won't split its nested if either. Just treat it as part
        // of the leaf; do not visit further; do not reach visit(AstNodeIf*)
        // for such an embedded if.

        // Each leaf must have a user3p
        UASSERT_OBJ(nodep->user3p(), nodep, "null user3p in V3Split leaf");

        // Clone the leaf into its new always block
        const SplitLogicVertex* const vxp = reinterpret_cast<SplitLogicVertex*>(nodep->user3p());
        const uint32_t color = vxp->color();
        AstNode* const clonedp = nodep->cloneTree(false);
        m_addAfter[color]->addNextHere(clonedp);
        m_addAfter[color] = clonedp;
    }

    void visit(AstNodeIf* nodep) override {
        const ColorSet& colors = m_ifColorp->colors(nodep);
        using CloneMap = std::unordered_map<uint32_t, AstNodeIf*>;
        CloneMap clones;

        for (unsigned int color : colors) {
            // Clone this if into its set of split blocks
            AstSplitPlaceholder* const if_placeholderp = makePlaceholderp();
            AstSplitPlaceholder* const else_placeholderp = makePlaceholderp();
            // We check for condition isPure earlier, but may still clone a
            // non-pure to separate from other pure statements.
            AstIf* const clonep = new AstIf{nodep->fileline(), nodep->condp()->cloneTree(true),
                                            if_placeholderp, else_placeholderp};
            const AstIf* const origp = VN_CAST(nodep, If);
            if (origp) {
                // Preserve pragmas from unique if's
                // so assertions work properly
                clonep->uniquePragma(origp->uniquePragma());
                clonep->unique0Pragma(origp->unique0Pragma());
                clonep->priorityPragma(origp->priorityPragma());
            }
            clones[color] = clonep;
            m_addAfter[color]->addNextHere(clonep);
            m_addAfter[color] = if_placeholderp;
        }

        iterateAndNextNull(nodep->thensp());

        for (const auto& color : colors) m_addAfter[color] = clones[color]->elsesp();

        iterateAndNextNull(nodep->elsesp());

        for (const auto& color : colors) m_addAfter[color] = clones[color];
    }

private:
    VL_UNCOPYABLE(EmitSplitVisitor);
};

class RemovePlaceholdersVisitor final : public VNVisitor {
    // MEMBERS
    bool m_isPure = true;
    int m_emptyAlways = 0;

    // CONSTRUCTORS
    RemovePlaceholdersVisitor() = default;
    ~RemovePlaceholdersVisitor() override = default;

    // VISITORS
    void visit(AstSplitPlaceholder* nodep) override { pushDeletep(nodep->unlinkFrBack()); }
    void visit(AstNodeIf* nodep) override {
        VL_RESTORER(m_isPure);
        m_isPure = true;
        iterateChildren(nodep);
        if (!nodep->thensp() && !nodep->elsesp() && m_isPure) pushDeletep(nodep->unlinkFrBack());
    }
    void visit(AstAlways* nodep) override {
        VL_RESTORER(m_isPure);
        m_isPure = true;
        iterateChildren(nodep);
        if (m_isPure) {
            bool emptyOrCommentOnly = true;
            for (AstNode* bodysp = nodep->stmtsp(); bodysp; bodysp = bodysp->nextp()) {
                // If this always block contains only AstComment, remove here.
                // V3Gate will remove anyway.
                if (!VN_IS(bodysp, Comment)) {
                    emptyOrCommentOnly = false;
                    break;
                }
            }
            if (emptyOrCommentOnly) {
                VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
                ++m_emptyAlways;
            }
        }
    }
    void visit(AstNode* nodep) override {
        m_isPure &= nodep->isPure();
        iterateChildren(nodep);  // must visit regardless of m_isPure to remove placeholders
    }

    VL_UNCOPYABLE(RemovePlaceholdersVisitor);

public:
    static int exec(AstAlways* nodep) {
        RemovePlaceholdersVisitor visitor;
        visitor.iterate(nodep);
        return visitor.m_emptyAlways;
    }
};

class SplitVisitor final : public SplitReorderBaseVisitor {
    // Keys are original always blocks pending delete,
    // values are newly split always blocks pending insertion
    // at the same position as the originals:
    std::unordered_map<AstAlways*, AlwaysVec> m_replaceBlocks;

    // AstNodeIf* whose condition we're currently visiting
    const AstNode* m_curIfConditional = nullptr;
    VDouble0 m_statSplits;  // Statistic tracking

    // CONSTRUCTORS
public:
    explicit SplitVisitor(AstNetlist* nodep) {
        iterate(nodep);

        // Splice newly-split blocks into the tree. Remove placeholders
        // from newly-split blocks. Delete the original always blocks
        // that we're replacing.
        for (auto it = m_replaceBlocks.begin(); it != m_replaceBlocks.end(); ++it) {
            AstAlways* const origp = it->first;
            for (AlwaysVec::iterator addme = it->second.begin(); addme != it->second.end();
                 ++addme) {
                origp->addNextHere(*addme);
                const int numRemoved = RemovePlaceholdersVisitor::exec(*addme);
                m_statSplits -= numRemoved;
            }
            origp->unlinkFrBack();  // Without next
            VL_DO_DANGLING(origp->deleteTree(), origp);
        }
    }

    ~SplitVisitor() override { V3Stats::addStat("Optimizations, Split always", m_statSplits); }

    // METHODS
protected:
    void makeRvalueEdges(SplitVarStdVertex* vstdp) override {
        // Each 'if' depends on rvalues in its own conditional ONLY,
        // not rvalues in the if/else bodies.
        for (auto it = m_stmtStackps.cbegin(); it != m_stmtStackps.cend(); ++it) {
            const AstNodeIf* const ifNodep = VN_CAST((*it)->nodep(), NodeIf);
            if (ifNodep && (m_curIfConditional != ifNodep)) continue;
            new SplitRVEdge{&m_graph, *it, vstdp};
        }
    }

    void colorAlwaysGraph() {
        // Color the graph to indicate subsets, each of which
        // we can split into its own always block.
        m_graph.removeRedundantEdgesMax(&V3GraphEdge::followAlwaysTrue);

        // Some vars are primary inputs to the always block; prune
        // edges on those vars. Reasoning: if two statements both depend
        // on primary input A, it's ok to split these statements. Whereas
        // if they both depend on locally-generated variable B, the statements
        // must be kept together.
        SplitEdge::incrementStep();
        pruneDepsOnInputs();

        // For any 'if' node whose deps have all been pruned
        // (meaning, its conditional expression only looks at primary
        // inputs) prune all edges that depend on the 'if'.
        for (V3GraphVertex& vertex : m_graph.vertices()) {
            SplitLogicVertex* const logicp = vertex.cast<SplitLogicVertex>();
            if (!logicp) continue;

            const AstNodeIf* const ifNodep = VN_CAST(logicp->nodep(), NodeIf);
            if (!ifNodep) continue;

            bool pruneMe = true;
            for (const V3GraphEdge& edge : logicp->outEdges()) {
                const SplitEdge& oedge = static_cast<const SplitEdge&>(edge);
                if (!oedge.ignoreThisStep()) {
                    // This if conditional depends on something we can't
                    // prune -- a variable generated in the current block.
                    pruneMe = false;

                    // When we can't prune dependencies on the conditional,
                    // give a hint about why...
                    if (debug() >= 9) {
                        V3GraphVertex* vxp = oedge.top();
                        const SplitNodeVertex* const nvxp
                            = static_cast<const SplitNodeVertex*>(vxp);
                        UINFO(0, "Cannot prune if-node due to edge "
                                     << &oedge << " pointing to node " << nvxp->nodep());
                        nvxp->nodep()->dumpTree("-  ");
                    }

                    break;
                }
            }

            if (!pruneMe) continue;

            // This if can be split; prune dependencies on it.
            for (V3GraphEdge& edge : logicp->inEdges()) {
                SplitEdge& oedge = static_cast<SplitEdge&>(edge);
                oedge.setIgnoreThisStep();
            }
        }

        if (dumpGraphLevel() >= 9) m_graph.dumpDotFilePrefixed("splitg_nodup", false);

        // Weak coloring to determine what needs to remain grouped
        // in a single always. This follows all edges excluding:
        //  - those we pruned above
        //  - PostEdges, which are done later
        m_graph.weaklyConnected(&SplitEdge::followScoreboard);
        if (dumpGraphLevel() >= 9) m_graph.dumpDotFilePrefixed("splitg_colored", false);
    }

    void visit(AstAlways* nodep) override {
        // build the scoreboard
        scoreboardClear();
        scanBlock(nodep->stmtsp());

        if (m_noReorderWhy != "") {
            // We saw a jump or something else rare that we don't handle.
            UINFO(9, "  NoSplitBlock because " << m_noReorderWhy);
            return;
        }

        // Look across the entire tree of if/else blocks in the always,
        // and color regions that must be kept together.
        UINFO(5, "SplitVisitor @ " << nodep);
        colorAlwaysGraph();

        // Map each AstNodeIf to the set of colors (split always blocks)
        // it must participate in. Also find the whole set of colors.
        const IfColorVisitor ifColor{nodep};

        if (ifColor.colors().size() > 1) {
            // Counting original always blocks rather than newly-split
            // always blocks makes it a little easier to use this stat to
            // check the result of the t_alw_split test:
            m_statSplits += ifColor.colors().size() - 1;  // -1 for the original always

            // Visit through the original always block one more time,
            // and emit the split always blocks into m_replaceBlocks:
            EmitSplitVisitor emitSplit{nodep, &ifColor, &(m_replaceBlocks[nodep])};
            emitSplit.go();
        }
    }
    void visit(AstNodeIf* nodep) override {
        UINFO(4, "     IF " << nodep);
        if (!nodep->condp()->isPure()) m_noReorderWhy = "Impure IF condition";
        {
            VL_RESTORER(m_curIfConditional);
            m_curIfConditional = nodep;
            iterateAndNextNull(nodep->condp());
        }
        scanBlock(nodep->thensp());
        scanBlock(nodep->elsesp());
    }

private:
    VL_UNCOPYABLE(SplitVisitor);
};

//######################################################################
// Split class functions

void V3Split::splitReorderAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    { ReorderVisitor{nodep}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("reorder", 0, dumpTreeEitherLevel() >= 3);
}
void V3Split::splitAlwaysAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    { SplitVisitor{nodep}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("split", 0, dumpTreeEitherLevel() >= 3);
}
