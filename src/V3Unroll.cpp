// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Add temporaries, such as for unroll nodes
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
// V3Unroll's Transformations:
//      Note is called twice.  Once on modules for GenFor unrolling,
//      Again after V3Scope for normal for loop unrolling.
//
// Each module:
//      Look for "FOR" loops and unroll them if <= 32 loops.
//      (Eventually, a better way would be to simulate the entire loop; ala V3Table.)
//      Convert remaining FORs to WHILEs
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Unroll.h"

#include "V3Const.h"
#include "V3Simulate.h"
#include "V3Stats.h"

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Unroll state, as a visitor of each AstNode

class UnrollVisitor final : public VNVisitor {
    // STATE - across all visitors
    AstVar* m_forVarp;  // Iterator variable
    const AstVarScope* m_forVscp;  // Iterator variable scope (nullptr for generate pass)
    const AstNode* m_ignoreIncp;  // Increment node to ignore
    bool m_varModeCheck;  // Just checking RHS assignments
    bool m_varAssignHit;  // Assign var hit
    bool m_forkHit;  // Fork hit
    bool m_generate;  // Expand single generate For loop
    string m_beginName;  // What name to give begin iterations
    // STATE - Statistic tracking
    VDouble0 m_statLoops;  // Statistic tracking
    VDouble0 m_statIters;  // Statistic tracking

    // METHODS
    void replaceVarRef(AstNode* bodyp, AstNode* varValuep) {
        // Replace all occurances of loop variable in bodyp and next
        bodyp->foreachAndNext([this, varValuep](AstVarRef* refp) {
            if (refp->varp() == m_forVarp && refp->varScopep() == m_forVscp
                && refp->access().isReadOnly()) {
                AstNode* const newconstp = varValuep->cloneTree(false);
                refp->replaceWith(newconstp);
                VL_DO_DANGLING(pushDeletep(refp), refp);
            }
        });
    }

    bool cantUnroll(AstNode* nodep, const char* reason) const {
        if (m_generate)
            nodep->v3warn(E_UNSUPPORTED, "Unsupported: Can't unroll generate for; " << reason);
        UINFO(4, "   Can't Unroll: " << reason << " :" << nodep);
        // if (debug() >= 9) nodep->dumpTree("-  cant: ");
        V3Stats::addStatSum("Unrolling gave up, "s + reason, 1);
        return false;
    }

    bool bodySizeOverRecurse(AstNode* nodep, int& bodySize, int bodyLimit) {
        if (!nodep) return false;
        bodySize++;
        // Exit once exceeds limits, rather than always total
        // so don't go O(n^2) when can't unroll
        if (bodySize > bodyLimit) return true;
        if (bodySizeOverRecurse(nodep->op1p(), bodySize, bodyLimit)) return true;
        if (bodySizeOverRecurse(nodep->op2p(), bodySize, bodyLimit)) return true;
        if (bodySizeOverRecurse(nodep->op3p(), bodySize, bodyLimit)) return true;
        if (bodySizeOverRecurse(nodep->op4p(), bodySize, bodyLimit)) return true;
        // Tail recurse.
        return bodySizeOverRecurse(nodep->nextp(), bodySize, bodyLimit);
    }

    bool forUnrollCheck(
        AstNode* const nodep,
        const VOptionBool& unrollFull,  // Pragma unroll_full, unroll_disable
        AstNode* const initp,  // Maybe under nodep (no nextp), or standalone (ignore nextp)
        AstNode* condp,
        AstNode* const incp,  // Maybe under nodep or in bodysp
        AstNode* bodysp) {
        // To keep the IF levels low, we return as each test fails.
        UINFO(4, " FOR Check " << nodep);
        if (initp) UINFO(6, "    Init " << initp);
        if (condp) UINFO(6, "    Cond " << condp);
        if (incp) UINFO(6, "    Inc  " << incp);

        if (unrollFull.isSetFalse()) return cantUnroll(nodep, "pragma unroll_disable");

        // Initial value check
        AstAssign* const initAssp = VN_CAST(initp, Assign);
        if (!initAssp) return cantUnroll(nodep, "no initial assignment");
        UASSERT_OBJ(!(initp->nextp() && initp->nextp() != nodep), nodep,
                    "initial assignment shouldn't be a list");
        if (!VN_IS(initAssp->lhsp(), VarRef)) {
            return cantUnroll(nodep, "no initial assignment to simple variable");
        }
        //
        // Condition check
        UASSERT_OBJ(!condp->nextp(), nodep, "conditional shouldn't be a list");
        //
        // Assignment of next value check
        const AstAssign* const incAssp = VN_CAST(incp, Assign);
        if (!incAssp) return cantUnroll(nodep, "no increment assignment");
        if (incAssp->nextp()) return cantUnroll(nodep, "multiple increments");

        m_forVarp = VN_AS(initAssp->lhsp(), VarRef)->varp();
        m_forVscp = VN_AS(initAssp->lhsp(), VarRef)->varScopep();
        if (VN_IS(nodep, GenFor) && !m_forVarp->isGenVar()) {
            nodep->v3error("Non-genvar used in generate for: " << m_forVarp->prettyNameQ());
        } else if (!VN_IS(nodep, GenFor) && m_forVarp->isGenVar()) {
            nodep->v3error("Genvar not legal in non-generate for (IEEE 1800-2023 27.4): "
                           << m_forVarp->prettyNameQ() << '\n'
                           << nodep->warnMore()
                           << "... Suggest move for loop upwards to generate-level scope.");
        }
        if (m_generate) V3Const::constifyParamsEdit(initAssp->rhsp());  // rhsp may change

        // This check shouldn't be needed when using V3Simulate
        // however, for repeat loops, the loop variable is auto-generated
        // and the initp statements will reference a variable outside of the initp scope
        // alas, failing to simulate.
        const AstConst* const constInitp = VN_CAST(initAssp->rhsp(), Const);
        if (!constInitp) return cantUnroll(nodep, "non-constant initializer");

        //
        // Now, make sure there's no assignment to this variable in the loop
        m_varModeCheck = true;
        m_varAssignHit = false;
        m_forkHit = false;
        m_ignoreIncp = incp;
        iterateAndNextNull(bodysp);
        iterateAndNextNull(incp);
        m_varModeCheck = false;
        m_ignoreIncp = nullptr;
        if (m_varAssignHit) return cantUnroll(nodep, "genvar assigned *inside* loop");

        if (m_forkHit) return cantUnroll(nodep, "fork inside loop");

        //
        if (m_forVscp) {
            UINFO(8, "   Loop Variable: " << m_forVscp);
        } else {
            UINFO(8, "   Loop Variable: " << m_forVarp);
        }
        if (debug() >= 9) nodep->dumpTree("-   for: ");

        if (!m_generate) {
            const AstAssign* const incpAssign = VN_AS(incp, Assign);
            if (!canSimulate(incpAssign->rhsp())) {
                return cantUnroll(incp, "Unable to simulate increment");
            }
            if (!canSimulate(condp)) return cantUnroll(condp, "Unable to simulate condition");

            // Check whether to we actually want to try and unroll.
            int loops;
            const int limit = v3Global.opt.unrollCountAdjusted(unrollFull, m_generate, false);
            if (!countLoops(initAssp, condp, incp, limit, loops)) {
                return cantUnroll(nodep, "Unable to simulate loop");
            }

            // Less than 10 statements in the body?
            if (!unrollFull.isSetTrue()) {
                int bodySize = 0;
                int bodyLimit = v3Global.opt.unrollStmts();
                if (loops > 0) bodyLimit = v3Global.opt.unrollStmts() / loops;
                if (bodySizeOverRecurse(bodysp, bodySize /*ref*/, bodyLimit)
                    || bodySizeOverRecurse(incp, bodySize /*ref*/, bodyLimit)) {
                    return cantUnroll(nodep, "too many statements");
                }
            }
        }
        // Finally, we can do it
        if (!forUnroller(nodep, unrollFull, initAssp, condp, incp, bodysp)) {
            return cantUnroll(nodep, "Unable to unroll loop");
        }
        VL_DANGLING(nodep);
        // Cleanup
        return true;
    }

    bool canSimulate(AstNode* nodep) {
        SimulateVisitor simvis;
        AstNode* clonep = nodep->cloneTree(true);
        simvis.mainCheckTree(clonep);
        VL_DO_CLEAR(pushDeletep(clonep), clonep = nullptr);
        return simvis.optimizable();
    }

    bool simulateTree(AstNode* nodep, const V3Number* loopValue, AstNode* dtypep,
                      V3Number& outNum) {
        AstNode* clonep = nodep->cloneTree(true);
        UASSERT_OBJ(clonep, nodep, "Failed to clone tree");
        if (loopValue) {
            AstConst* varValuep = new AstConst{nodep->fileline(), *loopValue};
            // Iteration requires a back, so put under temporary node
            AstBegin* tempp = new AstBegin{nodep->fileline(), "[EditWrapper]", clonep};
            replaceVarRef(tempp->stmtsp(), varValuep);
            clonep = tempp->stmtsp()->unlinkFrBackWithNext();
            VL_DO_CLEAR(tempp->deleteTree(), tempp = nullptr);
            VL_DO_DANGLING(pushDeletep(varValuep), varValuep);
        }
        SimulateVisitor simvis;
        simvis.mainParamEmulate(clonep);
        if (!simvis.optimizable()) {
            UINFO(4, "Unable to simulate");
            if (debug() >= 9) nodep->dumpTree("-  _simtree: ");
            VL_DO_DANGLING(clonep->deleteTree(), clonep);
            return false;
        }
        // Fetch the result
        V3Number* resp = simvis.fetchNumberNull(clonep);
        if (!resp) {
            UINFO(3, "No number returned from simulation");
            VL_DO_DANGLING(clonep->deleteTree(), clonep);
            return false;
        }
        // Patch up datatype
        if (dtypep) {
            AstConst new_con{clonep->fileline(), *resp};
            new_con.dtypeFrom(dtypep);
            outNum = new_con.num();
            outNum.isSigned(dtypep->isSigned());
            VL_DO_DANGLING(clonep->deleteTree(), clonep);
            return true;
        }
        outNum = *resp;
        VL_DO_DANGLING(clonep->deleteTree(), clonep);
        return true;
    }

    bool countLoops(AstAssign* initp, AstNode* condp, AstNode* incp, int max, int& outLoopsr) {
        outLoopsr = 0;
        V3Number loopValue{initp};
        if (!simulateTree(initp->rhsp(), nullptr, initp, loopValue)) {  //
            return false;
        }
        while (true) {
            V3Number res{initp};
            if (!simulateTree(condp, &loopValue, nullptr, res)) {  //
                return false;
            }
            if (!res.isEqOne()) break;

            outLoopsr++;

            // Run inc
            AstAssign* const incpass = VN_AS(incp, Assign);
            V3Number newLoopValue{initp};
            if (!simulateTree(incpass->rhsp(), &loopValue, incpass, newLoopValue)) {
                return false;
            }
            loopValue.opAssign(newLoopValue);
            if (outLoopsr > max) return false;
        }
        return true;
    }

    bool forUnroller(AstNode* nodep, const VOptionBool& unrollFull, AstAssign* initp,
                     AstNode* condp, AstNode* incp, AstNode* bodysp) {
        UINFO(9, "forUnroller " << nodep);
        V3Number loopValue{nodep};
        if (!simulateTree(initp->rhsp(), nullptr, initp, loopValue)) {  //
            return false;
        }
        AstNode* stmtsp = nullptr;
        if (initp) {
            initp->unlinkFrBack();  // Always a single statement; nextp() may be nodep
            // Don't add to list, we do it once, and setting loop index isn't
            // needed if we have > 1 loop, as we're constant propagating it
        }
        if (bodysp) {
            bodysp->unlinkFrBackWithNext();
            stmtsp = AstNode::addNext(stmtsp, bodysp);  // Maybe null if no body
        }
        if (incp && !VN_IS(nodep, GenFor)) {  // Generates don't need to increment loop index
            incp->unlinkFrBackWithNext();
            stmtsp = AstNode::addNext(stmtsp, incp);  // Maybe null if no body
        }
        // Mark variable to disable some later warnings
        m_forVarp->usedLoopIdx(true);

        ++m_statLoops;
        AstNode* newbodysp = nullptr;
        if (initp && !m_generate) {  // Set variable to initial value (may optimize away later)
            AstNode* clonep = initp->cloneTree(true);
            AstConst* varValuep = new AstConst{nodep->fileline(), loopValue};
            // Iteration requires a back, so put under temporary node
            AstBegin* tempp = new AstBegin{nodep->fileline(), "[EditWrapper]", clonep};
            replaceVarRef(clonep, varValuep);
            clonep = tempp->stmtsp()->unlinkFrBackWithNext();
            VL_DO_CLEAR(tempp->deleteTree(), tempp = nullptr);
            VL_DO_DANGLING(pushDeletep(varValuep), varValuep);
            newbodysp = clonep;
        }
        if (stmtsp) {
            int times = 0;
            while (true) {
                UINFO(8, "      Looping " << loopValue);
                V3Number res{nodep};
                if (!simulateTree(condp, &loopValue, nullptr, res)) {
                    nodep->v3error("Loop unrolling failed.");
                    return false;
                }
                if (!res.isEqOne()) {
                    break;  // Done with the loop
                } else {
                    // Replace iterator values with constant
                    AstNode* oneloopp = stmtsp->cloneTree(true);
                    AstConst* varValuep = new AstConst{nodep->fileline(), loopValue};
                    if (oneloopp) {
                        // Iteration requires a back, so put under temporary node
                        AstBegin* const tempp
                            = new AstBegin{oneloopp->fileline(), "[EditWrapper]", oneloopp};
                        replaceVarRef(tempp->stmtsp(), varValuep);
                        oneloopp = tempp->stmtsp()->unlinkFrBackWithNext();
                        VL_DO_DANGLING(tempp->deleteTree(), tempp);
                    }
                    if (m_generate) {
                        const string index = AstNode::encodeNumber(varValuep->toSInt());
                        const string nname = m_beginName + "__BRA__" + index + "__KET__";
                        oneloopp = new AstBegin{oneloopp->fileline(), nname, oneloopp, true};
                    }
                    VL_DO_DANGLING(pushDeletep(varValuep), varValuep);
                    if (newbodysp) {
                        newbodysp->addNext(oneloopp);
                    } else {
                        newbodysp = oneloopp;
                    }

                    ++m_statIters;
                    const int limit
                        = v3Global.opt.unrollCountAdjusted(unrollFull, m_generate, false);
                    if (++times / 3 > limit) {
                        nodep->v3error(
                            "Loop unrolling took too long;"
                            " probably this is an infinite loop, "
                            " or use /*verilator unroll_full*/, or set --unroll-count above "
                            << times);
                        break;
                    }

                    // loopValue += valInc
                    AstAssign* const incpass = VN_AS(incp, Assign);
                    V3Number newLoopValue{nodep};
                    if (!simulateTree(incpass->rhsp(), &loopValue, incpass, newLoopValue)) {
                        nodep->v3error("Loop unrolling failed");
                        return false;
                    }
                    loopValue.opAssign(newLoopValue);
                }
            }
        }
        if (!newbodysp) {  // initp might have effects after the loop
            if (m_generate && initp) {  // GENFOR(ASSIGN(...)) need to move under a new Initial
                newbodysp = new AstInitial{initp->fileline(), initp};
            } else {
                newbodysp = initp;  // Maybe nullptr
            }
            initp = nullptr;
        }
        // Replace the FOR()
        if (newbodysp) {
            nodep->replaceWith(newbodysp);
        } else {
            nodep->unlinkFrBack();
        }
        if (bodysp) VL_DO_DANGLING(pushDeletep(bodysp), bodysp);
        if (initp) VL_DO_DANGLING(pushDeletep(initp), initp);
        if (incp && !incp->backp()) VL_DO_DANGLING(pushDeletep(incp), incp);
        if (debug() >= 9 && newbodysp) newbodysp->dumpTree("-  _new: ");
        return true;
    }

    // VISITORS
    void visit(AstWhile* nodep) override {
        iterateChildren(nodep);
        if (!m_varModeCheck) {
            // Constify before unroll call, as it may change what is underneath.
            if (nodep->condp()) V3Const::constifyEdit(nodep->condp());  // condp may change
            // Grab initial value
            AstNode* initp = nullptr;  // Should be statement before the while.
            if (nodep->backp()->nextp() == nodep) initp = nodep->backp();
            if (initp) VL_DO_DANGLING(V3Const::constifyEdit(initp), initp);
            if (nodep->backp()->nextp() == nodep) initp = nodep->backp();
            // Grab assignment
            AstNode* incp = nullptr;  // Should be last statement
            AstNode* stmtsp = nodep->stmtsp();
            if (nodep->incsp()) V3Const::constifyEdit(nodep->incsp());
            // cppcheck-suppress duplicateCondition
            if (nodep->incsp()) {
                incp = nodep->incsp();
            } else {
                for (incp = nodep->stmtsp(); incp && incp->nextp(); incp = incp->nextp()) {}
                if (incp) VL_DO_DANGLING(V3Const::constifyEdit(incp), incp);
                // Again, as may have changed
                stmtsp = nodep->stmtsp();
                for (incp = nodep->stmtsp(); incp && incp->nextp(); incp = incp->nextp()) {}
                if (incp == stmtsp) stmtsp = nullptr;
            }
            // And check it
            if (forUnrollCheck(nodep, nodep->unrollFull(), initp, nodep->condp(), incp, stmtsp)) {
                VL_DO_DANGLING(pushDeletep(nodep), nodep);  // Did replacement
            }
        }
    }
    void visit(AstGenFor* nodep) override {
        if (!m_generate) {
            iterateChildren(nodep);
        }  // else V3Param will recursively call each for loop to be unrolled for us
        if (!m_varModeCheck) {
            // Constify before unroll call, as it may change what is underneath.
            if (nodep->initsp()) V3Const::constifyEdit(nodep->initsp());  // initsp may change
            if (nodep->condp()) V3Const::constifyEdit(nodep->condp());  // condp may change
            if (nodep->incsp()) V3Const::constifyEdit(nodep->incsp());  // incsp may change
            if (nodep->condp()->isZero()) {
                // We don't need to do any loops.  Remove the GenFor,
                // Genvar's don't care about any initial assignments.
                //
                // Note normal For's can't do exactly this deletion, as
                // we'd need to initialize the variable to the initial
                // condition, but they'll become while's which can be
                // deleted by V3Const.
                VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
            } else if (forUnrollCheck(nodep, VOptionBool{}, nodep->initsp(), nodep->condp(),
                                      nodep->incsp(), nodep->stmtsp())) {
                VL_DO_DANGLING(pushDeletep(nodep), nodep);  // Did replacement
            } else {
                nodep->v3error("For loop doesn't have genvar index, or is malformed");
            }
        }
    }
    void visit(AstNodeFor* nodep) override {
        if (m_generate) {  // Ignore for's when expanding genfor's
            iterateChildren(nodep);
        } else {
            nodep->v3fatalSrc("V3Begin should have removed standard FORs");
        }
    }

    void visit(AstVarRef* nodep) override {
        if (m_varModeCheck && nodep->varp() == m_forVarp && nodep->varScopep() == m_forVscp
            && nodep->access().isWriteOrRW()) {
            UINFO(8, "   Itervar assigned to: " << nodep);
            m_varAssignHit = true;
        }
    }

    void visit(AstFork* nodep) override {
        if (m_varModeCheck) {
            if (nodep->joinType().joinNone() || nodep->joinType().joinAny()) {
                // Forks are not allowed to unroll for loops, so we just set a flag
                m_forkHit = true;
            }
        } else {
            iterateChildren(nodep);
        }
    }

    void visit(AstNode* nodep) override {
        if (m_varModeCheck && nodep == m_ignoreIncp) {
            // Ignore subtree that is the increment
        } else {
            iterateChildren(nodep);
        }
    }

public:
    // CONSTRUCTORS
    UnrollVisitor() { init(false, ""); }
    ~UnrollVisitor() override {
        V3Stats::addStatSum("Optimizations, Unrolled Loops", m_statLoops);
        V3Stats::addStatSum("Optimizations, Unrolled Iterations", m_statIters);
    }
    // METHODS
    void init(bool generate, const string& beginName) {
        m_forVarp = nullptr;
        m_forVscp = nullptr;
        m_ignoreIncp = nullptr;
        m_varModeCheck = false;
        m_varAssignHit = false;
        m_forkHit = false;
        m_generate = generate;
        m_beginName = beginName;
    }
    void process(AstNode* nodep, bool generate, const string& beginName) {
        init(generate, beginName);
        iterate(nodep);
    }
};

//######################################################################
// Unroll class functions

UnrollStateful::UnrollStateful()
    : m_unrollerp{new UnrollVisitor} {}
UnrollStateful::~UnrollStateful() { delete m_unrollerp; }

void UnrollStateful::unrollGen(AstNodeFor* nodep, const string& beginName) {
    UINFO(5, __FUNCTION__ << ": ");
    m_unrollerp->process(nodep, true, beginName);
}

void UnrollStateful::unrollAll(AstNetlist* nodep) { m_unrollerp->process(nodep, false, ""); }

void V3Unroll::unrollAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    {
        UnrollStateful unroller;
        unroller.unrollAll(nodep);
    }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("unroll", 0, dumpTreeEitherLevel() >= 3);
}
