// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Replicate modules for parameterization
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
// PARAM TRANSFORMATIONS:
//   Top down traversal:
//      For each cell:
//          If parameterized,
//              Determine all parameter widths, constant values.
//              (Interfaces also matter, as if a module is parameterized
//              this effectively changes the width behavior of all that
//              reference the iface.)
//
//              Clone module cell calls, renaming with __{par1}_{par2}_...
//              Substitute constants for cell's module's parameters.
//              Relink pins and cell and ifacerefdtype to point to new module.
//
//              For interface Parent's we have the AstIfaceRefDType::cellp()
//              pointing to this module.  If that parent cell's interface
//              module gets parameterized, AstIfaceRefDType::cloneRelink
//              will update AstIfaceRefDType::cellp(), and V3LinkDot will
//              see the new interface.
//
//              However if a submodule's AstIfaceRefDType::ifacep() points
//              to the old (unparameterized) interface and needs correction.
//              To detect this we must walk all pins looking for interfaces
//              that the parent has changed and propagate down.
//
//          Then process all modules called by that cell.
//          (Cells never referenced after parameters expanded must be ignored.)
//
//   After we complete parameters, the varp's will be wrong (point to old module)
//   and must be relinked.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Param.h"

#include "V3Case.h"
#include "V3Const.h"
#include "V3EmitV.h"
#include "V3Hasher.h"
#include "V3Os.h"
#include "V3Parse.h"
#include "V3Unroll.h"
#include "V3Width.h"

#include <cctype>
#include <deque>
#include <memory>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Hierarchical block and parameter db (modules without parameters are also handled)

class ParameterizedHierBlocks final {
    using HierBlockOptsByOrigName = std::multimap<std::string, const V3HierarchicalBlockOption*>;
    using HierMapIt = HierBlockOptsByOrigName::const_iterator;
    using HierBlockModMap = std::map<const std::string, AstNodeModule*>;
    using ParamConstMap = std::map<const std::string, std::unique_ptr<AstConst>>;
    using GParamsMap = std::map<const std::string, AstVar*>;  // key:parameter name value:parameter

    // MEMBERS
    const bool m_hierSubRun;  // Is in sub-run for hierarchical verilation
    // key:Original module name, value:HiearchyBlockOption*
    // If a module is parameterized, the module is uniquified to overridden parameters.
    // This is why HierBlockOptsByOrigName is multimap.
    HierBlockOptsByOrigName m_hierBlockOptsByOrigName;
    // key:mangled module name, value:AstNodeModule*
    HierBlockModMap m_hierBlockMod;
    // Overridden parameters of the hierarchical block
    std::map<const V3HierarchicalBlockOption*, ParamConstMap> m_hierParams;
    std::map<const std::string, GParamsMap>
        m_modParams;  // Parameter variables of hierarchical blocks

    // METHODS

public:
    ParameterizedHierBlocks(const V3HierBlockOptSet& hierOpts, AstNetlist* nodep)
        : m_hierSubRun((!v3Global.opt.hierBlocks().empty() || v3Global.opt.hierChild())
                       // Exclude consolidation
                       && !v3Global.opt.hierParamFile().empty()) {
        for (const auto& hierOpt : hierOpts) {
            m_hierBlockOptsByOrigName.emplace(hierOpt.second.origName(), &hierOpt.second);
            const V3HierarchicalBlockOption::ParamStrMap& params = hierOpt.second.params();
            ParamConstMap& consts = m_hierParams[&hierOpt.second];
            for (V3HierarchicalBlockOption::ParamStrMap::const_iterator pIt = params.begin();
                 pIt != params.end(); ++pIt) {
                std::unique_ptr<AstConst> constp{AstConst::parseParamLiteral(
                    new FileLine{FileLine::builtInFilename()}, pIt->second)};
                UASSERT(constp, pIt->second << " is not a valid parameter literal");
                const bool inserted = consts.emplace(pIt->first, std::move(constp)).second;
                UASSERT(inserted, pIt->first << " is already added");
            }
            // origName may be already registered, but it's fine.
            m_modParams.insert({hierOpt.second.origName(), {}});
        }
        for (AstNodeModule* modp = nodep->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            if (hierOpts.find(modp->prettyName()) != hierOpts.end()) {
                m_hierBlockMod.emplace(modp->name(), modp);
            }
            // Recursive hierarchical module may change its name, so we have to match its origName.
            // Collect values from recursive cloned module as parameters in the top module could be
            // overridden.
            const string actualModName = modp->recursiveClone() ? modp->origName() : modp->name();
            const auto defParamIt = m_modParams.find(actualModName);
            if (defParamIt != m_modParams.end()) {
                // modp is the original of parameterized hierarchical block
                for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                    if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                        if (varp->isGParam()) defParamIt->second.emplace(varp->name(), varp);
                    }
                }
            }
        }
    }
    bool hierSubRun() const { return m_hierSubRun; }
    bool isHierBlock(const string& origName) const {
        return m_hierBlockOptsByOrigName.find(origName) != m_hierBlockOptsByOrigName.end();
    }
    AstNodeModule* findByParams(const string& origName, AstPin* firstPinp,
                                const AstNodeModule* modp) {
        UASSERT(isHierBlock(origName), origName << " is not hierarchical block\n");
        // This module is a hierarchical block. Need to replace it by the --lib-create wrapper.
        const std::pair<HierMapIt, HierMapIt> candidates
            = m_hierBlockOptsByOrigName.equal_range(origName);
        const auto paramsIt = m_modParams.find(origName);
        UASSERT_OBJ(paramsIt != m_modParams.end(), modp, origName << " must be registered");
        HierMapIt hierIt;
        for (hierIt = candidates.first; hierIt != candidates.second; ++hierIt) {
            bool found = true;
            size_t paramIdx = 0;
            const ParamConstMap& params = m_hierParams[hierIt->second];
            UASSERT(params.size() == hierIt->second->params().size(), "not match");
            for (AstPin* pinp = firstPinp; pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                if (!pinp->exprp()) continue;
                if (const AstVar* const modvarp = pinp->modVarp()) {
                    AstConst* const constp = VN_AS(pinp->exprp(), Const);
                    UASSERT_OBJ(constp, pinp,
                                "parameter for a hierarchical block must have been constified");
                    const auto paramIt = paramsIt->second.find(modvarp->name());
                    UASSERT_OBJ(paramIt != paramsIt->second.end(), modvarp, "must be registered");
                    AstConst* const defValuep = VN_CAST(paramIt->second->valuep(), Const);
                    if (defValuep && areSame(constp, defValuep)) {
                        UINFO(5, "Setting default value of " << constp << " to " << modvarp);
                        continue;  // Skip this parameter because setting the same value
                    }
                    const auto pIt = vlstd::as_const(params).find(modvarp->name());
                    UINFO(5, "Comparing " << modvarp->name() << " " << constp);
                    if (pIt == params.end() || paramIdx >= params.size()
                        || !areSame(constp, pIt->second.get())) {
                        found = false;
                        break;
                    }
                    UINFO(5, "Matched " << modvarp->name() << " " << constp << " and "
                                        << pIt->second.get());
                    ++paramIdx;
                }
            }
            if (found && paramIdx == hierIt->second->params().size()) break;
        }
        UASSERT_OBJ(hierIt != candidates.second, firstPinp, "No --lib-create wrapper found");
        // parameter settings will be removed in the bottom of caller visitCell().
        const HierBlockModMap::const_iterator modIt
            = m_hierBlockMod.find(hierIt->second->mangledName());
        UASSERT_OBJ(modIt != m_hierBlockMod.end(), firstPinp,
                    hierIt->second->mangledName() << " is not found");

        const auto it = vlstd::as_const(m_hierBlockMod).find(hierIt->second->mangledName());
        if (it == m_hierBlockMod.end()) return nullptr;
        return it->second;
    }
    static bool areSame(AstConst* pinValuep, AstConst* hierOptParamp) {
        if (pinValuep->isString()) {
            return pinValuep->num().toString() == hierOptParamp->num().toString();
        }

        if (hierOptParamp->isDouble()) {
            double var;
            if (pinValuep->isDouble()) {
                var = pinValuep->num().toDouble();
            } else {  // Cast from integer to real
                V3Number varNum{pinValuep, 0.0};
                varNum.opIToRD(pinValuep->num());
                var = varNum.toDouble();
            }
            return V3Number::epsilonEqual(var, hierOptParamp->num().toDouble());
        } else {  // Now integer type is assumed
            // Bitwidth of hierOptParamp is accurate because V3Width already calculated in the
            // previous run. Bitwidth of pinValuep is before width analysis, so pinValuep is casted
            // to hierOptParamp width.
            V3Number varNum{pinValuep, hierOptParamp->num().width()};
            if (pinValuep->isDouble()) {  // Need to cast to int
                // Parameter is actually an integral type, but passed value is floating point.
                // Conversion from real to integer uses rounding in V3Width.cpp
                varNum.opRToIRoundS(pinValuep->num());
            } else if (pinValuep->isSigned()) {
                varNum.opExtendS(pinValuep->num(), pinValuep->num().width());
            } else {
                varNum.opAssign(pinValuep->num());
            }
            V3Number isEq{pinValuep, 1};
            isEq.opEq(varNum, hierOptParamp->num());
            return isEq.isNeqZero();
        }
    }
};

//######################################################################
// Remove parameters from cells and build new modules

class ParamProcessor final {
    // NODE STATE - Local
    //   AstVar::user3()        // int    Global parameter number (for naming new module)
    //                          //        (0=not processed, 1=iterated, but no number,
    //                          //         65+ parameter numbered)
    // NODE STATE - Shared with ParamVisitor
    //   AstNodeModule::user3p()  // AstNodeModule* Unaltered copy of the parameterized module.
    //                               The module/class node may be modified according to parameter
    //                               values and an unchanged copy is needed to instantiate
    //                               modules/classes with different parameters.
    //   AstNodeModule::user2() // bool   True if processed
    //   AstGenFor::user2()     // bool   True if processed
    //   AstVar::user2()        // bool   True if constant propagated
    //   AstCell::user2p()      // string* Generate portion of hierarchical name
    const VNUser2InUse m_inuser2;
    const VNUser3InUse m_inuser3;
    // User1 used by constant function simulations

    // TYPES
    // Note may have duplicate entries
    using IfaceRefRefs = std::deque<std::pair<AstIfaceRefDType*, AstIfaceRefDType*>>;

    // STATE
    using CloneMap = std::unordered_map<const AstNode*, AstNode*>;
    struct ModInfo final {
        AstNodeModule* const m_modp;  // Module with specified name
        CloneMap m_cloneMap;  // Map of old-varp -> new cloned varp
        explicit ModInfo(AstNodeModule* modp)
            : m_modp{modp} {}
    };
    std::map<const std::string, ModInfo> m_modNameMap;  // Hash of created module flavors by name

    std::map<const std::string, std::string>
        m_longMap;  // Hash of very long names to unique identity number
    int m_longId = 0;

    // All module names that are loaded from source code
    // Generated modules by this visitor is not included
    V3StringSet m_allModuleNames;

    CloneMap m_originalParams;  // Map between parameters of copied parameteized classes and their
                                // original nodes

    std::map<const V3Hash, int> m_valueMap;  // Hash of node hash to param value
    int m_nextValue = 1;  // Next value to use in m_valueMap

    const AstNodeModule* m_modp = nullptr;  // Current module being processed

    // Database to get lib-create wrapper that matches parameters in hierarchical Verilation
    ParameterizedHierBlocks m_hierBlocks;
    // Default parameter values key:parameter name, value:default value (can be nullptr)
    using DefaultValueMap = std::map<std::string, AstNode*>;
    // Default parameter values of hierarchical blocks
    std::map<AstNodeModule*, DefaultValueMap> m_defaultParameterValues;
    VNDeleter m_deleter;  // Used to delay deletion of nodes

    // METHODS

    static void makeSmallNames(AstNodeModule* modp) {
        std::vector<int> usedLetter;
        usedLetter.resize(256);
        // Pass 1, assign first letter to each gparam's name
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (varp->isGParam() || varp->isIfaceRef()) {
                    char ch = varp->name()[0];
                    ch = std::toupper(ch);
                    if (ch < 'A' || ch > 'Z') ch = 'Z';
                    varp->user3(usedLetter[static_cast<int>(ch)] * 256 + ch);
                    usedLetter[static_cast<int>(ch)]++;
                }
            } else if (AstParamTypeDType* const typep = VN_CAST(stmtp, ParamTypeDType)) {
                const char ch = 'T';
                typep->user3(usedLetter[static_cast<int>(ch)] * 256 + ch);
                usedLetter[static_cast<int>(ch)]++;
            }
        }
    }
    static string paramSmallName(AstNodeModule* modp, AstNode* varp) {
        if (varp->user3() <= 1) makeSmallNames(modp);
        int index = varp->user3() / 256;
        const char ch = varp->user3() & 255;
        string st = cvtToStr(ch);
        while (index) {
            st += cvtToStr(char((index % 25) + 'A'));
            index /= 26;
        }
        return st;
    }

    static string paramValueString(const AstNode* nodep) {
        if (const AstRefDType* const refp = VN_CAST(nodep, RefDType)) {
            nodep = refp->skipRefToNonRefp();
        }
        string key = nodep->name();
        if (const AstIfaceRefDType* const ifrtp = VN_CAST(nodep, IfaceRefDType)) {
            if (ifrtp->cellp() && ifrtp->cellp()->modp()) {
                key = ifrtp->cellp()->modp()->name();
            } else if (ifrtp->ifacep()) {
                key = ifrtp->ifacep()->name();
            } else {
                nodep->v3fatalSrc("Can't parameterize interface without module name");
            }
        } else if (const AstNodeUOrStructDType* const dtypep
                   = VN_CAST(nodep, NodeUOrStructDType)) {
            key += " ";
            key += dtypep->verilogKwd();
            key += " {";
            for (const AstNode* memberp = dtypep->membersp(); memberp;
                 memberp = memberp->nextp()) {
                key += paramValueString(memberp);
                key += ";";
            }
            key += "}";
        } else if (const AstMemberDType* const dtypep = VN_CAST(nodep, MemberDType)) {
            key += " ";
            key += paramValueString(dtypep->subDTypep());
        } else if (const AstBasicDType* const dtypep = VN_CAST(nodep, BasicDType)) {
            if (dtypep->isSigned()) key += " signed";
            if (dtypep->isRanged()) {
                key += "[" + cvtToStr(dtypep->left()) + ":" + cvtToStr(dtypep->right()) + "]";
            }
        } else if (const AstPackArrayDType* const dtypep = VN_CAST(nodep, PackArrayDType)) {
            key += "[";
            key += cvtToStr(dtypep->left());
            key += ":";
            key += cvtToStr(dtypep->right());
            key += "] ";
            key += paramValueString(dtypep->subDTypep());
        } else if (const AstInitArray* const initp = VN_CAST(nodep, InitArray)) {
            key += "{";
            for (auto it : initp->map()) {
                key += paramValueString(it.second->valuep());
                key += ",";
            }
            key += "}";
        } else if (const AstNodeDType* const dtypep = VN_CAST(nodep, NodeDType)) {
            key += dtypep->prettyDTypeName(true);
        }
        UASSERT_OBJ(!key.empty(), nodep, "Parameter yielded no value string");
        return key;
    }

    string paramValueNumber(AstNode* nodep) {
        // TODO: This parameter value number lookup via a constructed key string is not
        //       particularly robust for type parameters. We should really have a type
        //       equivalence predicate function.
        if (AstRefDType* const refp = VN_CAST(nodep, RefDType)) nodep = refp->skipRefToNonRefp();
        const string paramStr = paramValueString(nodep);
        // cppcheck-has-bug-suppress unreadVariable
        V3Hash hash = V3Hasher::uncachedHash(nodep) + paramStr;
        // Force hash collisions -- for testing only
        // cppcheck-has-bug-suppress unreadVariable
        if (VL_UNLIKELY(v3Global.opt.debugCollision())) hash = V3Hash{paramStr};
        int num;
        const auto pair = m_valueMap.emplace(hash, 0);
        if (pair.second) pair.first->second = m_nextValue++;
        num = pair.first->second;
        return "z"s + cvtToStr(num);
    }
    string moduleCalcName(const AstNodeModule* srcModp, const string& longname) {
        string newname = longname;
        if (longname.length() > 30) {
            const auto pair = m_longMap.emplace(longname, "");
            if (pair.second) {
                newname = srcModp->name();
                // We use all upper case above, so lower here can't conflict
                newname += "__pi" + cvtToStr(++m_longId);
                pair.first->second = newname;
            }
            newname = pair.first->second;
        }
        UINFO(4, "Name: " << srcModp->name() << "->" << longname << "->" << newname);
        return newname;
    }
    AstNodeDType* arraySubDTypep(AstNodeDType* nodep) {
        // If an unpacked array, return the subDTypep under it
        if (const AstUnpackArrayDType* const adtypep = VN_CAST(nodep, UnpackArrayDType)) {
            return adtypep->subDTypep();
        }
        // We have not resolved parameter of the child yet, so still
        // have BracketArrayDType's. We'll presume it'll end up as assignment
        // compatible (or V3Width will complain).
        if (const AstBracketArrayDType* const adtypep = VN_CAST(nodep, BracketArrayDType)) {
            return adtypep->subDTypep();
        }
        return nullptr;
    }
    bool isString(AstNodeDType* nodep) {
        if (AstBasicDType* const basicp = VN_CAST(nodep->skipRefToNonRefp(), BasicDType))
            return basicp->isString();
        return false;
    }
    void collectPins(CloneMap* clonemapp, AstNodeModule* modp, bool originalIsCopy) {
        // Grab all I/O so we can remap our pins later
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            const AstNode* originalParamp = nullptr;
            if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (varp->isIO() || varp->isGParam() || varp->isIfaceRef()) {
                    // Cloning saved a pointer to the new node for us, so just follow that link.
                    originalParamp = varp->clonep();
                }
            } else if (AstParamTypeDType* const ptp = VN_CAST(stmtp, ParamTypeDType)) {
                if (ptp->isGParam()) originalParamp = ptp->clonep();
            }
            if (originalIsCopy) originalParamp = m_originalParams[originalParamp];
            if (originalParamp) clonemapp->emplace(originalParamp, stmtp);
        }
    }
    void relinkPins(const CloneMap* clonemapp, AstPin* startpinp) {
        for (AstPin* pinp = startpinp; pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
            if (pinp->modVarp()) {
                // Find it in the clone structure
                // UINFO(8,"Clone find 0x"<<hex<<(uint32_t)pinp->modVarp());
                const auto cloneiter = clonemapp->find(pinp->modVarp());
                UASSERT_OBJ(cloneiter != clonemapp->end(), pinp,
                            "Couldn't find pin in clone list");
                pinp->modVarp(VN_AS(cloneiter->second, Var));
            } else if (pinp->modPTypep()) {
                const auto cloneiter = clonemapp->find(pinp->modPTypep());
                UASSERT_OBJ(cloneiter != clonemapp->end(), pinp,
                            "Couldn't find pin in clone list");
                pinp->modPTypep(VN_AS(cloneiter->second, ParamTypeDType));
            } else {
                pinp->v3fatalSrc("Not linked?");
            }
        }
    }
    void relinkPinsByName(AstPin* startpinp, AstNodeModule* modp) {
        std::map<const string, AstVar*> nameToPin;
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (varp->isIO() || varp->isGParam() || varp->isIfaceRef()) {
                    nameToPin.emplace(varp->name(), varp);
                }
            }
        }
        for (AstPin* pinp = startpinp; pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
            if (const AstVar* const varp = pinp->modVarp()) {
                const auto varIt = vlstd::as_const(nameToPin).find(varp->name());
                UASSERT_OBJ(varIt != nameToPin.end(), varp,
                            "Not found in " << modp->prettyNameQ());
                pinp->modVarp(varIt->second);
            }
        }
    }
    // Check if parameter setting during instantiation is simple enough for hierarchical Verilation
    void checkSupportedParam(AstNodeModule* modp, AstPin* pinp) const {
        // InitArray is not supported because that can not be set via -G
        // option.
        if (pinp->modVarp()) {
            bool supported = false;
            if (const AstConst* const constp = VN_CAST(pinp->exprp(), Const)) {
                supported = !constp->isOpaque();
            }
            if (!supported) {
                pinp->v3error(
                    AstNode::prettyNameQ(modp->origName())
                    << " has hier_block metacomment, hierarchical Verilation"
                    << " supports only integer/floating point/string and type param parameters");
            }
        }
    }
    bool moduleExists(const string& modName) const {
        if (m_allModuleNames.find(modName) != m_allModuleNames.end()) return true;
        if (m_modNameMap.find(modName) != m_modNameMap.end()) return true;
        return false;
    }

    string parameterizedHierBlockName(AstNodeModule* modp, AstPin* paramPinsp) {
        // Create a unique name in the following steps
        //  - Make a long name that includes all parameters, that appear
        //    in the alphabetical order.
        //  - Hash the long name to get valid Verilog symbol
        UASSERT_OBJ(modp->hierBlock(), modp, "should be used for hierarchical block");

        std::map<string, AstNode*> pins;

        AstPin* pinp = paramPinsp;
        while (pinp) {
            checkSupportedParam(modp, pinp);
            if (const AstVar* const varp = pinp->modVarp()) {
                if (!pinp->exprp()) continue;
                if (varp->isGParam()) pins.emplace(varp->name(), pinp->exprp());
            } else if (VN_IS(pinp->exprp(), BasicDType) || VN_IS(pinp->exprp(), NodeDType)) {
                pins.emplace(pinp->name(), pinp->exprp());
            }
            pinp = VN_AS(pinp->nextp(), Pin);
        }

        const auto pair = m_defaultParameterValues.emplace(
            std::piecewise_construct, std::forward_as_tuple(modp), std::forward_as_tuple());
        if (pair.second) {  // Not cached yet, so check parameters
            // Using map with key=string so that we can scan it in deterministic order
            DefaultValueMap params;
            for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                if (const AstVar* const varp = VN_CAST(stmtp, Var)) {
                    if (varp->isGParam()) {
                        AstConst* const constp = VN_CAST(varp->valuep(), Const);
                        // constp can be nullptr if the parameter is not used to instantiate sub
                        // module. varp->valuep() is not constified yet in the case.
                        // nullptr means that the parameter is using some default value.
                        params.emplace(varp->name(), constp);
                    }
                } else if (AstParamTypeDType* const p = VN_CAST(stmtp, ParamTypeDType)) {
                    params.emplace(p->name(), p->skipRefp());
                }
            }
            pair.first->second = std::move(params);
        }
        const auto paramsIt = pair.first;
        if (paramsIt->second.empty()) return modp->origName();  // modp has no parameter

        string longname = modp->origName();
        for (auto&& defaultValue : paramsIt->second) {
            const auto pinIt = pins.find(defaultValue.first);
            // If the pin does not have a value assigned, use the default one.
            const AstNode* const nodep = pinIt == pins.end() ? defaultValue.second : pinIt->second;
            // This longname is not valid as verilog symbol, but ok, because it will be hashed
            longname += "_" + defaultValue.first + "=";
            // constp can be nullptr

            if (const AstConst* const p = VN_CAST(nodep, Const)) {
                // Treat modules parametrized with the same values but with different type as the
                // same.
                longname += p->num().ascii(false);
            } else if (nodep) {
                std::stringstream type;
                V3EmitV::verilogForTree(nodep, type);
                longname += type.str();
            }
        }
        UINFO(9, "       module params longname: " << longname);

        const auto iter = m_longMap.find(longname);
        if (iter != m_longMap.end()) return iter->second;  // Already calculated

        VHashSha256 hash;
        // Calculate hash using longname
        // The hash is used as the module suffix to find a module name that is unique in the design
        hash.insert(longname);
        while (true) {
            // Copy VHashSha256 just in case of hash collision
            VHashSha256 hashStrGen = hash;
            // Hex string must be a safe suffix for any symbol
            const string hashStr = hashStrGen.digestHex();
            for (string::size_type i = 1; i < hashStr.size(); ++i) {
                string newName = modp->origName();
                // Don't use '__' not to be encoded when this module is loaded later by Verilator
                if (newName.at(newName.size() - 1) != '_') newName += '_';
                newName += hashStr.substr(0, i);
                if (!moduleExists(newName)) {
                    m_longMap.emplace(longname, newName);
                    return newName;
                }
            }
            // Hash collision. maybe just v3error is practically enough
            hash.insert(V3Os::trueRandom(64));
        }
    }
    void replaceRefsRecurse(AstNode* const nodep, const AstClass* const oldClassp,
                            AstClass* const newClassp) {
        // Self references linked in the first pass of V3LinkDot.cpp should point to the default
        // instance.
        if (AstClassRefDType* const classRefp = VN_CAST(nodep, ClassRefDType)) {
            if (classRefp->classp() == oldClassp) classRefp->classp(newClassp);
        } else if (AstClassOrPackageRef* const classRefp = VN_CAST(nodep, ClassOrPackageRef)) {
            if (classRefp->classOrPackageSkipp() == oldClassp)
                classRefp->classOrPackagep(newClassp);
        }

        if (nodep->op1p()) replaceRefsRecurse(nodep->op1p(), oldClassp, newClassp);
        if (nodep->op2p()) replaceRefsRecurse(nodep->op2p(), oldClassp, newClassp);
        if (nodep->op3p()) replaceRefsRecurse(nodep->op3p(), oldClassp, newClassp);
        if (nodep->op4p()) replaceRefsRecurse(nodep->op4p(), oldClassp, newClassp);
        if (nodep->nextp()) replaceRefsRecurse(nodep->nextp(), oldClassp, newClassp);
    }
    void deepCloneModule(AstNodeModule* srcModp, AstNode* ifErrorp, AstPin* paramsp,
                         const string& newname, const IfaceRefRefs& ifaceRefRefs) {
        // Deep clone of new module
        // Note all module internal variables will be re-linked to the new modules by clone
        // However links outside the module (like on the upper cells) will not.
        AstNodeModule* newModp;
        if (srcModp->user3p()) {
            newModp = VN_CAST(srcModp->user3p()->cloneTree(false), NodeModule);
        } else {
            newModp = srcModp->cloneTree(false);
        }

        if (AstClass* const newClassp = VN_CAST(newModp, Class)) {
            replaceRefsRecurse(newModp->stmtsp(), newClassp, VN_AS(srcModp, Class));
        }

        newModp->name(newname);
        newModp->user2(false);  // We need to re-recurse this module once changed
        newModp->recursive(false);
        newModp->recursiveClone(false);
        newModp->hasGParam(false);
        // Recursion may need level cleanups
        if (newModp->level() <= m_modp->level()) newModp->level(m_modp->level() + 1);
        if ((newModp->level() - srcModp->level()) >= (v3Global.opt.moduleRecursionDepth() - 2)) {
            ifErrorp->v3error("Exceeded maximum --module-recursion-depth of "
                              << v3Global.opt.moduleRecursionDepth());
            return;
        }
        // Keep tree sorted by level. Note: Different parameterizations of the same recursive
        // module end up with the same level, which we will need to fix up at the end, as we do not
        // know up front how recursive modules are expanded, and a later expansion might re-use an
        // earlier expansion (see t_recursive_module_bug_2).
        AstNode* insertp = srcModp;
        while (VN_IS(insertp->nextp(), NodeModule)
               && VN_AS(insertp->nextp(), NodeModule)->level() <= newModp->level()) {
            insertp = insertp->nextp();
        }
        insertp->addNextHere(newModp);

        m_modNameMap.emplace(newModp->name(), ModInfo{newModp});
        const auto iter = m_modNameMap.find(newname);
        CloneMap* const clonemapp = &(iter->second.m_cloneMap);
        UINFO(4, "     De-parameterize to new: " << newModp);

        // Grab all I/O so we can remap our pins later
        // Note we allow multiple users of a parameterized model,
        // thus we need to stash this info.
        collectPins(clonemapp, newModp, srcModp->user3p());
        // Relink parameter vars to the new module
        relinkPins(clonemapp, paramsp);
        // Fix any interface references
        for (auto it = ifaceRefRefs.cbegin(); it != ifaceRefRefs.cend(); ++it) {
            const AstIfaceRefDType* const portIrefp = it->first;
            const AstIfaceRefDType* const pinIrefp = it->second;
            AstIfaceRefDType* const cloneIrefp = portIrefp->clonep();
            UINFO(8, "     IfaceOld " << portIrefp);
            UINFO(8, "     IfaceTo  " << pinIrefp);
            UASSERT_OBJ(cloneIrefp, portIrefp, "parameter clone didn't hit AstIfaceRefDType");
            UINFO(8, "     IfaceClo " << cloneIrefp);
            cloneIrefp->ifacep(pinIrefp->ifaceViaCellp());
            UINFO(8, "     IfaceNew " << cloneIrefp);
        }
        // Assign parameters to the constants specified
        // DOES clone() so must be finished with module clonep() before here
        for (AstPin* pinp = paramsp; pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
            if (pinp->exprp()) {
                if (AstVar* const modvarp = pinp->modVarp()) {
                    AstNode* const newp = pinp->exprp();  // Const or InitArray
                    AstConst* const exprp = VN_CAST(newp, Const);
                    AstConst* const origp = VN_CAST(modvarp->valuep(), Const);
                    const bool overridden
                        = !(origp && ParameterizedHierBlocks::areSame(exprp, origp));
                    // Remove any existing parameter
                    if (modvarp->valuep()) modvarp->valuep()->unlinkFrBack()->deleteTree();
                    // Set this parameter to value requested by cell
                    UINFO(9, "       set param " << modvarp << " = " << newp);
                    modvarp->valuep(newp->cloneTree(false));
                    modvarp->overriddenParam(overridden);
                } else if (AstParamTypeDType* const modptp = pinp->modPTypep()) {
                    AstNodeDType* const dtypep = VN_AS(pinp->exprp(), NodeDType);
                    UASSERT_OBJ(dtypep, pinp, "unlinked param dtype");
                    if (modptp->childDTypep()) modptp->childDTypep()->unlinkFrBack()->deleteTree();
                    // Set this parameter to value requested by cell
                    modptp->childDTypep(dtypep->cloneTree(false));
                    // Later V3LinkDot will convert the ParamDType to a Typedef
                    // Not done here as may be localparams, etc, that also need conversion
                }
            }
        }
    }
    const ModInfo* moduleFindOrClone(AstNodeModule* srcModp, AstNode* ifErrorp, AstPin* paramsp,
                                     const string& newname, const IfaceRefRefs& ifaceRefRefs) {
        // Already made this flavor?
        auto it = m_modNameMap.find(newname);
        if (it != m_modNameMap.end()) {
            UINFO(4, "     De-parameterize to prev: " << it->second.m_modp);
        } else {
            deepCloneModule(srcModp, ifErrorp, paramsp, newname, ifaceRefRefs);
            it = m_modNameMap.find(newname);
            UASSERT(it != m_modNameMap.end(), "should find just-made module");
        }
        const ModInfo* const modInfop = &(it->second);
        return modInfop;
    }

    void convertToStringp(AstNode* nodep) {
        // Should be called on values of parameters of type string to convert them
        // to properly typed string constants.
        // Has no effect if the value is not a string constant.
        AstConst* const constp = VN_CAST(nodep, Const);
        // Check if it wasn't already converted
        if (constp && !constp->num().isString()) {
            constp->replaceWith(
                new AstConst{constp->fileline(), AstConst::String{}, constp->num().toString()});
            constp->deleteTree();
        }
    }

    void cellPinCleanup(AstNode* nodep, AstPin* pinp, AstNodeModule* srcModp, string& longnamer,
                        bool& any_overridesr) {
        if (!pinp->exprp()) return;  // No-connect
        if (AstVar* const modvarp = pinp->modVarp()) {
            if (!modvarp->isGParam()) {
                pinp->v3fatalSrc("Attempted parameter setting of non-parameter: Param "
                                 << pinp->prettyNameQ() << " of " << nodep->prettyNameQ());
            } else if (VN_IS(pinp->exprp(), InitArray) && arraySubDTypep(modvarp->subDTypep())) {
                // Array assigned to array
                AstNode* const exprp = pinp->exprp();
                longnamer += "_" + paramSmallName(srcModp, modvarp) + paramValueNumber(exprp);
                any_overridesr = true;
            } else {
                V3Const::constifyParamsEdit(pinp->exprp());
                // String constants are parsed as logic arrays and converted to strings in V3Const.
                // At this moment, some constants may have been already converted.
                // To correctly compare constants, both should be of the same type,
                // so they need to be converted.
                if (isString(modvarp->subDTypep())) {
                    convertToStringp(pinp->exprp());
                    convertToStringp(modvarp->valuep());
                }
                AstConst* const exprp = VN_CAST(pinp->exprp(), Const);
                AstConst* const origp = VN_CAST(modvarp->valuep(), Const);
                if (!exprp) {
                    if (debug()) pinp->dumpTree("-  ");
                    pinp->v3error("Can't convert defparam value to constant: Param "
                                  << pinp->prettyNameQ() << " of " << nodep->prettyNameQ());
                    pinp->exprp()->replaceWith(new AstConst{
                        pinp->fileline(), AstConst::WidthedValue{}, modvarp->width(), 0});
                } else if (origp && exprp->sameTree(origp)) {
                    // Setting parameter to its default value.  Just ignore it.
                    // This prevents making additional modules, and makes coverage more
                    // obvious as it won't show up under a unique module page name.
                } else if (exprp->num().isDouble() || exprp->num().isString()
                           || exprp->num().isFourState() || exprp->num().width() != 32) {
                    longnamer
                        += ("_" + paramSmallName(srcModp, modvarp) + paramValueNumber(exprp));
                    any_overridesr = true;
                } else {
                    longnamer
                        += ("_" + paramSmallName(srcModp, modvarp) + exprp->num().ascii(false));
                    any_overridesr = true;
                }
            }
        } else if (AstParamTypeDType* const modvarp = pinp->modPTypep()) {
            AstNodeDType* rawTypep = VN_CAST(pinp->exprp(), NodeDType);
            if (rawTypep) V3Width::widthParamsEdit(rawTypep);
            AstNodeDType* exprp = rawTypep ? rawTypep->skipRefToNonRefp() : nullptr;
            const AstNodeDType* const origp = modvarp->skipRefToNonRefp();
            if (!exprp) {
                pinp->v3error("Parameter type pin value isn't a type: Param "
                              << pinp->prettyNameQ() << " of " << nodep->prettyNameQ());
            } else if (!origp) {
                pinp->v3error("Parameter type variable isn't a type: Param "
                              << modvarp->prettyNameQ());
            } else {
                UINFO(9, "Parameter type assignment expr=" << exprp << " to " << origp);
                V3Const::constifyParamsEdit(pinp->exprp());  // Reconcile typedefs
                // Constify may have caused pinp->exprp to change
                rawTypep = VN_AS(pinp->exprp(), NodeDType);
                exprp = rawTypep->skipRefToNonRefp();
                if (!modvarp->fwdType().isNodeCompatible(exprp)) {
                    pinp->v3error("Parameter type expression type "
                                  << exprp->prettyDTypeNameQ()
                                  << " violates parameter's forwarding type '"
                                  << modvarp->fwdType().ascii() << "'");
                }
                if (exprp->similarDType(origp)) {
                    // Setting parameter to its default value.  Just ignore it.
                    // This prevents making additional modules, and makes coverage more
                    // obvious as it won't show up under a unique module page name.
                } else {
                    VL_DO_DANGLING(V3Const::constifyParamsEdit(exprp), exprp);
                    rawTypep = VN_CAST(pinp->exprp(), NodeDType);
                    exprp = rawTypep ? rawTypep->skipRefToNonRefp() : nullptr;
                    longnamer += "_" + paramSmallName(srcModp, modvarp) + paramValueNumber(exprp);
                    any_overridesr = true;
                }
            }
        } else {
            pinp->v3fatalSrc("Parameter not found in sub-module: Param "
                             << pinp->prettyNameQ() << " of " << nodep->prettyNameQ());
        }
    }

    void cellInterfaceCleanup(AstPin* pinsp, AstNodeModule* srcModp, string& longnamer,
                              bool& any_overridesr, IfaceRefRefs& ifaceRefRefs) {
        for (AstPin* pinp = pinsp; pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
            const AstVar* const modvarp = pinp->modVarp();
            if (modvarp->isIfaceRef()) {
                AstIfaceRefDType* portIrefp = VN_CAST(modvarp->subDTypep(), IfaceRefDType);
                if (!portIrefp && arraySubDTypep(modvarp->subDTypep())) {
                    portIrefp = VN_CAST(arraySubDTypep(modvarp->subDTypep()), IfaceRefDType);
                }
                AstIfaceRefDType* pinIrefp = nullptr;
                const AstNode* const exprp = pinp->exprp();
                const AstVar* const varp
                    = (exprp && VN_IS(exprp, VarRef)) ? VN_AS(exprp, VarRef)->varp() : nullptr;
                if (varp && varp->subDTypep() && VN_IS(varp->subDTypep(), IfaceRefDType)) {
                    pinIrefp = VN_AS(varp->subDTypep(), IfaceRefDType);
                } else if (varp && varp->subDTypep() && arraySubDTypep(varp->subDTypep())
                           && VN_CAST(arraySubDTypep(varp->subDTypep()), IfaceRefDType)) {
                    pinIrefp = VN_CAST(arraySubDTypep(varp->subDTypep()), IfaceRefDType);
                } else if (exprp && exprp->op1p() && VN_IS(exprp->op1p(), VarRef)
                           && VN_CAST(exprp->op1p(), VarRef)->varp()
                           && VN_CAST(exprp->op1p(), VarRef)->varp()->subDTypep()
                           && arraySubDTypep(VN_CAST(exprp->op1p(), VarRef)->varp()->subDTypep())
                           && VN_CAST(
                               arraySubDTypep(VN_CAST(exprp->op1p(), VarRef)->varp()->subDTypep()),
                               IfaceRefDType)) {
                    pinIrefp
                        = VN_AS(arraySubDTypep(VN_AS(exprp->op1p(), VarRef)->varp()->subDTypep()),
                                IfaceRefDType);
                }

                UINFO(9, "     portIfaceRef " << portIrefp);

                if (!portIrefp) {
                    pinp->v3error("Interface port " << modvarp->prettyNameQ()
                                                    << " is not an interface " << modvarp);
                } else if (!pinIrefp) {
                    pinp->v3error("Interface port "
                                  << modvarp->prettyNameQ()
                                  << " is not connected to interface/modport pin expression");
                } else {
                    UINFO(9, "     pinIfaceRef " << pinIrefp);
                    if (portIrefp->ifaceViaCellp() != pinIrefp->ifaceViaCellp()) {
                        UINFO(9, "     IfaceRefDType needs reconnect  " << pinIrefp);
                        longnamer += ("_" + paramSmallName(srcModp, pinp->modVarp())
                                      + paramValueNumber(pinIrefp));
                        any_overridesr = true;
                        ifaceRefRefs.emplace_back(portIrefp, pinIrefp);
                        if (portIrefp->ifacep() != pinIrefp->ifacep()
                            // Might be different only due to param cloning, so check names too
                            && portIrefp->ifaceName() != pinIrefp->ifaceName()) {
                            pinp->v3error("Port " << pinp->prettyNameQ() << " expects "
                                                  << AstNode::prettyNameQ(portIrefp->ifaceName())
                                                  << " interface but pin connects "
                                                  << AstNode::prettyNameQ(pinIrefp->ifaceName())
                                                  << " interface");
                        }
                    }
                }
            }
        }
    }

    void storeOriginalParams(AstNodeModule* const classp) {
        for (AstNode* stmtp = classp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstNode* originalParamp = nullptr;
            if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (varp->isGParam()) originalParamp = varp->clonep();
            } else if (AstParamTypeDType* const ptp = VN_CAST(stmtp, ParamTypeDType)) {
                if (ptp->isGParam()) originalParamp = ptp->clonep();
            }
            if (originalParamp) m_originalParams[stmtp] = originalParamp;
        }
    }

    bool nodeDeparamCommon(AstNode* nodep, AstNodeModule*& srcModpr, AstPin* paramsp,
                           AstPin* pinsp, bool any_overrides) {
        // Make sure constification worked
        // Must be a separate loop, as constant conversion may have changed some pointers.
        // if (debug()) nodep->dumpTree("-  cel2: ");
        string longname = srcModpr->name() + "_";
        if (debug() > 8 && paramsp) paramsp->dumpTreeAndNext(cout, "-  cellparams: ");

        if (srcModpr->hierBlock()) {
            longname = parameterizedHierBlockName(srcModpr, paramsp);
            any_overrides = longname != srcModpr->name();
        } else {
            for (AstPin* pinp = paramsp; pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                cellPinCleanup(nodep, pinp, srcModpr, longname /*ref*/, any_overrides /*ref*/);
            }
        }
        IfaceRefRefs ifaceRefRefs;
        cellInterfaceCleanup(pinsp, srcModpr, longname /*ref*/, any_overrides /*ref*/,
                             ifaceRefRefs /*ref*/);

        if (m_hierBlocks.hierSubRun() && m_hierBlocks.isHierBlock(srcModpr->origName())) {
            AstNodeModule* const paramedModp
                = m_hierBlocks.findByParams(srcModpr->origName(), paramsp, m_modp);
            UASSERT_OBJ(paramedModp, nodep, "Failed to find sub-module for hierarchical block");
            paramedModp->dead(false);
            // We need to relink the pins to the new module
            relinkPinsByName(pinsp, paramedModp);
            srcModpr = paramedModp;
            any_overrides = true;
        } else if (!any_overrides) {
            UINFO(8, "Cell parameters all match original values, skipping expansion.");
            // If it's the first use of the default instance, create a copy and store it in user3p.
            // user3p will also be used to check if the default instance is used.
            if (!srcModpr->user3p() && (VN_IS(srcModpr, Class) || VN_IS(srcModpr, Iface))) {
                AstNodeModule* nodeCopyp = srcModpr->cloneTree(false);
                // It is a temporary copy of the original class node, stored in order to create
                // another instances. It is needed only during class instantiation.
                m_deleter.pushDeletep(nodeCopyp);
                srcModpr->user3p(nodeCopyp);
                storeOriginalParams(nodeCopyp);
            }
        } else {
            const string newname
                = srcModpr->hierBlock() ? longname : moduleCalcName(srcModpr, longname);
            const ModInfo* const modInfop
                = moduleFindOrClone(srcModpr, nodep, paramsp, newname, ifaceRefRefs);
            // We need to relink the pins to the new module
            relinkPinsByName(pinsp, modInfop->m_modp);
            UINFO(8, "     Done with " << modInfop->m_modp);
            srcModpr = modInfop->m_modp;
        }

        for (auto* stmtp = srcModpr->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstParamTypeDType* dtypep = VN_CAST(stmtp, ParamTypeDType)) {
                if (VN_IS(dtypep->skipRefOrNullp(), VoidDType)) {
                    nodep->v3error(
                        "Class parameter type without default value is never given value"
                        << " (IEEE 1800-2023 6.20.1): " << dtypep->prettyNameQ());
                    VL_DO_DANGLING(nodep->unlinkFrBack()->deleteTree(), nodep);
                }
            }
            if (AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (VN_IS(srcModpr, Class) && varp->isParam() && !varp->valuep()) {
                    nodep->v3error("Class parameter without default value is never given value"
                                   << " (IEEE 1800-2023 6.20.1): " << varp->prettyNameQ());
                }
            }
        }

        // Delete the parameters from the cell; they're not relevant any longer.
        if (paramsp) paramsp->unlinkFrBackWithNext()->deleteTree();
        return any_overrides;
    }

    void cellDeparam(AstCell* nodep, AstNodeModule*& srcModpr) {
        // Must always clone __Vrcm (recursive modules)
        if (nodeDeparamCommon(nodep, srcModpr, nodep->paramsp(), nodep->pinsp(),
                              nodep->recursive())) {
            nodep->modp(srcModpr);
            nodep->modName(srcModpr->name());
        }
        nodep->recursive(false);
    }

    void ifaceRefDeparam(AstIfaceRefDType* const nodep, AstNodeModule*& srcModpr) {
        nodeDeparamCommon(nodep, srcModpr, nodep->paramsp(), nullptr, false);
        nodep->ifacep(VN_AS(srcModpr, Iface));
    }

    void classRefDeparam(AstClassOrPackageRef* nodep, AstNodeModule*& srcModpr) {
        if (nodeDeparamCommon(nodep, srcModpr, nodep->paramsp(), nullptr, false))
            nodep->classOrPackagep(srcModpr);
    }

    void classRefDeparam(AstClassRefDType* nodep, AstNodeModule*& srcModpr) {
        if (nodeDeparamCommon(nodep, srcModpr, nodep->paramsp(), nullptr, false)) {
            AstClass* const classp = VN_AS(srcModpr, Class);
            nodep->classp(classp);
            nodep->classOrPackagep(classp);
        }
    }

public:
    void nodeDeparam(AstNode* nodep, AstNodeModule*& srcModpr, AstNodeModule* modp,
                     const string& someInstanceName) {
        m_modp = modp;
        // Cell: Check for parameters in the instantiation.
        // We always run this, even if no parameters, as need to look for interfaces,
        // and remove any recursive references
        UINFO(4, "De-parameterize: " << nodep);
        // Create new module name with _'s between the constants
        if (debug() >= 10) nodep->dumpTree("-  cell: ");
        // Evaluate all module constants
        V3Const::constifyParamsEdit(nodep);
        // Set name for warnings for when we param propagate the module
        const string instanceName = someInstanceName + "." + nodep->name();
        srcModpr->someInstanceName(instanceName);

        if (auto* cellp = VN_CAST(nodep, Cell)) {
            cellDeparam(cellp, srcModpr);
        } else if (auto* ifaceRefDTypep = VN_CAST(nodep, IfaceRefDType)) {
            ifaceRefDeparam(ifaceRefDTypep, srcModpr);
        } else if (auto* classRefp = VN_CAST(nodep, ClassRefDType)) {
            classRefDeparam(classRefp, srcModpr);
        } else if (auto* classRefp = VN_CAST(nodep, ClassOrPackageRef)) {
            classRefDeparam(classRefp, srcModpr);
        } else {
            nodep->v3fatalSrc("Expected module parameterization");
        }

        // Set name for later warnings (if srcModpr changed value due to cloning)
        srcModpr->someInstanceName(instanceName);

        UINFO(8, "     Done with orig " << nodep);
        // if (debug() >= 10)
        // v3Global.rootp()->dumpTreeFile(v3Global.debugFilename("param-out.tree"));
    }

    // CONSTRUCTORS
    explicit ParamProcessor(AstNetlist* nodep)
        : m_hierBlocks{v3Global.opt.hierBlocks(), nodep} {
        for (AstNodeModule* modp = nodep->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            m_allModuleNames.insert(modp->name());
        }
    }
    ~ParamProcessor() = default;
    VL_UNCOPYABLE(ParamProcessor);
};

//######################################################################
// Process parameter visitor

class ParamVisitor final : public VNVisitor {
    // NODE STATE
    // AstNodeModule::user1 -> bool: already fixed level (temporary)

    // STATE
    ParamProcessor m_processor;  // De-parameterize a cell, build modules
    UnrollStateful m_unroller;  // Loop unroller

    bool m_iterateModule = false;  // Iterating module body
    string m_generateHierName;  // Generate portion of hierarchy name
    string m_unlinkedTxt;  // Text for AstUnlinkedRef
    AstNodeModule* m_modp;  // Module iterating
    std::vector<AstDot*> m_dots;  // Dot references to process
    std::multimap<bool, AstNode*> m_cellps;  // Cells left to process (in current module)
    std::multimap<int, AstNodeModule*> m_workQueue;  // Modules left to process
    std::vector<AstClass*> m_paramClasses;  // Parameterized classes

    // Map from AstNodeModule to set of all AstNodeModules that instantiates it.
    std::unordered_map<AstNodeModule*, std::unordered_set<AstNodeModule*>> m_parentps;

    // METHODS

    void visitCells(AstNodeModule* nodep) {
        UASSERT_OBJ(!m_iterateModule, nodep, "Should not nest");
        std::multimap<int, AstNodeModule*> workQueue;
        workQueue.emplace(nodep->level(), nodep);
        m_generateHierName = "";
        m_iterateModule = true;

        // Visit all cells under module, recursively
        do {
            const auto itm = workQueue.cbegin();
            AstNodeModule* const modp = itm->second;
            workQueue.erase(itm);

            // Process once; note user2 will be cleared on specialization, so we will do the
            // specialized module if needed
            if (!modp->user2SetOnce()) {

                // TODO: this really should be an assert, but classes and hier_blocks are
                // special...
                if (modp->someInstanceName().empty()) modp->someInstanceName(modp->origName());

                // Iterate the body
                {
                    VL_RESTORER(m_modp);
                    m_modp = modp;
                    iterateChildren(modp);
                }
            }

            // Process interface cells, then non-interface cells, which may reference an interface
            // cell.
            while (!m_cellps.empty()) {
                const auto itim = m_cellps.cbegin();
                AstNode* const cellp = itim->second;
                m_cellps.erase(itim);

                AstNodeModule* srcModp = nullptr;
                if (const auto* modCellp = VN_CAST(cellp, Cell)) {
                    srcModp = modCellp->modp();
                } else if (const auto* classRefp = VN_CAST(cellp, ClassOrPackageRef)) {
                    srcModp = classRefp->classOrPackageSkipp();
                    if (VN_IS(classRefp->classOrPackageNodep(), ParamTypeDType)) continue;
                } else if (const auto* classRefp = VN_CAST(cellp, ClassRefDType)) {
                    srcModp = classRefp->classp();
                } else if (const auto* ifaceRefp = VN_CAST(cellp, IfaceRefDType)) {
                    srcModp = ifaceRefp->ifacep();
                } else {
                    cellp->v3fatalSrc("Expected module parameterization");
                }
                if (!srcModp) continue;

                // Update path
                string someInstanceName = modp->someInstanceName();
                if (const string* const genHierNamep = cellp->user2u().to<string*>()) {
                    someInstanceName += *genHierNamep;
                    cellp->user2p(nullptr);
                    VL_DO_DANGLING(delete genHierNamep, genHierNamep);
                }

                // Apply parameter specialization
                m_processor.nodeDeparam(cellp, srcModp /* ref */, modp, someInstanceName);

                // Add the (now potentially specialized) child module to the work queue
                workQueue.emplace(srcModp->level(), srcModp);

                // Add to the hierarchy registry
                m_parentps[srcModp].insert(modp);
            }
            if (workQueue.empty()) std::swap(workQueue, m_workQueue);
        } while (!workQueue.empty());

        m_iterateModule = false;
    }

    // Fix up level of module, based on who instantiates it
    void fixLevel(AstNodeModule* modp) {
        if (modp->user1SetOnce()) return;  // Already fixed
        if (m_parentps[modp].empty()) return;  // Leave top levels alone
        int maxParentLevel = 0;
        for (AstNodeModule* parentp : m_parentps[modp]) {
            fixLevel(parentp);  // Ensure parent level is correct
            maxParentLevel = std::max(maxParentLevel, parentp->level());
        }
        if (modp->level() <= maxParentLevel) modp->level(maxParentLevel + 1);
    }

    // A generic visitor for cells and class refs
    void visitCellOrClassRef(AstNode* nodep, bool isIface) {
        // Must do ifaces first, so push to list and do in proper order
        string* const genHierNamep = new std::string{m_generateHierName};
        nodep->user2p(genHierNamep);
        // Visit parameters in the instantiation.
        iterateChildren(nodep);
        m_cellps.emplace(!isIface, nodep);
    }

    // RHSs of AstDots need a relink when LHS is a parameterized class reference
    void relinkDots() {
        for (AstDot* const dotp : m_dots) {
            const AstClassOrPackageRef* const classRefp = VN_AS(dotp->lhsp(), ClassOrPackageRef);
            const AstClass* const lhsClassp = VN_AS(classRefp->classOrPackageSkipp(), Class);
            AstClassOrPackageRef* const rhsp = VN_AS(dotp->rhsp(), ClassOrPackageRef);
            for (auto* itemp = lhsClassp->membersp(); itemp; itemp = itemp->nextp()) {
                if (itemp->name() == rhsp->name()) {
                    rhsp->classOrPackageNodep(itemp);
                    break;
                }
            }
        }
    }

    // VISITORS
    void visit(AstNodeModule* nodep) override {
        if (nodep->recursiveClone()) nodep->dead(true);  // Fake, made for recursive elimination
        if (nodep->dead()) return;  // Marked by LinkDot (and above)
        if (AstClass* const classp = VN_CAST(nodep, Class)) {
            if (classp->hasGParam()) {
                // Don't enter into a definition.
                // If a class is used, it will be visited through a reference and cloned
                m_paramClasses.push_back(classp);
                return;
            }
        }

        if (m_iterateModule) {  // Iterating from visitCells
            UINFO(4, " MOD-under-MOD.  " << nodep);
            m_workQueue.emplace(nodep->level(), nodep);  // Delay until current module is done
            // visitCells (which we are returning to) will process nodep from m_workQueue later
            return;
        }

        // Start traversal at root-like things
        if (nodep->level() <= 2  // Haven't added top yet, so level 2 is the top
            || VN_IS(nodep, Class)  // Nor moved classes
            || VN_IS(nodep, Package)) {  // Likewise haven't done wrapTopPackages yet
            visitCells(nodep);
        }
    }

    void visit(AstCell* nodep) override {
        visitCellOrClassRef(nodep, VN_IS(nodep->modp(), Iface));
    }
    void visit(AstIfaceRefDType* nodep) override {
        if (nodep->ifacep()) visitCellOrClassRef(nodep, true);
    }
    void visit(AstClassRefDType* nodep) override { visitCellOrClassRef(nodep, false); }
    void visit(AstClassOrPackageRef* nodep) override {
        // If it points to a typedef it is not really a class reference. That typedef will be
        // visited anyway (from its parent node), so even if it points to a parameterized class
        // type, the instance will be created.
        if (!VN_IS(nodep->classOrPackageNodep(), Typedef)) visitCellOrClassRef(nodep, false);
    }

    // Make sure all parameters are constantified
    void visit(AstVar* nodep) override {
        if (nodep->user2SetOnce()) return;  // Process once
        iterateChildren(nodep);
        if (nodep->isParam()) {
            if (!nodep->valuep() && !VN_IS(m_modp, Class)) {
                nodep->v3error("Parameter without default value is never given value"
                               << " (IEEE 1800-2023 6.20.1): " << nodep->prettyNameQ());
            } else {
                V3Const::constifyParamsEdit(nodep);  // The variable, not just the var->init()
            }
        }
    }
    void visit(AstParamTypeDType* nodep) override {
        iterateChildren(nodep);
        if (VN_IS(nodep->skipRefOrNullp(), VoidDType)) {
            nodep->v3error("Parameter type without default value is never given value"
                           << " (IEEE 1800-2023 6.20.1): " << nodep->prettyNameQ());
        }
    }
    // Make sure varrefs cause vars to constify before things above
    void visit(AstVarRef* nodep) override {
        // Might jump across functions, so beware if ever add a m_funcp
        if (nodep->varp()) iterate(nodep->varp());
    }
    bool ifaceParamReplace(AstVarXRef* nodep, AstNode* candp) {
        for (; candp; candp = candp->nextp()) {
            if (nodep->name() == candp->name()) {
                if (AstVar* const varp = VN_CAST(candp, Var)) {
                    UINFO(9, "Found interface parameter: " << varp);
                    nodep->varp(varp);
                    return true;
                } else if (const AstPin* const pinp = VN_CAST(candp, Pin)) {
                    UINFO(9, "Found interface parameter: " << pinp);
                    UASSERT_OBJ(pinp->exprp(), pinp, "Interface parameter pin missing expression");
                    VL_DO_DANGLING(nodep->replaceWith(pinp->exprp()->cloneTree(false)), nodep);
                    return true;
                }
            }
        }
        return false;
    }
    void visit(AstVarXRef* nodep) override {
        // Check to see if the scope is just an interface because interfaces are special
        const string dotted = nodep->dotted();
        if (!dotted.empty() && nodep->varp() && nodep->varp()->isParam()) {
            const AstNode* backp = nodep;
            while ((backp = backp->backp())) {
                if (VN_IS(backp, NodeModule)) {
                    UINFO(9, "Hit module boundary, done looking for interface");
                    break;
                }
                if (const AstVar* const varp = VN_CAST(backp, Var)) {
                    if (!varp->isIfaceRef()) continue;
                    const AstIfaceRefDType* ifacerefp = nullptr;
                    if (const AstNodeDType* const typep = varp->childDTypep()) {
                        ifacerefp = VN_CAST(typep, IfaceRefDType);
                        if (!ifacerefp) {
                            if (VN_IS(typep, UnpackArrayDType)) {
                                ifacerefp = VN_CAST(typep->getChildDTypep(), IfaceRefDType);
                            }
                        }
                        if (!ifacerefp) {
                            if (VN_IS(typep, BracketArrayDType)) {
                                ifacerefp = VN_CAST(typep->subDTypep(), IfaceRefDType);
                            }
                        }
                    }
                    if (!ifacerefp) continue;
                    // Interfaces passed in on the port map have ifaces
                    if (const AstIface* const ifacep = ifacerefp->ifacep()) {
                        if (dotted == backp->name()) {
                            UINFO(9, "Iface matching scope:  " << ifacep);
                            if (ifaceParamReplace(nodep, ifacep->stmtsp())) {  //
                                return;
                            }
                        }
                    }
                    // Interfaces declared in this module have cells
                    else if (const AstCell* const cellp = ifacerefp->cellp()) {
                        if (dotted == cellp->name()) {
                            UINFO(9, "Iface matching scope:  " << cellp);
                            if (ifaceParamReplace(nodep, cellp->paramsp())) {  //
                                return;
                            }
                        }
                    }
                }
            }
        }
        if (nodep->containsGenBlock()) {
            // Needs relink, as may remove pointed-to var
            nodep->varp(nullptr);
        }
    }

    void visit(AstDot* nodep) override {
        iterate(nodep->lhsp());
        // Check if it is a reference to a field of a parameterized class.
        // If so, the RHS should be updated, when the LHS is replaced
        // by a class with actual parameter values.
        const AstClass* lhsClassp = nullptr;
        const AstClassOrPackageRef* const classRefp = VN_CAST(nodep->lhsp(), ClassOrPackageRef);
        if (classRefp) lhsClassp = VN_CAST(classRefp->classOrPackageSkipp(), Class);
        AstNode* rhsDefp = nullptr;
        AstClassOrPackageRef* const rhsp = VN_CAST(nodep->rhsp(), ClassOrPackageRef);
        if (rhsp) rhsDefp = rhsp->classOrPackageNodep();
        if (lhsClassp && rhsDefp) {
            m_dots.push_back(nodep);
            // No need to iterate into rhsp, because there should be nothing to do
        } else {
            iterate(nodep->rhsp());
        }
    }

    void visit(AstUnlinkedRef* nodep) override {
        AstVarXRef* const varxrefp = VN_CAST(nodep->refp(), VarXRef);
        AstNodeFTaskRef* const taskrefp = VN_CAST(nodep->refp(), NodeFTaskRef);
        if (varxrefp) {
            m_unlinkedTxt = varxrefp->dotted();
        } else if (taskrefp) {
            m_unlinkedTxt = taskrefp->dotted();
        } else {
            nodep->v3fatalSrc("Unexpected AstUnlinkedRef node");
            return;
        }
        iterate(nodep->cellrefp());

        if (varxrefp) {
            varxrefp->dotted(m_unlinkedTxt);
        } else {
            taskrefp->dotted(m_unlinkedTxt);
        }
        nodep->replaceWith(nodep->refp()->unlinkFrBack());
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstCellArrayRef* nodep) override {
        V3Const::constifyParamsEdit(nodep->selp());
        if (const AstConst* const constp = VN_CAST(nodep->selp(), Const)) {
            const string index = AstNode::encodeNumber(constp->toSInt());
            const string replacestr = nodep->name() + "__BRA__??__KET__";
            const size_t pos = m_unlinkedTxt.find(replacestr);
            UASSERT_OBJ(pos != string::npos, nodep,
                        "Could not find array index in unlinked text: '"
                            << m_unlinkedTxt << "' for node: " << nodep);
            m_unlinkedTxt.replace(pos, replacestr.length(),
                                  nodep->name() + "__BRA__" + index + "__KET__");
        } else {
            nodep->v3error("Could not expand constant selection inside dotted reference: "
                           << nodep->selp()->prettyNameQ());
            return;
        }
    }

    // Generate Statements
    void visit(AstGenIf* nodep) override {
        UINFO(9, "  GENIF " << nodep);
        iterateAndNextNull(nodep->condp());
        // We suppress errors when widthing params since short-circuiting in
        // the conditional evaluation may mean these error can never occur. We
        // then make sure that short-circuiting is used by constifyParamsEdit.
        V3Width::widthGenerateParamsEdit(nodep);  // Param typed widthing will
                                                  // NOT recurse the body.
        V3Const::constifyGenerateParamsEdit(nodep->condp());  // condp may change
        if (const AstConst* const constp = VN_CAST(nodep->condp(), Const)) {
            if (AstNode* const keepp = (constp->isZero() ? nodep->elsesp() : nodep->thensp())) {
                keepp->unlinkFrBackWithNext();
                nodep->replaceWith(keepp);
            } else {
                nodep->unlinkFrBack();
            }
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
            // Normal edit rules will now recurse the replacement
        } else {
            nodep->condp()->v3error("Generate If condition must evaluate to constant");
        }
    }

    void visit(AstBegin* nodep) override {
        // Parameter substitution for generated for loops.
        // TODO Unlike generated IF, we don't have to worry about short-circuiting the
        // conditional expression, since this is currently restricted to simple
        // comparisons. If we ever do move to more generic constant expressions, such code
        // will be needed here.
        if (AstGenFor* const forp = VN_AS(nodep->genforp(), GenFor)) {
            // We should have a GENFOR under here.  We will be replacing the begin,
            // so process here rather than at the generate to avoid iteration problems
            UINFO(9, "  BEGIN " << nodep);
            UINFO(9, "  GENFOR " << forp);
            // Visit child nodes before unrolling
            iterateAndNextNull(forp->initsp());
            iterateAndNextNull(forp->condp());
            iterateAndNextNull(forp->incsp());
            V3Width::widthParamsEdit(forp);  // Param typed widthing will NOT recurse the body
            // Outer wrapper around generate used to hold genvar, and to ensure genvar
            // doesn't conflict in V3LinkDot resolution with other genvars
            // Now though we need to change BEGIN("zzz", GENFOR(...)) to
            // a BEGIN("zzz__BRA__{loop#}__KET__")
            const string beginName = nodep->name();
            // Leave the original Begin, as need a container for the (possible) GENVAR
            // Note V3Unroll will replace some AstVarRef's to the loop variable with constants
            // Don't remove any deleted nodes in m_unroller until whole process finishes,
            // (are held in m_unroller), as some AstXRefs may still point to old nodes.
            VL_DO_DANGLING(m_unroller.unrollGen(forp, beginName), forp);
            // Blocks were constructed under the special begin, move them up
            // Note forp is null, so grab statements again
            if (AstNode* const stmtsp = nodep->genforp()) {
                stmtsp->unlinkFrBackWithNext();
                nodep->addNextHere(stmtsp);
                // Note this clears nodep->genforp(), so begin is no longer special
            }
        } else {
            VL_RESTORER(m_generateHierName);
            m_generateHierName += "." + nodep->prettyName();
            iterateChildren(nodep);
        }
    }
    void visit(AstGenFor* nodep) override {  // LCOV_EXCL_LINE
        nodep->v3fatalSrc("GENFOR should have been wrapped in BEGIN");
    }
    void visit(AstGenCase* nodep) override {
        UINFO(9, "  GENCASE " << nodep);
        bool hit = false;
        AstNode* keepp = nullptr;
        iterateAndNextNull(nodep->exprp());
        V3Case::caseLint(nodep);
        V3Width::widthParamsEdit(nodep);  // Param typed widthing will NOT recurse the
                                          // body, don't trigger errors yet.
        V3Const::constifyParamsEdit(nodep->exprp());  // exprp may change
        const AstConst* const exprp = VN_AS(nodep->exprp(), Const);
        // Constify
        for (AstCaseItem* itemp = nodep->itemsp(); itemp;
             itemp = VN_AS(itemp->nextp(), CaseItem)) {
            for (AstNode* ep = itemp->condsp(); ep;) {
                AstNode* const nextp = ep->nextp();  // May edit list
                iterateAndNextNull(ep);
                VL_DO_DANGLING(V3Const::constifyParamsEdit(ep), ep);  // ep may change
                ep = nextp;
            }
        }
        // Item match
        for (AstCaseItem* itemp = nodep->itemsp(); itemp;
             itemp = VN_AS(itemp->nextp(), CaseItem)) {
            if (!itemp->isDefault()) {
                for (AstNode* ep = itemp->condsp(); ep; ep = ep->nextp()) {
                    if (const AstConst* const ccondp = VN_CAST(ep, Const)) {
                        V3Number match{nodep, 1};
                        match.opEq(ccondp->num(), exprp->num());
                        if (!hit && match.isNeqZero()) {
                            hit = true;
                            keepp = itemp->stmtsp();
                        }
                    } else {
                        itemp->v3error("Generate Case item does not evaluate to constant");
                    }
                }
            }
        }
        // Else default match
        for (AstCaseItem* itemp = nodep->itemsp(); itemp;
             itemp = VN_AS(itemp->nextp(), CaseItem)) {
            if (itemp->isDefault()) {
                if (!hit) {
                    hit = true;
                    keepp = itemp->stmtsp();
                }
            }
        }
        // Replace
        if (keepp) {
            keepp->unlinkFrBackWithNext();
            nodep->replaceWith(keepp);
        } else {
            nodep->unlinkFrBack();
        }
        VL_DO_DANGLING(nodep->deleteTree(), nodep);
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit ParamVisitor(AstNetlist* netlistp)
        : m_processor{netlistp} {
        // Relies on modules already being in top-down-order
        iterate(netlistp);

        relinkDots();

        // Re-sort module list to be in topological order and fix-up incorrect levels. We need to
        // do this globally at the end due to the presence of recursive modules, which might be
        // expanded in orders that reuse earlier specializations later at a lower level.
        {
            // Gather modules
            std::vector<AstNodeModule*> modps;
            for (AstNodeModule *modp = netlistp->modulesp(), *nextp; modp; modp = nextp) {
                nextp = VN_AS(modp->nextp(), NodeModule);
                modp->unlinkFrBack();
                modps.push_back(modp);
            }

            // Fix-up levels
            {
                const VNUser1InUse user1InUse;
                for (AstNodeModule* const modp : modps) fixLevel(modp);
            }

            // Sort by level
            std::stable_sort(modps.begin(), modps.end(),
                             [](const AstNodeModule* ap, const AstNodeModule* bp) {
                                 return ap->level() < bp->level();
                             });

            // Re-insert modules
            for (AstNodeModule* const modp : modps) netlistp->addModulesp(modp);

            for (AstClass* const classp : m_paramClasses) {
                if (!classp->user3p()) {
                    // The default value isn't referenced, so it can be removed
                    VL_DO_DANGLING(pushDeletep(classp->unlinkFrBack()), classp);
                } else {
                    // Referenced. classp became a specialized class with the default
                    // values of parameters and is not a parameterized class anymore
                    classp->hasGParam(false);
                }
            }
        }
    }
    ~ParamVisitor() override = default;
    VL_UNCOPYABLE(ParamVisitor);
};

//######################################################################
// Param class functions

void V3Param::param(AstNetlist* rootp) {
    UINFO(2, __FUNCTION__ << ":");
    { ParamVisitor{rootp}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("param", 0, dumpTreeEitherLevel() >= 3);
}
