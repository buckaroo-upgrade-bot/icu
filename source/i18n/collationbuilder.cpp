/*
*******************************************************************************
* Copyright (C) 2013, International Business Machines
* Corporation and others.  All Rights Reserved.
*******************************************************************************
* collationbuilder.cpp
*
* created on: 2013may06
* created by: Markus W. Scherer
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_COLLATION

#include "unicode/normalizer2.h"
#include "unicode/parseerr.h"
#include "unicode/ucol.h"
#include "unicode/unistr.h"
#include "unicode/utf16.h"
#include "collation.h"
#include "collationbuilder.h"
#include "collationdata.h"
#include "collationroot.h"
#include "collationrootelements.h"
#include "collationruleparser.h"
#include "collationsettings.h"
#include "collationtailoring.h"
#include "collationtailoringdatabuilder.h"
#include "rulebasedcollator.h"
#include "uassert.h"

#define LENGTHOF(array) (int32_t)(sizeof(array)/sizeof((array)[0]))

U_NAMESPACE_BEGIN

// RuleBasedCollator implementation ---------------------------------------- ***

// These methods are here, rather than in rulebasedcollator.cpp,
// for modularization:
// Most code using Collator does not need to build a Collator from rules.
// By moving these constructors and helper methods to a separate file,
// most code will not have a static dependency on the builder code.

RuleBasedCollator2::RuleBasedCollator2(const UnicodeString &rules, UErrorCode &errorCode)
        : data(NULL),
          settings(NULL),
          reader(NULL),
          tailoring(NULL),
          ownedSettings(NULL),
          ownedReorderCodesCapacity(0),
          explicitlySetAttributes(0) {
    buildTailoring(rules, UCOL_DEFAULT, UCOL_DEFAULT, NULL, errorCode);
}

RuleBasedCollator2::RuleBasedCollator2(const UnicodeString &rules, ECollationStrength strength,
                                       UErrorCode &errorCode)
        : data(NULL),
          settings(NULL),
          reader(NULL),
          tailoring(NULL),
          ownedSettings(NULL),
          ownedReorderCodesCapacity(0),
          explicitlySetAttributes(0) {
    buildTailoring(rules, strength, UCOL_DEFAULT, NULL, errorCode);
}

RuleBasedCollator2::RuleBasedCollator2(const UnicodeString &rules,
                                       UColAttributeValue decompositionMode,
                                       UErrorCode &errorCode)
        : data(NULL),
          settings(NULL),
          reader(NULL),
          tailoring(NULL),
          ownedSettings(NULL),
          ownedReorderCodesCapacity(0),
          explicitlySetAttributes(0) {
    buildTailoring(rules, UCOL_DEFAULT, decompositionMode, NULL, errorCode);
}

RuleBasedCollator2::RuleBasedCollator2(const UnicodeString &rules,
                                       ECollationStrength strength,
                                       UColAttributeValue decompositionMode,
                                       UErrorCode &errorCode)
        : data(NULL),
          settings(NULL),
          reader(NULL),
          tailoring(NULL),
          ownedSettings(NULL),
          ownedReorderCodesCapacity(0),
          explicitlySetAttributes(0) {
    buildTailoring(rules, strength, decompositionMode, NULL, errorCode);
}

void
RuleBasedCollator2::buildTailoring(const UnicodeString &rules,
                                   int32_t strength,
                                   UColAttributeValue decompositionMode,
                                   UParseError *outParseError, UErrorCode &errorCode) {
    const CollationData *baseData = CollationRoot::getBaseData(errorCode);
    const CollationSettings *baseSettings = CollationRoot::getBaseSettings(errorCode);
    if(U_FAILURE(errorCode)) { return; }
    tailoring = new CollationTailoring(*baseSettings);
    if(tailoring == NULL) {
        errorCode = U_MEMORY_ALLOCATION_ERROR;
        return;
    }
    CollationBuilder builder(baseData, errorCode);
    builder.parseAndBuild(rules, NULL /* TODO: importer */, *tailoring, outParseError, errorCode);
    if(U_FAILURE(errorCode)) { return; }
    tailoring->rules = rules;
    data = tailoring->data;
    settings = &tailoring->settings;
    // TODO: tailoring->version: maybe root version xor rules.hashCode() xor strength xor decomp (if not default)
    // TODO: tailoring->isDataOwned
    if(strength != UCOL_DEFAULT) {
        tailoring->settings.setStrength(strength, 0, errorCode);
    }
    if(decompositionMode != UCOL_DEFAULT) {
        tailoring->settings.setFlag(CollationSettings::CHECK_FCD, decompositionMode, 0, errorCode);
    }
}

// CollationBuilder implementation ----------------------------------------- ***

CollationBuilder::CollationBuilder(const CollationData *base, UErrorCode &errorCode)
        : nfd(*Normalizer2::getNFDInstance(errorCode)),
          baseData(base),
          rootElements(base->rootElements, base->rootElementsLength),
          variableTop(0),
          firstImplicitCE(0),
          dataBuilder(errorCode),
          errorReason(NULL),
          cesLength(0),
          rootPrimaryIndexes(errorCode), nodes(errorCode) {
    // Preset node 0 as the start of a list for root primary 0.
    nodes.addElement(0, errorCode);
    rootPrimaryIndexes.addElement(0, errorCode);

    // Look up [first implicit] before tailoring the relevant character.
    int32_t length = dataBuilder.getCEs(UnicodeString((UChar)0x4e00), ces, 0);
    U_ASSERT(length == 1);
    firstImplicitCE = ces[0];

    if(U_FAILURE(errorCode)) {
        errorReason = "CollationBuilder initialization failed";
    }
}

CollationBuilder::~CollationBuilder() {
}

void
CollationBuilder::parseAndBuild(const UnicodeString &ruleString,
                                CollationRuleParser::Importer *importer,
                                CollationTailoring &tailoring,
                                UParseError *outParseError,
                                UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    if(baseData->rootElements) {
        errorCode = U_MISSING_RESOURCE_ERROR;
        errorReason = "missing root elements data, tailoring not supported";
        return;
    }
    CollationRuleParser parser(baseData, errorCode);
    if(U_FAILURE(errorCode)) { return; }
    // TODO: This always bases &[last variable] and &[first regular]
    // on the root collator's maxVariable/variableTop.
    // Discuss whether we would want this to change after [maxVariable x],
    // in which case we would keep the tailoring.settings pointer here
    // and read its variableTop when we need it.
    // See http://unicode.org/cldr/trac/ticket/6070
    variableTop = tailoring.settings.variableTop;
    parser.setSink(this);
    parser.setImporter(importer);
    parser.parse(ruleString, tailoring.settings, outParseError, errorCode);
    errorReason = parser.getErrorReason();
    if(U_FAILURE(errorCode)) { return; }
    // TODO
}

void
CollationBuilder::addReset(int32_t strength, const UnicodeString &str,
                           const char *&parserErrorReason, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    U_ASSERT(!str.isEmpty());
    if(str.charAt(0) == CollationRuleParser::POS_LEAD) {
        ces[0] = getSpecialResetPosition(str, parserErrorReason, errorCode);
        cesLength = 1;
        if(U_FAILURE(errorCode)) { return; }
    } else {
        // normal reset to a character or string
        UnicodeString nfdString = nfd.normalize(str, errorCode);
        if(U_FAILURE(errorCode)) {
            parserErrorReason = "NFD(reset position)";
            return;
        }
        cesLength = dataBuilder.getCEs(nfdString, ces, 0);
        if(cesLength > Collation::MAX_EXPANSION_LENGTH) {
            errorCode = U_ILLEGAL_ARGUMENT_ERROR;
            parserErrorReason = "reset position maps to too many collation elements (more than 31)";
            return;
        }
    }
    if(strength == UCOL_IDENTICAL) { return; }  // simple reset-at-position

    // &[before strength]position
    U_ASSERT(UCOL_PRIMARY <= strength && strength <= UCOL_TERTIARY);
    int32_t index = findOrInsertNodeForCEs(strength, parserErrorReason, errorCode);
    if(U_FAILURE(errorCode)) { return; }

    int64_t node = nodes.elementAti(index);
    // If the index is for a "weaker" tailored node,
    // then skip backwards over this and further "weaker" nodes.
    while(strengthFromNode(node) > strength) {
        index = previousIndexFromNode(node);
        node = nodes.elementAti(index);
    }

    // Find or insert a node whose index we will put into a temporary CE.
    if(strengthFromNode(node) == strength && isTailoredNode(node)) {
        // Reset to just before this same-strength tailored node.
        index = previousIndexFromNode(node);
    } else if(strength == UCOL_PRIMARY) {
        // root primary node (has no previous index)
        uint32_t p = weight32FromNode(node);
        if(p == 0) {
            errorCode = U_UNSUPPORTED_ERROR;
            parserErrorReason = "reset before completely-ignorable not possible";
            return;
        }
        if(p <= rootElements.getFirstPrimary()) {
            // There is no primary gap between ignorables and the space-first-primary.
            errorCode = U_UNSUPPORTED_ERROR;
            parserErrorReason = "reset primary-before first non-ignorable not supported";
            return;
        }
        if(p == Collation::FIRST_TRAILING_PRIMARY) {
            // We do not support tailoring to an unassigned-implicit CE.
            errorCode = U_UNSUPPORTED_ERROR;
            parserErrorReason = "reset primary-before [first trailing] not supported";
            return;
        }
        int32_t limitIndex = index;
        p = rootElements.getPrimaryBefore(p, baseData->isCompressiblePrimary(p));
        index = findOrInsertNodeForRootCE(Collation::makeCE(p), UCOL_PRIMARY, errorCode);
        if(U_FAILURE(errorCode)) { return; }
        node = nodes.elementAti(index);
        if(nextIndexFromNode(node) == 0) {
            // Small optimization:
            // Terminate this new list with the node for the next root primary,
            // so that we need not look up the limit later.
            nodes.setElementAt(index, node | nodeFromNextIndex(limitIndex));
        }
    } else {
        // &[before 2] or &[before 3]
        index = findCommonNode(index, UCOL_SECONDARY);
        if(strength >= UCOL_TERTIARY) {
            index = findCommonNode(index, UCOL_TERTIARY);
        }
        node = nodes.elementAti(index);
        if(strengthFromNode(node) == strength) {
            // Found a same-strength node with an explicit weight.
            uint32_t weight16 = weight16FromNode(node);
            if(weight16 == 0) {
                errorCode = U_UNSUPPORTED_ERROR;
                parserErrorReason = "reset before completely-ignorable not possible";
                return;
            }
            U_ASSERT(weight16 >= Collation::COMMON_WEIGHT16);
            int32_t previousIndex = previousIndexFromNode(node);
            if(weight16 == Collation::COMMON_WEIGHT16) {
                // Reset to just before this same-strength common-weight node.
                index = previousIndex;
            } else {
                // A non-common weight is only possible from a root CE.
                // Find the higher-level weights, which must all be explicit,
                // and then find the preceding weight for this level.
                uint32_t previousWeight16 = 0;
                int32_t previousWeightIndex = -1;
                int32_t i = index;
                if(strength == UCOL_SECONDARY) {
                    uint32_t p;
                    do {
                        i = previousIndexFromNode(node);
                        node = nodes.elementAti(i);
                        if(strengthFromNode(node) == UCOL_SECONDARY && !isTailoredNode(node) &&
                                previousWeightIndex < 0) {
                            previousWeightIndex = i;
                            previousWeight16 = weight16FromNode(node);
                        }
                    } while(strengthFromNode(node) > UCOL_PRIMARY);
                    U_ASSERT(!isTailoredNode(node));
                    p = weight32FromNode(node);
                    weight16 = rootElements.getSecondaryBefore(p, weight16);
                } else {
                    uint32_t p, s;
                    do {
                        i = previousIndexFromNode(node);
                        node = nodes.elementAti(i);
                        if(strengthFromNode(node) == UCOL_TERTIARY && !isTailoredNode(node) &&
                                previousWeightIndex < 0) {
                            previousWeightIndex = i;
                            previousWeight16 = weight16FromNode(node);
                        }
                    } while(strengthFromNode(node) > UCOL_SECONDARY);
                    U_ASSERT(!isTailoredNode(node));
                    if(strengthFromNode(node) == UCOL_SECONDARY) {
                        s = weight16FromNode(node);
                        do {
                            i = previousIndexFromNode(node);
                            node = nodes.elementAti(i);
                        } while(strengthFromNode(node) > UCOL_PRIMARY);
                        U_ASSERT(!isTailoredNode(node));
                    } else {
                        U_ASSERT(!nodeHasBefore2(node));
                        s = Collation::COMMON_WEIGHT16;
                    }
                    p = weight32FromNode(node);
                    weight16 = rootElements.getTertiaryBefore(p, s, weight16);
                }
                // Find or insert the new explicit weight before the current one.
                if(previousWeightIndex >= 0 && weight16 == previousWeight16) {
                    index = previousWeightIndex;
                } else {
                    node = nodeFromWeight16(weight16) | nodeFromStrength(strength);
                    index = insertNodeBetween(previousIndex, index, node, errorCode);
                }
            }
        } else {
            // Found a stronger node with implied strength-common weight.
            int64_t hasBefore3 = 0;
            if(strength == UCOL_SECONDARY) {
                U_ASSERT(!nodeHasBefore2(node));
                // Move the HAS_BEFORE3 flag from the parent node
                // to the new secondary common node.
                hasBefore3 = node & HAS_BEFORE3;
                node = (node & ~(int64_t)HAS_BEFORE3) | HAS_BEFORE2;
            } else {
                U_ASSERT(!nodeHasBefore3(node));
                node |= HAS_BEFORE3;
            }
            nodes.setElementAt(index, node);
            int32_t nextIndex = nextIndexFromNode(node);
            // Insert default nodes with weights 02 and 05, reset to the 02 node.
            node = nodeFromWeight16(BEFORE_WEIGHT16) | nodeFromStrength(strength);
            index = insertNodeBetween(index, nextIndex, node, errorCode);
            node = nodeFromWeight16(Collation::COMMON_WEIGHT16) | hasBefore3 |
                    nodeFromStrength(strength);
            insertNodeBetween(index, nextIndex, node, errorCode);
        }
    }
    if(U_FAILURE(errorCode)) {
        parserErrorReason = "inserting reset position for &[before n]";
        return;
    }
    ces[cesLength - 1] = tempCEFromIndexAndStrength(index, strength);
}

int64_t
CollationBuilder::getSpecialResetPosition(const UnicodeString &str,
                                          const char *&parserErrorReason, UErrorCode &errorCode) {
    U_ASSERT(str.length() == 2);
    UChar32 pos = str.charAt(1) - CollationRuleParser::POS_BASE;
    U_ASSERT(0 <= pos && pos <= CollationRuleParser::LAST_TRAILING);
    // TODO: [first Grek], [last punct], etc.
    switch(pos) {
    case CollationRuleParser::FIRST_TERTIARY_IGNORABLE:
        return 0;
    case CollationRuleParser::LAST_TERTIARY_IGNORABLE:
        return 0;
    case CollationRuleParser::FIRST_SECONDARY_IGNORABLE:
        return rootElements.getFirstTertiaryCE();
    case CollationRuleParser::LAST_SECONDARY_IGNORABLE:
        return rootElements.getLastTertiaryCE();
    case CollationRuleParser::FIRST_PRIMARY_IGNORABLE:
        return rootElements.getFirstSecondaryCE();
    case CollationRuleParser::LAST_PRIMARY_IGNORABLE:
        return rootElements.getLastSecondaryCE();
    case CollationRuleParser::FIRST_VARIABLE:
        return rootElements.getFirstPrimaryCE();
    case CollationRuleParser::LAST_VARIABLE:
        return rootElements.lastCEWithPrimaryBefore(variableTop + 1);
    case CollationRuleParser::FIRST_REGULAR:
        return rootElements.firstCEWithPrimaryAtLeast(variableTop + 1);
    case CollationRuleParser::LAST_REGULAR:
        // Use the Hani-first-primary rather than the actual last "regular" CE before it,
        // for backward compatibility with behavior before the introduction of
        // script-first-primary CEs in the root collator.
        return rootElements.firstCEWithPrimaryAtLeast(
            baseData->getFirstPrimaryForGroup(USCRIPT_HAN));
    case CollationRuleParser::FIRST_IMPLICIT:
        return firstImplicitCE;
    case CollationRuleParser::LAST_IMPLICIT:
        // We do not support tailoring to an unassigned-implicit CE.
        errorCode = U_UNSUPPORTED_ERROR;
        parserErrorReason = "reset to [last implicit] not supported";
        return 0;
    case CollationRuleParser::FIRST_TRAILING:
        return Collation::makeCE(Collation::FIRST_TRAILING_PRIMARY);
    case CollationRuleParser::LAST_TRAILING:
        return rootElements.lastCEWithPrimaryBefore(Collation::FFFD_PRIMARY);
    default:
        U_ASSERT(FALSE);
        return 0;
    }
}

void
CollationBuilder::addRelation(int32_t strength, const UnicodeString &prefix,
                              const UnicodeString &str, const UnicodeString &extension,
                              const char *&parserErrorReason, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    UnicodeString nfdPrefix;
    if(!prefix.isEmpty()) {
        nfd.normalize(prefix, nfdPrefix, errorCode);
        if(U_FAILURE(errorCode)) {
            parserErrorReason = "NFD(prefix)";
            return;
        }
    }
    UnicodeString nfdString = nfd.normalize(str, errorCode);
    if(U_FAILURE(errorCode)) {
        parserErrorReason = "NFD(string)";
        return;
    }
    if(strength != UCOL_IDENTICAL) {
        // Find the node index after which we insert the new tailored node.
        int32_t index = findOrInsertNodeForCEs(strength, parserErrorReason, errorCode);
        if(index == 0) {
            // There is no primary gap between ignorables and the space-first-primary.
            errorCode = U_UNSUPPORTED_ERROR;
            parserErrorReason = "tailoring primary after ignorables not supported";
            return;
        }
        // Insert the new tailored node.
        index = insertTailoredNodeAfter(index, strength, errorCode);
        if(U_FAILURE(errorCode)) {
            parserErrorReason = "modifying collation elements";
            return;
        }
        ces[cesLength - 1] = tempCEFromIndexAndStrength(index, strength);
    }
    int32_t totalLength = cesLength;
    if(!extension.isEmpty()) {
        UnicodeString nfdExtension = nfd.normalize(extension, errorCode);
        if(U_FAILURE(errorCode)) {
            parserErrorReason = "NFD(extension)";
            return;
        }
        totalLength = dataBuilder.getCEs(nfdExtension, ces, cesLength);
        if(totalLength > Collation::MAX_EXPANSION_LENGTH) {
            errorCode = U_ILLEGAL_ARGUMENT_ERROR;
            parserErrorReason =
                "extension string adds too many collation elements (more than 31 total)";
            return;
        }
    }
    // Map from the NFD input to the CEs.
    dataBuilder.add(nfdPrefix, nfdString, ces, cesLength, errorCode);
    if(prefix != nfdPrefix || str != nfdString) {
        // Also right away map from the FCC input to the CEs.
        dataBuilder.add(prefix, str, ces, cesLength, errorCode);
    }
    if(U_FAILURE(errorCode)) {
        parserErrorReason = "writing collation elements";
        return;
    }
}

int32_t
CollationBuilder::ceStrength(int64_t ce) {
    return
        isTempCE(ce) ? strengthFromTempCE(ce) :
        (ce & 0xff00000000000000) != 0 ? UCOL_PRIMARY :
        ((uint32_t)ce & 0xff000000) != 0 ? UCOL_SECONDARY :
        ce != 0 ? UCOL_TERTIARY :
        UCOL_IDENTICAL;
}

int32_t
CollationBuilder::findOrInsertNodeForCEs(int32_t strength, const char *&parserErrorReason,
                                         UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return 0; }
    U_ASSERT(UCOL_PRIMARY <= strength && strength <= UCOL_QUATERNARY);

    // Find the last CE that is at least as "strong" as the requested difference.
    // Note: Stronger is smaller (UCOL_PRIMARY=0).
    int64_t ce;
    for(;; --cesLength) {
        if(cesLength == 0) {
            ce = ces[0] = 0;
            cesLength = 1;
            break;
        } else {
            ce = ces[cesLength - 1];
        }
        if(ceStrength(ce) <= strength) {
            break;
        }
    }

    if(isTempCE(ce)) { return indexFromTempCE(ce); }

    if((uint8_t)(ce >> 56) == Collation::UNASSIGNED_IMPLICIT_BYTE) {
        errorCode = U_UNSUPPORTED_ERROR;
        parserErrorReason = "tailoring relative to an unassigned code point not supported";
        return 0;
    }
    return findOrInsertNodeForRootCE(ce, strength, errorCode);
}

namespace {

/**
 * Like Java Collections.binarySearch(List, key, Comparator).
 *
 * @return the index>=0 where the item was found,
 *         or the index<0 for inserting the string at ~index in sorted order
 *         (index into rootPrimaryIndexes)
 */
int32_t
binarySearchForRootPrimaryNode(const int32_t *rootPrimaryIndexes, int32_t length,
                               const int64_t *nodes, uint32_t p) {
    U_ASSERT(length > 0);
    int32_t start = 0;
    int32_t limit = length;
    for (;;) {
        int32_t i = (start + limit) / 2;
        int64_t node = nodes[rootPrimaryIndexes[i]];
        uint32_t nodePrimary = (uint32_t)(node >> 32);  // weight32FromNode(node)
        if (p == nodePrimary) {
            return i;
        } else if (p < nodePrimary) {
            if (i == start) {
                return ~start;  // insert s before i
            }
            limit = i;
        } else {
            if (i == start) {
                return ~(start + 1);  // insert s after i
            }
            start = i;
        }
    }
}

}  // namespace

int32_t
CollationBuilder::findOrInsertNodeForRootCE(int64_t ce, int32_t strength, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return 0; }

    // Find or insert the node for the root CE's primary weight.
    uint32_t p = (uint32_t)(ce >> 32);
    int32_t rootIndex = binarySearchForRootPrimaryNode(
        rootPrimaryIndexes.getBuffer(), rootPrimaryIndexes.size(), nodes.getBuffer(), p);
    int32_t index;
    if(rootIndex >= 0) {
        index = rootPrimaryIndexes.elementAti(rootIndex);
    } else {
        // Start a new list of nodes with this primary.
        index = nodes.size();
        nodes.addElement(nodeFromWeight32(p), errorCode);
        rootPrimaryIndexes.insertElementAt(index, ~rootIndex, errorCode);
        if(U_FAILURE(errorCode)) { return 0; }
    }

    // Find or insert the node for each of the root CE's lower-level weights,
    // down to the requested level/strength.
    // Root CEs must have common=zero quaternary weights (for which we never insert any nodes).
    U_ASSERT((ce & 0xc0) == 0);
    for(int32_t level = UCOL_SECONDARY; level <= strength && level <= UCOL_TERTIARY; ++level) {
        uint32_t lower32 = (uint32_t)ce;
        uint32_t weight16 =
            level == UCOL_SECONDARY ? lower32 >> 16 :
            lower32 & Collation::ONLY_TERTIARY_MASK;
        U_ASSERT(weight16 >= Collation::COMMON_WEIGHT16);
        // Only reset-before inserts common weights.
        if(weight16 == Collation::COMMON_WEIGHT16) {
            index = findCommonNode(index, level);
            continue;
        }
        // Find the root CE's weight for this level.
        // Postpone insertion if not found:
        // Insert the new root node before the next stronger node,
        // or before the next root node with the same strength and a larger weight.
        int64_t node = nodes.elementAti(index);
        int32_t nextIndex;
        for(;;) {
            nextIndex = nextIndexFromNode(node);
            node = nodes.elementAti(nextIndex);
            int32_t nextStrength = strengthFromNode(node);
            if(nextStrength <= strength) {
                // Insert before a stronger node.
                if(nextStrength < strength) { break; }
                // nextStrength == strength
                if(isTailoredNode(node)) {
                    uint32_t nextWeight16 = weight16FromNode(node);
                    if(nextWeight16 == weight16) {
                        // Found the node for the root CE up to this level.
                        index = nextIndex;
                        nextIndex = -1;  // no insertion (continue outer loop)
                        break;
                    }
                    // Insert before a node with a larger same-strength weight.
                    if(nextWeight16 > weight16) { break; }
                }
            }
            // Skip the next node.
            index = nextIndex;
        }
        if(nextIndex >= 0) {
            node = nodeFromWeight16(weight16) | nodeFromStrength(level);
            index = insertNodeBetween(index, nextIndex, node, errorCode);
            if(U_FAILURE(errorCode)) { return 0; }
        }
    }
    return index;
}

int32_t
CollationBuilder::insertTailoredNodeAfter(int32_t index, int32_t strength, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return 0; }
    if(strength >= UCOL_SECONDARY) {
        index = findCommonNode(index, UCOL_SECONDARY);
    }
    if(strength >= UCOL_TERTIARY) {
        index = findCommonNode(index, UCOL_TERTIARY);
    }
    // Postpone insertion:
    // Insert the new node before the next one with a strength at least as strong.
    int64_t node = nodes.elementAti(index);
    int32_t nextIndex;
    for(;;) {
        nextIndex = nextIndexFromNode(node);
        if(nextIndex == 0) { break; }
        node = nodes.elementAti(nextIndex);
        if(strengthFromNode(node) <= strength) { break; }
        // Skip the next node which has a weaker (larger) strength than the new one.
        index = nextIndex;
    }
    node = IS_TAILORED | nodeFromStrength(strength);
    return insertNodeBetween(index, nextIndex, node, errorCode);
}

int32_t
CollationBuilder::insertNodeBetween(int32_t index, int32_t nextIndex, int64_t node,
                                    UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return 0; }
    U_ASSERT(previousIndexFromNode(node) == 0);
    U_ASSERT(nextIndexFromNode(node) == 0);
    U_ASSERT(nextIndexFromNode(nodes.elementAti(index)) == nextIndex);
    // Append the new node and link it to the existing nodes.
    int32_t newIndex = nodes.size();
    node |= nodeFromPreviousIndex(index) | nodeFromNextIndex(nextIndex);
    nodes.addElement(node, errorCode);
    if(U_FAILURE(errorCode)) { return 0; }
    // nodes[index].nextIndex = newIndex
    node = nodes.elementAti(index);
    nodes.setElementAt(changeNodeNextIndex(node, newIndex), index);
    // nodes[nextIndex].previousIndex = newIndex
    if(nextIndex != 0) {
        node = nodes.elementAti(nextIndex);
        nodes.setElementAt(changeNodePreviousIndex(node, newIndex), nextIndex);
    }
    return newIndex;
}

int32_t
CollationBuilder::findCommonNode(int32_t index, int32_t strength) const {
    U_ASSERT(UCOL_SECONDARY <= strength && strength <= UCOL_TERTIARY);
    int64_t node = nodes.elementAti(index);
    if(strengthFromNode(node) >= strength) {
        // The current node is no stronger.
        return index;
    }
    if(strength == UCOL_SECONDARY ? !nodeHasBefore2(node) : !nodeHasBefore3(node)) {
        // The current node implies the strength-common weight.
        return index;
    }
    index = nextIndexFromNode(node);
    node = nodes.elementAti(index);
    U_ASSERT(strengthFromNode(node) == strength &&
            weight16FromNode(node) == BEFORE_WEIGHT16);
    // Skip to the explicit common node.
    do {
        index = nextIndexFromNode(node);
        node = nodes.elementAti(index);
        U_ASSERT(strengthFromNode(node) >= strength);
    } while(isTailoredNode(node) || strengthFromNode(node) > strength);
    U_ASSERT(weight16FromNode(node) == Collation::COMMON_WEIGHT16);
    return index;
}

void
CollationBuilder::suppressContractions(const UnicodeSet &set,
                                       const char *&parserErrorReason, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    if(!set.isEmpty()) {
        errorCode = U_UNSUPPORTED_ERROR;  // TODO
        parserErrorReason = "TODO: support [suppressContractions [set]]";
        return;
    }
    // TODO
}

U_NAMESPACE_END

#endif  // !UCONFIG_NO_COLLATION