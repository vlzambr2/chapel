/*
 * Copyright 2021-2023 Hewlett Packard Enterprise Development LP
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "chpl/resolution/resolution-queries.h"

#include "chpl/parsing/parsing-queries.h"
#include "chpl/framework/compiler-configuration.h"
#include "chpl/framework/ErrorMessage.h"
#include "chpl/framework/UniqueString.h"
#include "chpl/framework/global-strings.h"
#include "chpl/framework/query-impl.h"
#include "chpl/resolution/can-pass.h"
#include "chpl/resolution/disambiguation.h"
#include "chpl/resolution/intents.h"
#include "chpl/resolution/scope-queries.h"
#include "chpl/types/all-types.h"
#include "chpl/uast/all-uast.h"

#include "Resolver.h"
#include "call-init-deinit.h"
#include "default-functions.h"
#include "maybe-const.h"
#include "prims.h"
#include "return-type-inference.h"
#include "signature-checks.h"
#include "try-catch-analysis.h"

#include <cstdio>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chpl {
namespace resolution {


using namespace uast;
using namespace types;

using CandidatesVec = std::vector<const TypedFnSignature*>;
using ForwardingInfoVec = std::vector<QualifiedType>;

const ResolutionResultByPostorderID& resolveModuleStmt(Context* context,
                                                       ID id) {
  QUERY_BEGIN(resolveModuleStmt, context, id);

  CHPL_ASSERT(id.postOrderId() >= 0);

  // TODO: can we save space better here by having
  // the ResolutionResultByPostorderID have a different offset
  // (so it can contain only ids within the requested stmt) or
  // maybe we can make it sparse with a hashtable or something?
  ResolutionResultByPostorderID result;

  ID moduleId = parsing::idToParentId(context, id);
  auto moduleAst = parsing::idToAst(context, moduleId);
  if (const Module* mod = moduleAst->toModule()) {
    // Resolve just the requested statement
    auto modStmt = parsing::idToAst(context, id);
    auto visitor = Resolver::createForModuleStmt(context, mod, modStmt, result);
    modStmt->traverse(visitor);
  }

  return QUERY_END(result);
}

static const ResolutionResultByPostorderID&
scopeResolveModuleStmt(Context* context, ID id) {
  QUERY_BEGIN(scopeResolveModuleStmt, context, id);

  CHPL_ASSERT(id.postOrderId() >= 0);

  // TODO: can we save space better here by having
  // the ResolutionResultByPostorderID have a different offset
  // (so it can contain only ids within the requested stmt) or
  // maybe we can make it sparse with a hashtable or something?
  ResolutionResultByPostorderID result;

  ID moduleId = parsing::idToParentId(context, id);
  auto moduleAst = parsing::idToAst(context, moduleId);
  if (const Module* mod = moduleAst->toModule()) {
    // Resolve just the requested statement
    auto modStmt = parsing::idToAst(context, id);
    auto visitor =
      Resolver::createForScopeResolvingModuleStmt(context, mod, modStmt,
                                                  result);
    modStmt->traverse(visitor);
  }

  return QUERY_END(result);
}



const ResolutionResultByPostorderID& resolveModule(Context* context, ID id) {
  QUERY_BEGIN(resolveModule, context, id);

  const AstNode* ast = parsing::idToAst(context, id);
  CHPL_ASSERT(ast != nullptr);

  ResolutionResultByPostorderID result;

  if (ast != nullptr) {
    if (const Module* mod = ast->toModule()) {
      // check for multiply-defined symbols within the module
      auto modScope = scopeForId(context, mod->id());
      emitMultipleDefinedSymbolErrors(context, modScope);

      result.setupForSymbol(mod);
      for (auto child: mod->children()) {
        if (child->isComment() ||
            child->isTypeDecl() ||
            child->isFunction() ||
            child->isModule() ||
            child->isExternBlock()) {
            // Resolve use/import to find deprecation/unstable warnings.
            // child->isUse() ||
            // child->isImport()) {
          // ignore this statement since it is not relevant to
          // the resolution of module initializers and module-level
          // variables.
        } else {
          ID stmtId = child->id();
          // resolve the statement
          const ResolutionResultByPostorderID& resolved =
            resolveModuleStmt(context, stmtId);

          // copy results for children and the node itself
          int firstId = stmtId.postOrderId() - stmtId.numContainedChildren();
          int lastId = firstId + stmtId.numContainedChildren();
          for (int i = firstId; i <= lastId; i++) {
            ID exprId(stmtId.symbolPath(), i, 0);
            ResolvedExpression& re = result.byId(exprId);
            if (auto reToCopy = resolved.byIdOrNull(exprId)) {
              re = *reToCopy;
            }
          }
        }
      }
      checkThrows(context, result, mod);
    }
  }

  return QUERY_END(result);
}

const ResolutionResultByPostorderID&
scopeResolveModule(Context* context, ID id) {
  QUERY_BEGIN(scopeResolveModule, context, id);

  const AstNode* ast = parsing::idToAst(context, id);
  CHPL_ASSERT(ast != nullptr);

  ResolutionResultByPostorderID result;

  if (ast != nullptr) {
    if (const Module* mod = ast->toModule()) {
      // check for multiply-defined symbols within the module
      auto modScope = scopeForId(context, mod->id());
      emitMultipleDefinedSymbolErrors(context, modScope);

      result.setupForSymbol(mod);
      for (auto child: mod->children()) {
        if (child->isComment() ||
            child->isTypeDecl() ||
            child->isFunction() ||
            child->isModule() ||
            child->isInterface() ||
            child->isExternBlock()) {
            // Resolve use/import to find deprecation/unstable warnings.
            // child->isUse() ||
            // child->isImport()) {
          // ignore this statement since it is not relevant to
          // the resolution of module initializers and module-level
          // variables.
        } else {
          ID stmtId = child->id();
          // resolve the statement
          const ResolutionResultByPostorderID& resolved =
            scopeResolveModuleStmt(context, stmtId);

          // copy results for children and the node itself
          int firstId = stmtId.postOrderId() - stmtId.numContainedChildren();
          int lastId = firstId + stmtId.numContainedChildren();
          for (int i = firstId; i <= lastId; i++) {
            ID exprId(stmtId.symbolPath(), i, 0);
            ResolvedExpression& re = result.byId(exprId);
            if (auto reToCopy = resolved.byIdOrNull(exprId)) {
              re = *reToCopy;
            }
          }
        }
      }
    }
  }

  return QUERY_END(result);

}

const QualifiedType& typeForModuleLevelSymbol(Context* context, ID id) {
  QUERY_BEGIN(typeForModuleLevelSymbol, context, id);

  QualifiedType result;

  int postOrderId = id.postOrderId();
  if (postOrderId >= 0) {
    const auto& resolvedStmt = resolveModuleStmt(context, id);
    if (resolvedStmt.hasId(id)) {
      result = resolvedStmt.byId(id).type();
    } else {
      // fall back to default value
      result = QualifiedType();
    }
  } else {
    QualifiedType::Kind kind = QualifiedType::UNKNOWN;
    const Type* t = nullptr;

    auto tag = parsing::idToTag(context, id);

    if (asttags::isNamedDecl(tag)) {
      if (asttags::isTypeDecl(tag)) {
        kind = QualifiedType::TYPE;
        t = initialTypeForTypeDecl(context, id);
      } else if (asttags::isModule(tag)) {
        kind = QualifiedType::MODULE;
      } else if (asttags::isFunction(tag)) {
        if (parsing::idIsParenlessFunction(context, id)) {
          kind = QualifiedType::PARENLESS_FUNCTION;
        } else {
          kind = QualifiedType::FUNCTION;
        }
      } else if (asttags::isInterface(tag)) {
        //TODO: kind = QualifiedType::INTERFACE;
        CHPL_UNIMPL("interfaces");
      } else {
        CHPL_ASSERT(false && "case not handled");
      }
    } else {
      CHPL_ASSERT(false && "case not handled");
    }

    result = QualifiedType(kind, t);
  }

  return QUERY_END(result);
}

const QualifiedType& typeForBuiltin(Context* context,
                                    UniqueString name) {
  QUERY_BEGIN(typeForBuiltin, context, name);

  QualifiedType result;

  std::unordered_map<UniqueString,const Type*> typeMap;
  Type::gatherBuiltins(context, typeMap);
  auto& globalMap = getCompilerGeneratedGlobals(context);

  auto searchTypes = typeMap.find(name);
  auto searchGlobals = globalMap.find(name);
  if (searchTypes != typeMap.end()) {
    const Type* t = searchTypes->second;
    CHPL_ASSERT(t);

    if (auto bct = t->toManageableType()) {
      auto d = ClassTypeDecorator(ClassTypeDecorator::GENERIC_NONNIL);
      t = ClassType::get(context, bct, /*manager*/ nullptr, d);
    }

    result = QualifiedType(QualifiedType::TYPE, t);
  } else if (searchGlobals != globalMap.end()) {
    result = searchGlobals->second;
  } else {
    // Could be a non-type builtin like 'index'
    result = QualifiedType();
  }

  return QUERY_END(result);
}

QualifiedType typeForLiteral(Context* context, const Literal* literal) {
  const Type* typePtr = nullptr;
  const Param* paramPtr = nullptr;

  switch (literal->tag()) {
    case asttags::BoolLiteral:
      typePtr = BoolType::get(context);
      break;
    case asttags::ImagLiteral:
      typePtr = ImagType::get(context, 0);
      break;
    case asttags::IntLiteral:
      typePtr = IntType::get(context, 0);
      break;
    case asttags::RealLiteral:
      typePtr = RealType::get(context, 0);
      break;
    case asttags::UintLiteral:
      typePtr = UintType::get(context, 0);
      break;
    case asttags::BytesLiteral:
      typePtr = CompositeType::getBytesType(context);
      break;
    case asttags::CStringLiteral:
      typePtr = CStringType::get(context);
      break;
    case asttags::StringLiteral:
      typePtr = CompositeType::getStringType(context);
      break;
    default:
      CHPL_ASSERT(false && "case not handled");
  }
  paramPtr = literal->param();

  return QualifiedType(QualifiedType::PARAM, typePtr, paramPtr);
}


/////// function resolution

static bool
anyFormalNeedsInstantiation(Context* context,
                            const std::vector<types::QualifiedType>& formalTs,
                            const UntypedFnSignature* untypedSig,
                            SubstitutionsMap* substitutions) {
  bool genericOrUnknown = false;
  int i = 0;
  for (const auto& qt : formalTs) {
    if (qt.isUnknown()) {
      genericOrUnknown = true;
      break;
    }

    bool considerGenericity = true;
    if (substitutions != nullptr) {
      auto formalDecl = untypedSig->formalDecl(i);
      if (substitutions->count(formalDecl->id())) {
        // don't consider it needing a substitution - e.g. when passing
        // a generic type into a type argument.
        considerGenericity = false;
      }
    }

    if (considerGenericity) {
      auto g = getTypeGenericity(context, qt);
      if (g != Type::CONCRETE) {
        genericOrUnknown = true;
        break;
      }
    }

    i++;
  }
  return genericOrUnknown;
}

static TypedFnSignature::WhereClauseResult whereClauseResult(
                                     Context* context,
                                     const Function* fn,
                                     const ResolutionResultByPostorderID& r,
                                     bool needsInstantiation) {
  auto whereClauseResult = TypedFnSignature::WHERE_TBD;
  if (const AstNode* where = fn->whereClause()) {
    const QualifiedType& qt = r.byAst(where).type();
    bool isBoolType = qt.type() && qt.type()->isBoolType();
    if (isBoolType && qt.isParamTrue()) {
      whereClauseResult = TypedFnSignature::WHERE_TRUE;
    } else if (isBoolType && qt.isParamFalse()) {
      whereClauseResult = TypedFnSignature::WHERE_FALSE;
    } else if (needsInstantiation) {
      // it's OK, need to establish the value of the where clause later
      whereClauseResult = TypedFnSignature::WHERE_TBD;
    } else {
      // where clause should be resolved by now
      context->error(where,
                     "where clause does not result in a param bool value");
    }
  } else {
    whereClauseResult = TypedFnSignature::WHERE_NONE;
  }

  return whereClauseResult;
}

// Finds a parent function from a function ID
// Returns that parent function, or an empty ID if there was none.
static ID parentFunctionId(Context* context, ID functionId) {
  ID parentSymId = functionId.parentSymbolId(context);
  const Scope* parentScope = scopeForId(context, parentSymId);
  for (const Scope* s = parentScope; s != nullptr; s = s->parentScope()) {
    if (s->tag() == asttags::Function) {
      return s->id();
    }
  }

  return ID();
}

static void checkForParenlessMethodFieldRedefinition(Context* context,
                                                     const Function* fn,
                                                     Resolver& visitor) {

  if (fn->isMethod() && fn->isParenless()) {
    QualifiedType receiverType;
    ID receiverId;
    if (visitor.getMethodReceiver(&receiverType, &receiverId)) {
      if (receiverType.type()) {
        // use the type information, if it is present
        if (auto ct = receiverType.type()->getCompositeType()) {
          receiverId = ct->id();
        }
      }
      if (!receiverId.isEmpty()) {
        if (parsing::idContainsFieldWithName(context, receiverId, fn->name())) {
          context->error(fn, "parenless proc redeclares the field '%s'",
                         fn->name().c_str());
        }
      }
    }
  }
}

static const TypedFnSignature* const&
typedSignatureInitialQuery(Context* context,
                           const UntypedFnSignature* untypedSig) {
  QUERY_BEGIN(typedSignatureInitialQuery, context, untypedSig);

  const TypedFnSignature* result = nullptr;
  const AstNode* ast = parsing::idToAst(context, untypedSig->id());
  const Function* fn = ast->toFunction();

  if (fn != nullptr) {
    // look at the parent scopes to find the parent function, if any
    const UntypedFnSignature* parentFnUntyped = nullptr;
    const TypedFnSignature* parentFnTyped = nullptr;
    ID parentFnId = parentFunctionId(context, fn->id());
    if (!parentFnId.isEmpty()) {
      auto parentAst = parsing::idToAst(context, parentFnId);
      auto parentFn = parentAst->toFunction();
      parentFnUntyped = UntypedFnSignature::get(context, parentFn);
      parentFnTyped = typedSignatureInitial(context, parentFnUntyped);
    }

    ResolutionResultByPostorderID r;
    auto visitor = Resolver::createForInitialSignature(context, fn, r);
    // visit the formals
    for (auto formal : fn->formals()) {
      formal->traverse(visitor);
    }
    // do not visit the return type or function body

    // now, construct a TypedFnSignature from the result
    std::vector<types::QualifiedType> formalTypes = visitor.getFormalTypes(fn);
    bool needsInstantiation = anyFormalNeedsInstantiation(context, formalTypes,
                                                          untypedSig,
                                                          nullptr);

    // visit the where clause, unless it needs to be instantiated, in
    // which case we will visit the where clause when that happens
    TypedFnSignature::WhereClauseResult whereResult =
      TypedFnSignature::WHERE_NONE;
    if (auto whereClause = fn->whereClause()) {
      if (needsInstantiation) {
        whereResult = TypedFnSignature::WHERE_TBD;
      } else {
        whereClause->traverse(visitor);
        whereResult = whereClauseResult(context, fn, r, needsInstantiation);
      }
    }

    checkForParenlessMethodFieldRedefinition(context, fn, visitor);

    result = TypedFnSignature::get(context,
                                   untypedSig,
                                   std::move(formalTypes),
                                   whereResult,
                                   needsInstantiation,
                                   /* instantiatedFrom */ nullptr,
                                   /* parentFn */ parentFnTyped,
                                   /* formalsInstantiated */ Bitmap());

  }

  return QUERY_END(result);
}

const TypedFnSignature*
typedSignatureInitial(Context* context,
                      const UntypedFnSignature* untypedSig) {

  auto ret = typedSignatureInitialQuery(context, untypedSig);
  // also check the signature at this point if it is concrete
  if (!ret->needsInstantiation()) {
    checkSignature(context, ret);
  }
  return ret;
}


// initedInParent is true if the decl variable is inited due to a parent
// uast node.  This comes up for TupleDecls.
static void helpSetFieldTypes(const AstNode* ast,
                              ResolutionResultByPostorderID& r,
                              bool initedInParent,
                              ResolvedFields& fields) {

  if (auto var = ast->toVarLikeDecl()) {
    bool hasDefaultValue = initedInParent || var->initExpression() != nullptr;
    const ResolvedExpression& e = r.byAst(var);
    fields.addField(var->name(), hasDefaultValue, var->id(), e.type());
  } else if (auto mult = ast->toMultiDecl()) {
    for (auto decl : mult->decls()) {
      helpSetFieldTypes(decl, r, initedInParent, fields);
    }
  } else if (auto tup = ast->toTupleDecl()) {
    bool hasInit = initedInParent || tup->initExpression() != nullptr;
    for (auto decl : tup->decls()) {
      helpSetFieldTypes(decl, r, hasInit, fields);
    }
  } else if (auto fwd = ast->toForwardingDecl()) {
    if (auto fwdTo = fwd->expr()) {
      if (fwdTo->isDecl()) {
        helpSetFieldTypes(fwd->expr(), r, initedInParent, fields);
      }
      fields.addForwarding(fwd->id(), r.byAst(fwdTo).type());
    }
  }

  // no action needed for other types of Decls since they aren't fields
  // and can't contain fields
}

static const Type* const&
initialTypeForTypeDeclQuery(Context* context, ID declId) {
  QUERY_BEGIN(initialTypeForTypeDeclQuery, context, declId);

  const Type* result = nullptr;

  auto ast = parsing::idToAst(context, declId);
  if (auto ad = ast->toAggregateDecl()) {
    result = helpGetTypeForDecl(context, ad,
                                SubstitutionsMap(),
                                /* poiScope */ nullptr,
                                /* instantiatedFrom */ nullptr);

    if (result != nullptr) {
      if (auto bct = result->toBasicClassType()) {
        // for a 'class C' declaration, 'C' normally means a generic-management
        // non-nil C so adjust the result.
        auto dec = ClassTypeDecorator(ClassTypeDecorator::GENERIC_NONNIL);
        result = ClassType::get(context, bct, /*manager*/ nullptr, dec);
      }
    }
  } else if (auto td = ast->toEnum()) {
    result = EnumType::get(context, td->id(), td->name());
  }

  return QUERY_END(result);
}

const Type* initialTypeForTypeDecl(Context* context, ID declId) {
  return initialTypeForTypeDeclQuery(context, declId);
}

const ResolvedFields& resolveFieldDecl(Context* context,
                                       const CompositeType* ct,
                                       ID fieldId,
                                       DefaultsPolicy defaultsPolicy) {
  QUERY_BEGIN(resolveFieldDecl, context, ct, fieldId, defaultsPolicy);

  ResolvedFields result;
  bool isObjectType = false;
  if (auto bct = ct->toBasicClassType()) {
    isObjectType = bct->isObjectType();
  }

  if (isObjectType) {
    // no need to try to resolve the fields for the object type,
    // which doesn't have a real uAST ID.

  } else {
    auto typeAst = parsing::idToAst(context, ct->id());
    CHPL_ASSERT(typeAst && typeAst->isAggregateDecl());
    auto ad = typeAst->toAggregateDecl();

    auto fieldAst = parsing::idToAst(context, fieldId);
    CHPL_ASSERT(fieldAst);

    if (ct->instantiatedFromCompositeType() == nullptr) {
      // handle resolving a not-yet-instantiated type
      ResolutionResultByPostorderID r;
      auto visitor =
        Resolver::createForInitialFieldStmt(context, ad, fieldAst,
                                            ct, r, defaultsPolicy);

      // resolve the field types and set them in 'result'
      fieldAst->traverse(visitor);
      helpSetFieldTypes(fieldAst, r, /* initedInParent */ false, result);
    } else {
      // handle resolving an instantiated type

      // use nullptr for POI scope because POI is not considered
      // when resolving the fields when constructing a type..
      const PoiScope* poiScope = nullptr;
      ResolutionResultByPostorderID r;
      auto visitor =
        Resolver::createForInstantiatedFieldStmt(context, ad, fieldAst, ct,
                                                 poiScope, r,
                                                 defaultsPolicy);

      // resolve the field types and set them in 'result'
      fieldAst->traverse(visitor);
      helpSetFieldTypes(fieldAst, r, /* initedInParent */ false, result);
    }
  }


  return QUERY_END(result);
}

static
const ResolvedFields& fieldsForTypeDeclQuery(Context* context,
                                             const CompositeType* ct,
                                             DefaultsPolicy defaultsPolicy) {
  QUERY_BEGIN(fieldsForTypeDeclQuery, context, ct, defaultsPolicy);

  ResolvedFields result;

  CHPL_ASSERT(ct);
  result.setType(ct);

  bool isObjectType = false;
  if (auto bct = ct->toBasicClassType()) {
    isObjectType = bct->isObjectType();
  }
  bool isMissingBundledType =
    CompositeType::isMissingBundledType(context, ct->id());

  if (isObjectType || isMissingBundledType) {
    // no need to try to resolve the fields for the object type,
    // which doesn't have a real uAST ID.
    // for built-in types like Errors when we didn't parse the standard library
    // don't try to resolve the fields
  } else {
    auto ast = parsing::idToAst(context, ct->id());
    CHPL_ASSERT(ast && ast->isAggregateDecl());
    auto ad = ast->toAggregateDecl();

    for (auto child: ad->children()) {
      // Ignore everything other than VarLikeDecl, MultiDecl, TupleDecl
      bool isForwardingField = child->isForwardingDecl() &&
                               child->toForwardingDecl()->expr()->isDecl();
      if (child->isVarLikeDecl() ||
          child->isMultiDecl() ||
          child->isTupleDecl() ||
          isForwardingField) {
        const ResolvedFields& resolvedFields =
          resolveFieldDecl(context, ct, child->id(), defaultsPolicy);
        // Copy resolvedFields into result
        int n = resolvedFields.numFields();
        for (int i = 0; i < n; i++) {
          result.addField(resolvedFields.fieldName(i),
                          resolvedFields.fieldHasDefaultValue(i),
                          resolvedFields.fieldDeclId(i),
                          resolvedFields.fieldType(i));
        }
        result.addForwarding(resolvedFields);
      }
    }

    // finalize the field types to compute summary information
    // like whether any was generic
    result.finalizeFields(context);
  }

  return QUERY_END(result);
}

const ResolvedFields& fieldsForTypeDecl(Context* context,
                                        const CompositeType* ct,
                                        DefaultsPolicy defaultsPolicy) {
  if (defaultsPolicy == DefaultsPolicy::IGNORE_DEFAULTS){
    return fieldsForTypeDeclQuery(context, ct, DefaultsPolicy::IGNORE_DEFAULTS);
  }

  // try first with defaultsPolicy=FOR_OTHER_FIELDS
  const auto& f = fieldsForTypeDeclQuery(context, ct,
                                         DefaultsPolicy::USE_DEFAULTS_OTHER_FIELDS);

  // If defaultsPolicy=USE was requested and the type
  // is generic with defaults, compute the type again.
  // We do it this way so that we are more likely to be able to reuse the
  // result of the above query in most cases since most types
  // are not generic record/class with defaults.
  if (defaultsPolicy == DefaultsPolicy::USE_DEFAULTS) {
    // if record is not generic with defaults, return its
    // uninstantiated fields. Otherwise, instantiate.
    auto finalDefaultsPolicy = f.isGenericWithDefaults() ?
      DefaultsPolicy::USE_DEFAULTS :
      DefaultsPolicy::IGNORE_DEFAULTS;
    return fieldsForTypeDeclQuery(context, ct, finalDefaultsPolicy);
  }

  // Otherwise, use the value we just computed.
  return f;
}

// Resolve all statements like 'forwarding _value;' in 'ct'
static
const ResolvedFields& resolveForwardingExprs(Context* context,
                                             const CompositeType* ct) {
  QUERY_BEGIN(resolveForwardingExprs, context, ct);

  ResolvedFields result;

  CHPL_ASSERT(ct);
  result.setType(ct);

  bool isObjectType = false;
  if (auto bct = ct->toBasicClassType()) {
    isObjectType = bct->isObjectType();
  }
  bool isMissingBundledType =
    CompositeType::isMissingBundledType(context, ct->id());

  if (isObjectType || isMissingBundledType) {
    // no need to try to resolve the fields for the object type,
    // which doesn't have a real uAST ID.
    // for built-in types like Errors when we didn't parse the standard library
    // don't try to resolve the fields
  } else {
    auto ast = parsing::idToAst(context, ct->id());
    CHPL_ASSERT(ast && ast->isAggregateDecl());
    auto ad = ast->toAggregateDecl();

    // TODO: don't rely on 'ResolvedFields' or 'resolveFieldDecl' here...
    for (auto child: ad->children()) {
      if (child->isForwardingDecl() &&
          !child->toForwardingDecl()->expr()->isDecl()) {
        const ResolvedFields& resolvedFields =
          resolveFieldDecl(context, ct, child->id(), DefaultsPolicy::USE_DEFAULTS);
        result.addForwarding(resolvedFields);
      }
    }
  }

  return QUERY_END(result);
}

static bool typeUsesForwarding(Context* context, const Type* receiverType) {
  if (auto ct = receiverType->getCompositeType()) {
    if (ct->isBasicClassType() || ct->isRecordType() || ct->isUnionType()) {
      ID ctId = ct->id();
      if (!ctId.isEmpty()) {
        return parsing::aggregateUsesForwarding(context, ctId);
      }
    }
  }

  return false;
}

// returns 'true' if a cycle was detected
static bool
checkForwardingCycles(Context* context,
                      const CompositeType* ct,
                      llvm::SmallPtrSet<const CompositeType*, 8>& visited) {

  if (typeUsesForwarding(context, ct)) {
    auto pair = visited.insert(ct);
    if (pair.second == false) {
      // it was already in the visited set
      context->error(ct->id(), "forwarding cycle detected");
      return true;
    }

    const ResolvedFields& r = fieldsForTypeDecl(context, ct,
                                                DefaultsPolicy::USE_DEFAULTS);

    // Check for cycles. If a cycle is detected, emit an error
    // and return 'true'.
    int n = r.numForwards();
    for (int i = 0; i < n; i++) {
      auto qt = r.forwardingToType(i);
      if (auto t = qt.type()) {
        if (auto forwardingCt = t->getCompositeType()) {
          bool cyc = checkForwardingCycles(context, forwardingCt, visited);
          if (cyc) {
            return true;
          }
        }
      }
    }
  }
  return false;
}


// returns a 'true' if there was a cycle and reports an error in that case.
// otherwise, returns 'false'.
static const bool&
forwardingCycleCheckQuery(Context* context, const CompositeType* ct) {
  QUERY_BEGIN(forwardingCycleCheckQuery, context, ct);

  bool result = false;
  llvm::SmallPtrSet<const CompositeType*, 8> visited;

  result = checkForwardingCycles(context, ct, visited);

  return QUERY_END(result);
}

// returns 'true' if a forwarding cycle was detected & error emitted
static bool
emitErrorForForwardingCycles(Context* context, const CompositeType* ct) {
  bool cycleFound = false;
  if (typeUsesForwarding(context, ct)) {
    // check for cycles
    cycleFound = forwardingCycleCheckQuery(context, ct);
  }

  return cycleFound;
}

static const CompositeType* getTypeWithDefaults(Context* context,
                                                const CompositeType* ct) {
  // resolve the fields with DefaultsPolicy=FOR_OTHER_FIELDS
  const ResolvedFields& g = fieldsForTypeDecl(context, ct,
                                              DefaultsPolicy::USE_DEFAULTS_OTHER_FIELDS);
  if (!g.isGenericWithDefaults()) {
    return ct;
  }

  // and with DefaultsPolicy=USE
  const ResolvedFields& r = fieldsForTypeDecl(context, ct,
                                              DefaultsPolicy::USE_DEFAULTS);

  // for any field that has a different type in r than in g, add
  // a substitution, and get the type with those substitutions.
  SubstitutionsMap substitutions;
  int nFields = g.numFields();
  CHPL_ASSERT(r.numFields() == nFields);
  for (int i = 0; i < nFields; i++) {
    CHPL_ASSERT(g.fieldName(i) == r.fieldName(i));
    CHPL_ASSERT(g.fieldDeclId(i) == r.fieldDeclId(i));
    QualifiedType gType = g.fieldType(i);
    QualifiedType rType = r.fieldType(i);
    if (gType != rType) {
      // The type is different so add a substitution.
      substitutions.insert({g.fieldDeclId(i), rType});
    }
  }

  if (substitutions.size() == 0) {
    return ct;
  }

  auto ast = parsing::idToAst(context, ct->id());
  CHPL_ASSERT(ast && ast->isAggregateDecl());
  auto ad = ast->toAggregateDecl();

  // POI is not relevant here
  const PoiScope* poiScope = nullptr;
  const Type* instantiatedFrom = ct;

  // Create the composite type with those substitutions
  return helpGetTypeForDecl(context, ad, substitutions,
                            poiScope, instantiatedFrom);
}

static
const CompositeType* const& getTypeWithDefaultsQuery(Context* context,
                                                     const CompositeType* ct) {
  QUERY_BEGIN(getTypeWithDefaultsQuery, context, ct);

  auto result = getTypeWithDefaults(context, ct);

  return QUERY_END(result);
}

const types::QualifiedType typeWithDefaults(Context* context,
                                            types::QualifiedType t) {
  if (t.type()) {
    if (auto clst = t.type()->toClassType()) {
      if (auto bct = clst->basicClassType()) {
        auto got = getTypeWithDefaultsQuery(context, bct);
        CHPL_ASSERT(got->isBasicClassType());
        bct = got->toBasicClassType();

        auto r = ClassType::get(context, bct, clst->manager(), clst->decorator());
        return QualifiedType(t.kind(), r, t.param());
      }
    } else if (auto ct = t.type()->toCompositeType()) {
      auto got = getTypeWithDefaultsQuery(context, ct);
      return QualifiedType(t.kind(), got, t.param());
    }
  }

  return t;
}


// the ignore argument is just to ignore types that we are currently
// computing the genericity of (we can assume that those are concrete).
// that is important for recursive class types (e.g. a linked list).
static Type::Genericity getFieldsGenericity(Context* context,
                                            const CompositeType* ct,
                                            std::set<const Type*>& ignore) {
  // Figure out the genericity of the type based on the genericity
  // of the fields.

  // add the current type to the ignore set, and stop now
  // if it is already in the ignore set.
  auto it = ignore.insert(ct);
  if (it.second == false) {
    // set already contained ct, so stop & consider it concrete
    return Type::CONCRETE;
  }

  // compute genericity of tuple types
  if (auto tt = ct->toTupleType()) {
    Type::Genericity combined = Type::CONCRETE;
    int n = tt->numElements();
    for (int i = 0; i < n; i++) {
      auto g = getTypeGenericityIgnoring(context, tt->elementType(i), ignore);
      CHPL_ASSERT(g != Type::MAYBE_GENERIC);
      if (g == Type::GENERIC) {
        combined = g;
      } else if (g == Type::GENERIC_WITH_DEFAULTS &&
                 combined == Type::CONCRETE) {
        combined = g;
      }
    }
    return combined;
  } else if (auto dt = ct->toDomainType()) {
    Type::Genericity combined = Type::CONCRETE;

    // Allows for instantiation of things like 'arg: domain'
    // TODO: currently partially generic domains are not supported
    if (dt->kind() == DomainType::Kind::Unknown) {
      combined = Type::GENERIC;
    }

    return combined;
  } else if (auto at = ct->toArrayType()) {
    auto dt = getTypeGenericityIgnoring(context, at->domainType(), ignore);
    auto et = getTypeGenericityIgnoring(context, at->eltType(), ignore);

    if (dt != Type::CONCRETE || et != Type::CONCRETE) {
      return Type::GENERIC;
    } else {
      return Type::CONCRETE;
    }
  }

  // Some testing code creates CompositeType with empty IDs.
  // Assume these are concrete.
  // Also 'object' has an empty ID and is concrete.
  if (ct->id().isEmpty())
    return Type::CONCRETE;

  Type::Genericity g = Type::CONCRETE;

  if (auto bct = ct->toBasicClassType()) {
    g = getFieldsGenericity(context, bct->parentClassType(), ignore);
    CHPL_ASSERT(g != Type::MAYBE_GENERIC);
    if (g == Type::GENERIC)
      return Type::GENERIC;
  }

  if (context->isQueryRunning(fieldsForTypeDeclQuery,
                              std::make_tuple(ct, DefaultsPolicy::IGNORE_DEFAULTS)) ||
      context->isQueryRunning(fieldsForTypeDeclQuery,
                              std::make_tuple(ct, DefaultsPolicy::USE_DEFAULTS)) ||
      context->isQueryRunning(fieldsForTypeDeclQuery,
                              std::make_tuple(ct, DefaultsPolicy::USE_DEFAULTS_OTHER_FIELDS))) {
    // TODO: is there a better way to avoid problems with recursion here?
    return Type::CONCRETE;
  }

  // we only care about whether or not each field is generic on its own
  // merit, as only these fields need defaults. Thus, we allow defaults
  // for fields other than the one we are checking. In this way, we prevent
  // some field (a) that depends on the value of field (b) from being
  // marked generic just because (b) is generic.
  DefaultsPolicy defaultsPolicy = DefaultsPolicy::USE_DEFAULTS_OTHER_FIELDS;
  const ResolvedFields& f = fieldsForTypeDecl(context, ct,
                                              defaultsPolicy);

  if (f.isGenericWithDefaults() &&
      (g == Type::CONCRETE || g == Type::GENERIC_WITH_DEFAULTS))
    return Type::GENERIC_WITH_DEFAULTS;

  if (f.isGeneric())
    return Type::GENERIC;

  return g;
}

Type::Genericity getTypeGenericityIgnoring(Context* context, const Type* t,
                                           std::set<const Type*>& ignore) {
  if (t == nullptr)
    return Type::MAYBE_GENERIC;

  // check if the type knows the full answer
  Type::Genericity ret = t->genericity();

  if (ret != Type::MAYBE_GENERIC)
    return ret;

  if (t->isUnknownType())
    return Type::MAYBE_GENERIC;

  if (auto pt = t->toCPtrType()) {
    // Mimics the fields logic: if any field is non-concrete, the whole
    // type is generic. Logically, the c_ptr has a single field, the element
    // type.
    if (getTypeGenericityIgnoring(context, pt->eltType(), ignore) == Type::CONCRETE) {
      return Type::CONCRETE;
    } else {
      return Type::GENERIC;
    }
  }

  // MAYBE_GENERIC should only be returned for CompositeType /
  // ClassType right now.
  CHPL_ASSERT(t->isCompositeType() || t->isClassType());

  // the tuple type that isn't an instantiation is a generic type
  if (auto tt = t->toTupleType()) {
    if (tt->instantiatedFromCompositeType() == nullptr)
      return Type::GENERIC;
    if (tt->isKnownSize() == false) {
      return Type::GENERIC;
    }
  }

  // string and bytes types are never generic
  if (t->isStringType() || t->isBytesType()) {
    return Type::CONCRETE;
  }

  if (auto classType = t->toClassType()) {
    // should be handled in BasicClassType::isGeneric
    // so this code should only be called if the management is concrete
    CHPL_ASSERT(!classType->decorator().isUnknownManagement());
    CHPL_ASSERT(!classType->decorator().isUnknownNilability());

    auto mt = classType->manageableType();
    if (auto bct = mt->toBasicClassType()) {
      return getFieldsGenericity(context, bct, ignore);
    } else {
      CHPL_ASSERT(mt->isAnyClassType());
      return Type::GENERIC;
    }
  }

  auto compositeType = t->toCompositeType();
  return getFieldsGenericity(context, compositeType, ignore);
}

Type::Genericity getTypeGenericityIgnoring(Context* context, QualifiedType qt,
                                           std::set<const Type*>& ignore) {
   Type::Genericity g = qt.genericity();
   if (g == Type::MAYBE_GENERIC && qt.type() != nullptr ) {
     return resolution::getTypeGenericityIgnoring(context, qt.type(), ignore);
   }

   // otherwise return whatever we computed
   return g;
}

Type::Genericity getTypeGenericity(Context* context, const Type* t) {
  std::set<const Type*> ignore;
  return getTypeGenericityIgnoring(context, t, ignore);
}

Type::Genericity getTypeGenericity(Context* context, QualifiedType qt) {
  std::set<const Type*> ignore;
  return getTypeGenericityIgnoring(context, qt, ignore);
}

// Returns true if the field should be included in the type constructor.
// In that event, also sets formalType to the type the formal should use.
static
bool shouldIncludeFieldInTypeConstructor(Context* context,
                                         const Decl* fieldDecl,
                                         const QualifiedType& fieldType,
                                         QualifiedType& formalType) {
  // compare with AggregateType::fieldIsGeneric

  // fields with concrete types don't need to be in type constructor
  if (!fieldType.isGenericOrUnknown()) {
    return false;
  }

  // fields that are 'type' or 'param' are generic
  // and we can use the same type/param intent for the type constructor
  if ((fieldType.isParam() && !fieldType.hasParamPtr()) ||
      fieldType.isType()) {
    formalType = fieldType;
    return true;
  }

  if (const VarLikeDecl* var = fieldDecl->toVarLikeDecl()) {
    // non-type/param fields with an init expression aren't generic
    if (var->initExpression())
      return false;

    // non-type/param fields that have no declared type and no initializer
    // are generic and these need a type variable for the argument with AnyType.
    if (var->typeExpression() == nullptr) {
      formalType = QualifiedType(QualifiedType::TYPE, AnyType::get(context));
      return true;
    }

    // otherwise, the field may or may not be generic.
    // it is generic if the field type is generic.
    // for this check we make some simplifying assumptions:
    //  * generic-with-defaults means concrete, unless ? is used in the type
    //  * unknown type means it depends on a previous generic field
    //    (and when previous generic fields are set, they will be concrete)
    const Type* t = fieldType.type();
    if (t && !t->isUnknownType()) {
      Type::Genericity g = getTypeGenericity(context, t);
      if (g == Type::GENERIC) { // and not GENERIC_WITH_DEFAULTS
        formalType = QualifiedType(QualifiedType::TYPE, t);
        return true;
      }
    }
  }

  // otherwise it does not need to go into the type constructor
  return false;
}

static const TypedFnSignature* const&
typeConstructorInitialQuery(Context* context, const Type* t)
{
  QUERY_BEGIN(typeConstructorInitialQuery, context, t);

  const TypedFnSignature* result = nullptr;

  ID id;
  UniqueString name;
  std::vector<UntypedFnSignature::FormalDetail> formals;
  std::vector<types::QualifiedType> formalTypes;
  auto idTag = uast::asttags::AST_TAG_UNKNOWN;

  if (auto ct = t->getCompositeType()) {
    id = ct->id();
    name = ct->name();

    // attempt to resolve the fields
    DefaultsPolicy defaultsPolicy = DefaultsPolicy::IGNORE_DEFAULTS;
    const ResolvedFields& f = fieldsForTypeDecl(context, ct,
                                                defaultsPolicy);

    // find the generic fields from the type and add
    // these as type constructor arguments.
    int nFields = f.numFields();
    for (int i = 0; i < nFields; i++) {
      auto declId = f.fieldDeclId(i);
      auto declAst = parsing::idToAst(context, declId);
      CHPL_ASSERT(declAst);
      const Decl* fieldDecl = declAst->toDecl();
      CHPL_ASSERT(fieldDecl);
      QualifiedType fieldType = f.fieldType(i);
      QualifiedType formalType;
      if (shouldIncludeFieldInTypeConstructor(context, fieldDecl, fieldType,
                                              formalType)) {

        auto d = UntypedFnSignature::FormalDetail(f.fieldName(i),
                                                  f.fieldHasDefaultValue(i),
                                                  fieldDecl,
                                                  fieldDecl->isVarArgFormal());
        formals.push_back(d);
        // formalType should have been set above
        CHPL_ASSERT(formalType.kind() != QualifiedType::UNKNOWN);
        formalTypes.push_back(formalType);
      }
    }

    if (t->isBasicClassType() || t->isClassType()) {
      idTag = uast::asttags::Class;
    } else if (t->isRecordType()) {
      idTag = uast::asttags::Record;
    } else if (t->isUnionType()) {
      idTag = uast::asttags::Union;
    }
  } else {
    CHPL_ASSERT(false && "case not handled");
  }

  auto untyped = UntypedFnSignature::get(context,
                                         id, name,
                                         /* isMethod */ false,
                                         /* isTypeConstructor */ true,
                                         /* isCompilerGenerated */ true,
                                         /* throws */ false,
                                         idTag,
                                         Function::PROC,
                                         std::move(formals),
                                         /* whereClause */ nullptr);

  result = TypedFnSignature::get(context,
                                 untyped,
                                 std::move(formalTypes),
                                 TypedFnSignature::WHERE_NONE,
                                 /* needsInstantiation */ true,
                                 /* instantiatedFrom */ nullptr,
                                 /* parentFn */ nullptr,
                                 /* formalsInstantiated */ Bitmap());

  return QUERY_END(result);
}

const TypedFnSignature* typeConstructorInitial(Context* context,
                                               const types::Type* t) {
  return typeConstructorInitialQuery(context, t);
}

QualifiedType getInstantiationType(Context* context,
                                   QualifiedType actualType,
                                   QualifiedType formalType) {

  // The formal is generic but the actual might require a coercion
  // on the way to it. In that event, instantiate the formal type
  // using the type that the actual will coerce to.

  // E.g. a MyClass actual passed to an x:borrowed? formal
  // should instantiate with MyClass?

  const Type* actualT = actualType.type();
  const Type* formalT = formalType.type();

  CHPL_ASSERT(actualT != nullptr);
  CHPL_ASSERT(formalT != nullptr);

  // this function should only be called when instantiation is required
  CHPL_ASSERT(canPass(context, actualType, formalType).passes());
  CHPL_ASSERT(canPass(context, actualType, formalType).instantiates());

  if (auto actualCt = actualT->toClassType()) {
    // handle decorated class passed to decorated class
    if (auto formalCt = formalT->toClassType()) {
      // which decorator to use?
      auto dec = formalCt->decorator().combine(actualCt->decorator());

      // which manager to use?
      const Type* manager = nullptr;
      if (dec.isManaged()) {
        // there aren't implicit conversions from managed -> managed,
        // so we can always use the actual's manager if the combined
        // decorator indicates management.
        CHPL_ASSERT(actualCt->decorator().isManaged() && actualCt->manager());
        manager = actualCt->manager();
      }

      // which BasicClassType to use?
      const BasicClassType* bct;
      if (auto formalBct = formalCt->basicClassType()) {
        bct = formalBct;
      } else {
        CHPL_ASSERT(formalCt->manageableType()->toManageableType());
        bct = actualCt->basicClassType();
      }
      auto g = getTypeGenericity(context, bct);
      if (g != Type::CONCRETE) {
        CHPL_UNIMPL("instantiate generic class formal");
      }

      // now construct the ClassType
      auto ct = ClassType::get(context, bct, manager, dec);
      return QualifiedType(formalType.kind(), ct);
    }

    // handle decorated class passed to special built-in type
    auto classBuiltinTypeDec = ClassTypeDecorator::GENERIC;
    bool foundClassyBuiltinType = true;

    if (formalT->isAnyOwnedType() &&
               actualCt->decorator().isManaged() &&
               actualCt->manager()->isAnyOwnedType()) {
      classBuiltinTypeDec = ClassTypeDecorator::MANAGED;
    } else if (formalT->isAnySharedType() &&
               actualCt->decorator().isManaged() &&
               actualCt->manager()->isAnySharedType()) {
      classBuiltinTypeDec = ClassTypeDecorator::MANAGED;
    } else {
      foundClassyBuiltinType = false;
    }

    if (foundClassyBuiltinType) {
      // which basic class type?
      const BasicClassType* bct = actualCt->basicClassType();

      // which decorator?
      auto formalDec = ClassTypeDecorator(classBuiltinTypeDec);
      auto dec = formalDec.combine(actualCt->decorator());

      // which manager?
      const Type* manager = nullptr;
      if (dec.isManaged())
        manager = actualCt->manager();

      // now construct the ClassType
      auto ct = ClassType::get(context, bct, manager, dec);
      return QualifiedType(formalType.kind(), ct);
    }
  }

  // TODO: sync type -> value type?
  CHPL_ASSERT(false && "case not handled");
  return QualifiedType();
}

static bool varArgCountMatch(const VarArgFormal* formal,
                             ResolutionResultByPostorderID& r) {
  QualifiedType formalType = r.byAst(formal).type();
  auto tupleType = formalType.type()->toTupleType();

  if (formal->count() != nullptr) {
    const ResolvedExpression& count = r.byAst(formal->count());
    QualifiedType ct = count.type();
    if (ct.isParam() && ct.param() != nullptr) {
      auto numElements = tupleType->numElements();
      if (auto ip = ct.param()->toIntParam()) {
        return numElements == ip->value();
      } else if (auto up = ct.param()->toUintParam()) {
        return (uint64_t)numElements == up->value();
      } else {
        // TODO: Error message about coercing non-integrals in the
        // count-expression.
        return false;
      }
    }
  }

  return true;
}

static QualifiedType getVarArgTupleElemType(const QualifiedType& varArgType) {
  // If the type is a VarArgTuple then we should use its 'star' type
  // with 'canPass'.
  //
  // Note: Unless there was an error resolving the type, this tuple
  // should be a VarArgTuple
  //
  // TODO: Should we update 'canPass' to reason about VarArgTuples?
  const TupleType* tup = varArgType.type()->toTupleType();
  if (tup != nullptr && tup->isVarArgTuple()) {
    return tup->starType();
  } else {
    return varArgType;
  }
}

static Resolver createResolverForFnOrAd(Context* context,
                                        const Function* fn,
                                        const AggregateDecl* ad,
                                        const SubstitutionsMap& substitutions,
                                        const PoiScope* poiScope,
                                        ResolutionResultByPostorderID& r) {
  if (fn != nullptr) {
    return Resolver::createForInstantiatedSignature(context, fn, substitutions,
                                                    poiScope, r);
  } else {
    CHPL_ASSERT(ad != nullptr);
    return Resolver::createForInstantiatedSignatureFields(context, ad,
                                                          substitutions,
                                                          poiScope, r);
  }
}

static QualifiedType getProperFormalType(const ResolutionResultByPostorderID& r,
                                         const FormalActual& entry,
                                         const AggregateDecl* ad,
                                         const AstNode* typeFor) {
  auto type = r.byAst(typeFor).type();
  if (ad != nullptr) {
    // generic var fields from a type are type fields in its type constructor.
    // so, make sure the kind is correct.
    type = QualifiedType(entry.formalType().kind(),
                         type.type(),
                         type.param());
  }
  return type;
}

static bool isCallInfoForInitializer(const CallInfo& ci) {
  if (ci.name() == USTR("init") || ci.name() == USTR("init="))
    if (ci.isMethodCall())
      return true;
  return false;
}

// TODO: Move these to the 'InitResolver' visitor.
static bool isTfsForInitializer(const TypedFnSignature* tfs) {
  if (tfs->untyped()->name() == USTR("init") ||
      tfs->untyped()->name() == USTR("init="))
    if (tfs->untyped()->isMethod())
      return true;
  return false;
}

static bool ensureBodyIsResolved(Context* context, const CallInfo& ci,
                                 const TypedFnSignature* tfs) {
  if (tfs->untyped()->isCompilerGenerated()) return false;
  if (isTfsForInitializer(tfs)) return true;
  return false;
}

ApplicabilityResult instantiateSignature(Context* context,
                                         const TypedFnSignature* sig,
                                         const CallInfo& call,
                                         const PoiScope* poiScope) {
  // Performance: Should this query use a similar approach to
  // resolveFunctionByInfoQuery, where the PoiInfo and visibility
  // are consulted?
  //
  // It does not impact correctness, because typedSignatureQuery
  // will arrange to construct a unique TypedFnSignature by
  // its contents.

  CHPL_ASSERT(sig->needsInstantiation());

  const UntypedFnSignature* untypedSignature = sig->untyped();
  const AstNode* ast = nullptr;
  const Function* fn = nullptr;
  const AggregateDecl* ad = nullptr;

  if (!untypedSignature->id().isEmpty()) {
    ast = parsing::idToAst(context, untypedSignature->id());
    fn = ast->toFunction();
    ad = ast->toAggregateDecl();
  }

  const TypedFnSignature* parentFnTyped = nullptr;
  if (sig->parentFn()) {
    CHPL_ASSERT(false && "generic child functions not yet supported");
    // TODO: how to compute parentFn for the instantiation?
    // Does the parent function need to be instantiated in some case?
    // Set parentFnTyped somehow.
  }

  auto faMap = FormalActualMap(sig, call);
  if (!faMap.isValid()) {
    return ApplicabilityResult::failure(sig->id(), FAIL_FORMAL_ACTUAL_MISMATCH);
  }

  // compute the substitutions
  SubstitutionsMap substitutions;
  Bitmap formalsInstantiated;
  int formalIdx = 0;

  bool instantiateVarArgs = false;
  std::vector<QualifiedType> varargsTypes;
  int varArgIdx = -1;

  ResolutionResultByPostorderID r;
  auto visitor = createResolverForFnOrAd(context, fn, ad, substitutions,
                                         poiScope, r);

  QualifiedType varArgType;
  for (const FormalActual& entry : faMap.byFormals()) {
    // Do not ignore substitutions initially
    visitor.ignoreSubstitutionFor = nullptr;
    visitor.skipTypeQueries = false;

    bool addSub = false;
    QualifiedType useType;
    const auto formal = untypedSignature->formalDecl(entry.formalIdx());
    const auto& actualType = entry.actualType();

    // Re-compute the formal type using substitutions if needed.
    // Performance: we can start doing this only after the first substitution
    //              is created
    QualifiedType formalType;
    if (entry.isVarArgEntry()) {
      if (varArgType.isUnknown()) {
        // We haven't yet re-computed the vararg tuple type.
        formal->traverse(visitor);
        varArgType = r.byAst(formal).type();
      }
      formalType = getVarArgTupleElemType(varArgType);
    } else {
      formal->traverse(visitor);
      formalType = getProperFormalType(r, entry, ad, formal);
    }

    // note: entry.actualType can have type()==nullptr and UNKNOWN.
    // in that case, resolver code should treat it as a hint to
    // use the default value. Unless the call used a ? argument.
    if (actualType.kind() == QualifiedType::UNKNOWN &&
        actualType.type() == nullptr) {
      if (call.hasQuestionArg()) {
        // don't add any substitution
      } else {
        // add a "use the default" hint substitution.
        addSub = true;
        useType = actualType;
      }
    } else {
      auto got = canPass(context, actualType, formalType);
      if (!got.passes()) {
        // Including past type information made this instantiation fail.
        return ApplicabilityResult::failure(sig, got.reason(), entry.formalIdx());
      }
      if (got.instantiates()) {
        // add a substitution for a valid value
        if (!got.converts() && !got.promotes()) {
          // use the actual type since no conversion/promotion was needed
          addSub = true;
          useType = actualType;
        } else {
          // get instantiation type
          addSub = true;
          useType = getInstantiationType(context,
                                         actualType,
                                         formalType);

          // Verify that the 'instantiation type' still accepts the actual.
          // This might not be the case based on legal argument mapping rules.
          //
          // For instance, we can successfully instantiate 'ref x: Parent'
          // with 'shared Child', leading to a 'ref x: shared Parent'
          // useType. However, we cannot pass a 'shared Child' to a
          // 'ref x: shared Parent' formal, because 'ref' requires the types
          // to match exactly, and rules out subtype conversions.

          auto kind = resolveIntent(useType, /* isThis */ false, /* isInit */ false);
          auto useTypeConcrete = QualifiedType(kind, useType.type(), useType.param());

          auto got = canPass(context, actualType, useTypeConcrete);
          if (!got.passes()) {
            return ApplicabilityResult::failure(sig, got.reason(), entry.formalIdx());
          }
        }
      }
    }

    if (entry.isVarArgEntry()) {
      // If any formal needs instantiating then we need to instantiate all
      // the VarArgs
      instantiateVarArgs = instantiateVarArgs || addSub;

      // If the formal wasn't instantiated then use whatever type was computed.
      if (!addSub) useType = formalType;

      QualifiedType::Kind qtKind = formalType.kind();
      auto tempQT = QualifiedType(qtKind, useType.type());
      auto newKind = resolveIntent(tempQT, /*isThis*/ false, /*isInit*/false);

      auto param = formalType.isParam() ? useType.param() : nullptr;
      useType = QualifiedType(newKind, useType.type(), param);

      varargsTypes.push_back(useType);

      // Grab the index and formal when first encountering a VarArgFormal.
      // Also increment the formalIdx once to stay aligned.
      if (varArgIdx < 0) {
        varArgIdx = formalIdx;
        formalIdx += 1;
      }
    } else {
      // add the substitution if we identified that we need to
      if (addSub) {
        // add it to the substitutions map
        substitutions.insert({entry.formal()->id(), useType});
        // Explicitly override the type in the resolver to make it available
        // to later fields without re-visiting and re-constructing the resolver.
        // TODO: is this too hacky?
        r.byAst(entry.formal()).setType(useType);
        // note that a substitution was used here
        if ((size_t) formalIdx >= formalsInstantiated.size()) {
          formalsInstantiated.resize(sig->numFormals());
        }
        formalsInstantiated.setBit(formalIdx, true);
      }

      formalIdx++;
    }

    // At this point, we have computed the instantiated type for this
    // formal. However, what we're still missing some information,
    // and furthermore, we have not enforced type query constraints.

    if (entry.isVarArgEntry()) {
      // Vararg entries don't get substitutions at this point, so
      // manually update type queries.
      if (auto vld = formal->toVarLikeDecl()) {
        if (auto te = vld->typeExpression()) {
            visitor.resolveTypeQueries(te, useType);
        }
      }
    } else {
      // Substitutions have been updated; re-run resolution to get better
      // intents, vararg info, and to extract type query info.
      formal->traverse(visitor);
      formalType = getProperFormalType(r, entry, ad, formal);
    }

    // Type queries have now been computed. We need to verify that type
    // query constraints are matched. To do this, instruct resolver to avoid
    // using substitutions, and to preserve previously computed type query
    // info. This way, we'll get as output the type expression's QualifiedType
    // which incorporates type query info.
    if (auto vld = formal->toVarLikeDecl()) {
      if (vld->typeExpression()) {
        visitor.ignoreSubstitutionFor = formal;
        visitor.skipTypeQueries = true;
      }
    }
    formal->traverse(visitor);
    auto qFormalType = getProperFormalType(r, entry, ad, formal);

    if (entry.isVarArgEntry()) {
      // We only need to canPass the tuple element types.
      qFormalType = getVarArgTupleElemType(qFormalType);
    } else {
      // Explicitly override the type in the resolver to what we have found it
      // to be before the type-query-aware traversal.
      r.byAst(entry.formal()).setType(formalType);
    }

    auto checkType = !useType.isUnknown() ? useType : formalType;
    // With the type and query-aware type known, make sure that they're compatible
    auto passResult = canPass(context, checkType, qFormalType);
    if (!passResult.passes()) {
      // Type query constraints were not satisfied
      return ApplicabilityResult::failure(sig, passResult.reason(), entry.formalIdx());
    }

    if (fn != nullptr && fn->isMethod() && fn->thisFormal() == formal) {
      visitor.setCompositeType(qFormalType.type()->toCompositeType());
    }
  }

  // instantiate the VarArg formal if necessary
  if (varargsTypes.size() > 0) {
    const TupleType* tup = sig->formalType(varArgIdx).type()->toTupleType();
    if (tup->isKnownSize() == false) {
      instantiateVarArgs = true;
    }

    if (instantiateVarArgs) {
      const TupleType* t = TupleType::getQualifiedTuple(context, varargsTypes);
      auto formal = faMap.byFormalIdx(varArgIdx).formal()->toVarArgFormal();
      QualifiedType vat = QualifiedType(formal->storageKind(), t);
      substitutions.insert({formal->id(), vat});
      r.byAst(formal).setType(vat);

      // note that a substitution was used here
      if ((size_t) varArgIdx >= formalsInstantiated.size()) {
        formalsInstantiated.resize(sig->numFormals());
      }
      formalsInstantiated.setBit(varArgIdx, true);
    }
  }

  // use the existing signature if there were no substitutions
  if (substitutions.size() == 0) {
    return ApplicabilityResult::success(sig);
  }

  std::vector<types::QualifiedType> formalTypes;
  bool needsInstantiation = false;
  TypedFnSignature::WhereClauseResult where = TypedFnSignature::WHERE_NONE;

  if (fn != nullptr) {
    for (auto formal : fn->formals()) {
      if (auto varArgFormal = formal->toVarArgFormal()) {
        if (!varArgCountMatch(varArgFormal, r)) {
          return ApplicabilityResult::failure(sig->id(), FAIL_VARARG_MISMATCH);
        }
      }
    }

    // visit the where clause
    if (auto whereClause = fn->whereClause()) {
      whereClause->traverse(visitor);
    }
    // do not visit the return type or function body

    auto tmp = visitor.getFormalTypes(fn);
    formalTypes.swap(tmp);
    needsInstantiation = anyFormalNeedsInstantiation(context, formalTypes,
                                                     untypedSignature,
                                                     &substitutions);
    where = whereClauseResult(context, fn, r, needsInstantiation);
  } else if (ad != nullptr) {
    // TODO: compute the class type

    // visit the fields
    ResolutionResultByPostorderID r;
    auto visitor =
      Resolver::createForInstantiatedSignatureFields(context, ad, substitutions,
                                                     poiScope, r);
    // visit the parent type
    if (auto cls = ad->toClass()) {
      for (int i = 0; i < cls->numInheritExprs(); i++) {
        cls->inheritExpr(i)->traverse(visitor);
      }
    }

    // visit the field declarations
    for (auto child: ad->children()) {
      if (child->isVariable() ||
          child->isMultiDecl() ||
          child->isTupleDecl() ||
          child->isForwardingDecl()) {
        child->traverse(visitor);
      }
    }

    // add formals according to the parent class type

    // now pull out the field types
    int nFormals = sig->numFormals();
    for (int i = 0; i < nFormals; i++) {
      const Decl* fieldDecl = untypedSignature->formalDecl(i);
      const ResolvedExpression& e = r.byAst(fieldDecl);
      QualifiedType fieldType = e.type();
      QualifiedType sigType = sig->formalType(i);

      // use the same kind as the old formal type but update the type, param
      // to reflect how instantiation occurred.
      formalTypes.push_back(QualifiedType(sigType.kind(),
                                          fieldType.type(),
                                          fieldType.param()));
    }
    needsInstantiation = anyFormalNeedsInstantiation(context, formalTypes,
                                                     untypedSignature,
                                                     &substitutions);
  } else {
    CHPL_ASSERT(false && "case not handled");
  }

  // now, construct a TypedFnSignature from the result
  auto result = TypedFnSignature::get(context,
                                      untypedSignature,
                                      std::move(formalTypes),
                                      where,
                                      needsInstantiation,
                                      /* instantiatedFrom */ sig,
                                      /* parentFn */ parentFnTyped,
                                      std::move(formalsInstantiated));

  // May need to resolve the body at this point to compute final TFS.
  if (ensureBodyIsResolved(context, call, result)) {
    if (!result->untyped()->isCompilerGenerated()) {
      if (isTfsForInitializer(result)) {
        auto resolvedFn = resolveInitializer(context, result, poiScope);
        auto newTfs = resolvedFn->signature();
        CHPL_ASSERT(!newTfs->needsInstantiation());
        result = newTfs;
      } else {
        CHPL_ASSERT(false && "Not handled yet!");
        std::ignore = resolveFunction(context, result, poiScope);
      }
    }
  }

  return ApplicabilityResult::success(result);
}

static const owned<ResolvedFunction>&
resolveFunctionByPoisQuery(Context* context,
                           const TypedFnSignature* sig,
                           PoiCallIdFnIds poiFnIdsUsed,
                           PoiRecursiveCalls recursiveFnsUsed) {
  QUERY_BEGIN(resolveFunctionByPoisQuery,
              context, sig, poiFnIdsUsed, recursiveFnsUsed);

  owned<ResolvedFunction> result;
  // the actual value is set in resolveFunctionByInfoQuery after it is
  // computed because computing it generates the poiFnIdsUsed which is
  // part of the key for this query.
  CHPL_ASSERT(false && "should not be reached");

  return QUERY_END(result);
}

// TODO: remove this workaround now that the build uses
// -Wno-dangling-reference
static const owned<ResolvedFunction>&
resolveFunctionByPoisQueryWrapper(Context* context,
                                  const TypedFnSignature* sig,
                                  const PoiInfo& poiInfo) {
  auto poiFnIdsUsedCopy = poiInfo.poiFnIdsUsed();
  auto recursiveFnsUsedCopy = poiInfo.recursiveFnsUsed();

  return resolveFunctionByPoisQuery(context, sig,
                                    std::move(poiFnIdsUsedCopy),
                                    std::move(recursiveFnsUsedCopy));
}

static const ResolvedFunction* const&
resolveFunctionByInfoQuery(Context* context,
                           const TypedFnSignature* sig,
                           PoiInfo poiInfo) {
  QUERY_BEGIN(resolveFunctionByInfoQuery, context, sig, poiInfo);

  const UntypedFnSignature* untypedSignature = sig->untyped();
  const AstNode* ast = parsing::idToAst(context, untypedSignature->id());
  const Function* fn = ast->toFunction();

  const PoiScope* poiScope = poiInfo.poiScope();

  PoiInfo resolvedPoiInfo;

  // Note that in this case the AST for the function can be nullptr.
  if (isTfsForInitializer(sig)) {
    auto retType = QualifiedType(QualifiedType::VAR, VoidType::get(context));

    ResolutionResultByPostorderID resolutionById;
    auto visitor = Resolver::createForInitializer(context, fn, poiScope,
                                                  sig,
                                                  resolutionById);
    CHPL_ASSERT(visitor.initResolver.get());
    if (fn) {
      fn->body()->traverse(visitor);
      // then, set the return type
      visitor.returnType = retType;
      // then, resolve '=' and add any copy init/deinit calls as needed
      callInitDeinit(visitor);
      // then, handle return intent overloads and maybe-const formals
      adjustReturnIntentOverloadsAndMaybeConstRefs(visitor);
    }

    auto newTfsForInitializer = visitor.initResolver->finalize();

    // TODO: can this be encapsulated in a method?
    resolvedPoiInfo.swap(visitor.poiInfo);
    resolvedPoiInfo.setResolved(true);
    resolvedPoiInfo.setPoiScope(nullptr);

    // If we resolved an initializer, then we started with a function
    // signature that might have needed instantiation for the receiver.
    // We need to communicate to the query framework that the new TFS
    // does not need to have its corresponding function resolved.
    if (newTfsForInitializer != sig) {
      auto resolutionByIdCopy = resolutionById;
      auto resolvedInit = toOwned(new ResolvedFunction(newTfsForInitializer,
                                  fn->returnIntent(),
                                  std::move(resolutionByIdCopy),
                                  resolvedPoiInfo,
                                  visitor.returnType));
      QUERY_STORE_RESULT(resolveFunctionByPoisQuery,
                         context,
                         resolvedInit,
                         newTfsForInitializer,
                         resolvedPoiInfo.poiFnIdsUsed(),
                         resolvedPoiInfo.recursiveFnsUsed());
      auto& saved =
        resolveFunctionByPoisQueryWrapper(context, newTfsForInitializer,
                                          resolvedPoiInfo);
      const ResolvedFunction* resultInit = saved.get();
      QUERY_STORE_RESULT(resolveFunctionByInfoQuery,
                         context,
                         resultInit,
                         newTfsForInitializer,
                         poiInfo);
    }

    // If we resolved an initializer, the result should point to the
    // final, fully instantiated TFS that was created (if there is
    // one). In other cases, we just use the input signature.
    auto finalTfs = newTfsForInitializer ? newTfsForInitializer : sig;

    owned<ResolvedFunction> resolved
        = toOwned(new ResolvedFunction(finalTfs, fn->returnIntent(),
                  std::move(resolutionById),
                  resolvedPoiInfo,
                  visitor.returnType));

    // Store the result in the query under the POIs used.
    // If there was already a value for this revision, this
    // call will not update it. (If it did, that could lead to
    // memory errors).
    QUERY_STORE_RESULT(resolveFunctionByPoisQuery,
                       context,
                       resolved,
                       sig,
                       resolvedPoiInfo.poiFnIdsUsed(),
                       resolvedPoiInfo.recursiveFnsUsed());

  // On this path we are just resolving a normal function.
  } else if (fn) {
    ResolutionResultByPostorderID resolutionById;
    auto visitor = Resolver::createForFunction(context, fn, poiScope, sig,
                                               resolutionById);

    if (fn->body()) {
      fn->body()->traverse(visitor);
    }

    // then, compute the return type
    computeReturnType(visitor);

    // then, resolve '=' and add any copy init/deinit calls as needed
    callInitDeinit(visitor);

    // then, handle return intent overloads and maybe-const formals
    adjustReturnIntentOverloadsAndMaybeConstRefs(visitor);

    // check that throws are handled or forwarded
    checkThrows(context, resolutionById, fn);

    // TODO: can this be encapsulated in a method?
    resolvedPoiInfo.swap(visitor.poiInfo);
    resolvedPoiInfo.setResolved(true);
    resolvedPoiInfo.setPoiScope(nullptr);

    owned<ResolvedFunction> resolved
        = toOwned(new ResolvedFunction(sig, fn->returnIntent(),
                  std::move(resolutionById),
                  resolvedPoiInfo,
                  visitor.returnType));

    // Store the result in the query under the POIs used.
    // If there was already a value for this revision, this
    // call will not update it. (If it did, that could lead to
    // memory errors).
    QUERY_STORE_RESULT(resolveFunctionByPoisQuery,
                       context,
                       resolved,
                       sig,
                       resolvedPoiInfo.poiFnIdsUsed(),
                       resolvedPoiInfo.recursiveFnsUsed());

  } else {
    CHPL_ASSERT(false && "this query should be called on Functions");
  }

  // Return the unique result from the query (that might have been saved above)
  const owned<ResolvedFunction>& resolved =
    resolveFunctionByPoisQueryWrapper(context, sig, resolvedPoiInfo);

  const ResolvedFunction* result = resolved.get();

  return QUERY_END(result);
}

const ResolvedFunction* resolveInitializer(Context* context,
                                           const TypedFnSignature* sig,
                                           const PoiScope* poiScope) {
  bool isAcceptable = isTfsForInitializer(sig);
  if (!isAcceptable) {
    CHPL_ASSERT(false && "Should only be called for initializers");
  }

  // construct the PoiInfo for this case
  auto poiInfo = PoiInfo(poiScope);

  // lookup in the map using this PoiInfo
  return resolveFunctionByInfoQuery(context, sig, std::move(poiInfo));
}

static const ResolvedFunction* helpResolveFunction(Context* context,
                                                   const TypedFnSignature* sig,
                                                   const PoiScope* poiScope,
                                                   bool skipIfRunning) {

  // Forget about any inferred signature (to avoid resolving the
  // same function twice when working with inferred 'out' formals)
  sig = sig->inferredFrom();

  // this should only be applied to concrete fns or instantiations
  CHPL_ASSERT(!sig->needsInstantiation());

  // construct the PoiInfo for this case
  auto poiInfo = PoiInfo(poiScope);

  if (skipIfRunning) {
    if (context->isQueryRunning(resolveFunctionByInfoQuery,
                                std::make_tuple(sig, poiInfo))) {
      return nullptr;
    }
  }

  // lookup in the map using this PoiInfo
  return resolveFunctionByInfoQuery(context, sig, std::move(poiInfo));
}

const TypedFnSignature* inferRefMaybeConstFormals(Context* context,
                                                  const TypedFnSignature* sig,
                                                  const PoiScope* poiScope) {
  if (sig == nullptr) {
    return nullptr;
  }

  bool anyRefMaybeConstFormals = false;
  int numFormals = sig->numFormals();
  for (int i = 0; i < numFormals; i++) {
    const types::QualifiedType& ft = sig->formalType(i);
    if (ft.kind() == QualifiedType::REF_MAYBE_CONST) {
      anyRefMaybeConstFormals = true;
      break;
    }
  }

  if (anyRefMaybeConstFormals == false) {
    // nothing else to do here
    return sig;
  }

  // otherwise, try to resolve the body of the function
  const ResolvedFunction* rFn =
    helpResolveFunction(context, sig, poiScope, /* skipIfRunning */ true);

  if (rFn == nullptr)
    return nullptr; // give up if it would be a recursive query invocation

  // resolve the function body
  const UntypedFnSignature* untyped = sig->untyped();
  const ResolutionResultByPostorderID& rr = rFn->resolutionById();
  std::vector<types::QualifiedType> formalTypes;
  for (int i = 0; i < numFormals; i++) {
    const types::QualifiedType& ft = sig->formalType(i);
    if (ft.kind() == QualifiedType::REF_MAYBE_CONST) {
      formalTypes.push_back(rr.byAst(untyped->formalDecl(i)).type());
    } else {
      formalTypes.push_back(ft);
    }
  }

  const TypedFnSignature* result = nullptr;
  result = TypedFnSignature::getInferred(context,
                                         std::move(formalTypes),
                                         sig);
  return result;
}

const ResolvedFunction* resolveFunction(Context* context,
                                        const TypedFnSignature* sig,
                                        const PoiScope* poiScope) {
  return helpResolveFunction(context, sig, poiScope, /* skipIfRunning */ false);
}

const ResolvedFunction* resolveConcreteFunction(Context* context, ID id) {
  if (id.isEmpty())
    return nullptr;

  const UntypedFnSignature* uSig = UntypedFnSignature::get(context, id);
  const TypedFnSignature* sig = typedSignatureInitial(context, uSig);
  if (sig->needsInstantiation()) {
    return nullptr;
  }

  auto whereFalse =
    resolution::TypedFnSignature::WhereClauseResult::WHERE_FALSE;
  if (sig->whereClauseResult() == whereFalse) {
    return nullptr;
  }

  const ResolvedFunction* ret = resolveFunction(context, sig, nullptr);
  return ret;
}

static const owned<ResolvedFunction>&
scopeResolveFunctionQuery(Context* context, ID id) {
  QUERY_BEGIN(scopeResolveFunctionQuery, context, id);

  const AstNode* ast = parsing::idToAst(context, id);
  const Function* fn = ast->toFunction();

  ResolutionResultByPostorderID resolutionById;
  const TypedFnSignature* sig = nullptr;
  owned<ResolvedFunction> result;

  if (fn) {
    auto visitor =
      Resolver::createForScopeResolvingFunction(context, fn, resolutionById);

    // visit the children of fn to scope resolve
    // (visiting the children because visiting a function will not
    //  cause it to be scope resolved).
    for (auto child: fn->children()) {
      child->traverse(visitor);

      // Recompute the method receiver after the 'this' formal is
      // scope-resolved, when we might be able to gather some information
      // about the type on which the method is declared.
      if (fn->isMethod() && child == fn->thisFormal()) {
        visitor.methodReceiverScopes(/*recompute=*/true);
      }
    }

    checkForParenlessMethodFieldRedefinition(context, fn, visitor);

    sig = visitor.typedSignature;
  }

  result = toOwned(new ResolvedFunction(sig, fn->returnIntent(),
                                        std::move(resolutionById),
                                        PoiInfo(),
                                        QualifiedType()));

  return QUERY_END(result);
}

const ResolvedFunction* scopeResolveFunction(Context* context,
                                                     ID id) {
  if (id.isEmpty())
    return nullptr;

  const owned<ResolvedFunction>& result =
    scopeResolveFunctionQuery(context, id);

  return result.get();
}

const ResolutionResultByPostorderID& scopeResolveAggregate(Context* context,
                                                           ID id) {
  QUERY_BEGIN(scopeResolveAggregate, context, id);

  const AggregateDecl* ad = parsing::idToAst(context, id)->toAggregateDecl();
  ResolutionResultByPostorderID result;

  if (ad) {
    // TODO: Use some kind of "ad->fields()" iterator
    for (auto child : ad->children()) {
      if (child->isVarLikeDecl() ||
          child->isMultiDecl() ||
          child->isTupleDecl() ||
          child->isForwardingDecl()) {
        auto res = Resolver::createForScopeResolvingField(context, ad, child, result);
        child->traverse(res);
      }
    }
  }

  return QUERY_END(result);
}


const ResolvedFunction* resolveOnlyCandidate(Context* context,
                                             const ResolvedExpression& r) {
  auto msc = r.mostSpecific().only();
  if (!msc) return nullptr;

  const TypedFnSignature* sig = msc.fn();
  const PoiScope* poiScope = r.poiScope();

  return resolveFunction(context, sig, poiScope);
}


static bool
isUntypedSignatureApplicable(Context* context,
                             const UntypedFnSignature* ufs,
                             const FormalActualMap& faMap,
                             const CallInfo& ci) {
  // First, check that the untyped properties allow a match:
  //  * number of arguments
  //  * names of arguments
  //  * method-ness
  //  * ref-ness

  if (!faMap.isValid()) {
    return false;
  }

  // TODO: more to check for method-ness?
  if (!ci.isOpCall() && ci.isMethodCall() != ufs->isMethod()) {
    return false;
  }

  // TODO: reason failed

  return true;
}

// given a typed function signature, determine if it applies to a call
static ApplicabilityResult
isInitialTypedSignatureApplicable(Context* context,
                                  const TypedFnSignature* tfs,
                                  const FormalActualMap& faMap,
                                  const CallInfo& ci) {
  if (!isUntypedSignatureApplicable(context, tfs->untyped(), faMap, ci)) {
    return ApplicabilityResult::failure(tfs->id(), /* TODO */ FAIL_CANDIDATE_OTHER);
  }

  // Next, check that the types are compatible
  int numVarArgActuals = 0;
  QualifiedType varArgType;
  for (const FormalActual& entry : faMap.byFormals()) {
    const auto& actualType = entry.actualType();

    // note: entry.actualType can have type()==nullptr and UNKNOWN.
    // in that case, resolver code should treat it as a hint to
    // use the default value. Unless the call used a ? argument.
    //
    // TODO: set a flag in the entry rather than relying on some encoded
    // property via QualifiedType.
    if (actualType.kind() == QualifiedType::UNKNOWN &&
        actualType.type() == nullptr &&
        !ci.hasQuestionArg()) {
      // use the default value - no need to check it matches formal
    } else {
      const auto& formalType = tfs->formalType(entry.formalIdx());
      CanPassResult got;
      if (entry.isVarArgEntry()) {
        if (varArgType.isUnknown()) {
          varArgType = formalType;
        }
        numVarArgActuals += 1;

        got = canPass(context, actualType, getVarArgTupleElemType(formalType));
      } else {
        got = canPass(context, actualType, formalType);
      }
      if (!got.passes()) {
        return ApplicabilityResult::failure(tfs, got.reason(), entry.formalIdx());
      }
    }
  }

  if (!varArgType.isUnknown()) {
    const TupleType* tup = varArgType.type()->toTupleType();
    if (tup != nullptr && tup->isVarArgTuple() &&
        tup->isKnownSize() && numVarArgActuals != tup->numElements()) {
      return ApplicabilityResult::failure(tfs->id(), FAIL_VARARG_MISMATCH);
    }
  }

  // check that the where clause applies
  auto whereResult = tfs->whereClauseResult();
  if (whereResult == TypedFnSignature::WHERE_FALSE) {
    return ApplicabilityResult::failure(tfs->id(), FAIL_WHERE_CLAUSE);
  }

  return ApplicabilityResult::success(tfs);
}

// returns nullptr if the candidate is not applicable,
// or the result of typedSignatureInitial if it is.
static ApplicabilityResult
doIsCandidateApplicableInitial(Context* context,
                               const ID& candidateId,
                               const CallInfo& ci) {
  AstTag tag = asttags::AST_TAG_UNKNOWN;

  if (!candidateId.isEmpty()) {
    tag = parsing::idToTag(context, candidateId);
  }

  // if it's a paren-less call, only consider parenless routines
  // (including generated field accessors) but not types/outer variables/
  // calls with parens.
  if (ci.isParenless()) {
    if (parsing::idIsParenlessFunction(context, candidateId) ||
        parsing::idIsField(context, candidateId)) {
      // OK
    } else {
      return ApplicabilityResult::failure(candidateId, FAIL_PARENLESS_MISMATCH);
    }
  }

  if (isTypeDecl(tag)) {
    // calling a type - i.e. type construction
    const Type* t = initialTypeForTypeDecl(context, candidateId);
    return ApplicabilityResult::success(typeConstructorInitial(context, t));
  }

  // not a candidate
  if (ci.isMethodCall() && isFormal(tag)) {
    return ApplicabilityResult::failure(candidateId, /* TODO */ FAIL_CANDIDATE_OTHER);
  }

  if (isVariable(tag)) {
    if (ci.isParenless() && ci.isMethodCall() && ci.numActuals() == 1) {
      // calling a field accessor
      auto ct = ci.actual(0).type().type()->getCompositeType();
      CHPL_ASSERT(ct);
      auto containingType = isNameOfField(context, ci.name(), ct);
      CHPL_ASSERT(containingType != nullptr);
      return ApplicabilityResult::success(fieldAccessor(context, containingType, ci.name()));
    } else {
      // not a candidate
      return ApplicabilityResult::failure(candidateId, /* TODO */ FAIL_CANDIDATE_OTHER);
    }
  }

  CHPL_ASSERT(isFunction(tag) && "expected fn case only by this point");

  if (ci.isMethodCall() && (ci.name() == "init" || ci.name() == "init=")) {
    // TODO: test when record has defaults for type/param fields
    auto recv = ci.calledType();
    auto fn = parsing::idToAst(context, candidateId)->toFunction();
    ResolutionResultByPostorderID r;
    auto vis = Resolver::createForInitialSignature(context, fn, r);
    fn->thisFormal()->traverse(vis);
    auto res = vis.byPostorder.byAst(fn->thisFormal());

    auto got = canPass(context, recv, res.type());
    if (!got.passes()) {
      return ApplicabilityResult::failure(candidateId, /* TODO */ FAIL_CANDIDATE_OTHER);
    }
  }

  auto ufs = UntypedFnSignature::get(context, candidateId);
  auto faMap = FormalActualMap(ufs, ci);
  auto ret = typedSignatureInitial(context, ufs);

  return isInitialTypedSignatureApplicable(context, ret, faMap, ci);
}

// returns nullptr if the candidate is not applicable,
// or the result of an instantiated typedSignature if it is.
static ApplicabilityResult
doIsCandidateApplicableInstantiating(Context* context,
                                     const TypedFnSignature* typedSignature,
                                     const CallInfo& call,
                                     const PoiScope* poiScope) {

  auto instantiated =
    instantiateSignature(context, typedSignature, call, poiScope);

  if (!instantiated.success())
    return instantiated;

  // check that the where clause applies
  if (instantiated.candidate()->whereClauseResult() == TypedFnSignature::WHERE_FALSE)
    return ApplicabilityResult::failure(typedSignature->id(), FAIL_WHERE_CLAUSE);

  return instantiated;
}

static ApplicabilityResult const&
isCandidateApplicableInitialQuery(Context* context,
                                  ID candidateId,
                                  CallInfo call) {

  QUERY_BEGIN(isCandidateApplicableInitialQuery, context, candidateId, call);

  auto result =
    doIsCandidateApplicableInitial(context, candidateId, call);

  return QUERY_END(result);
}

static const std::pair<std::vector<const TypedFnSignature*>,
                       std::vector<ApplicabilityResult>>&
filterCandidatesInitialGatherRejected(Context* context,
                                      std::vector<BorrowedIdsWithName> lst,
                                      CallInfo call,
                                      bool gatherRejected) {
  QUERY_BEGIN(filterCandidatesInitialGatherRejected, context, lst, call, gatherRejected);

  std::vector<const TypedFnSignature*> matching;
  std::vector<ApplicabilityResult> rejected;

  for (const BorrowedIdsWithName& ids : lst) {
    for (const ID& id : ids) {
      auto s = isCandidateApplicableInitialQuery(context, id, call);
      if (s.success()) {
        matching.push_back(s.candidate());
      } else if (gatherRejected) {
        rejected.push_back(s);
      }
    }
  }

  auto result = std::make_pair(std::move(matching), std::move(rejected));
  return QUERY_END(result);
}

const std::vector<const TypedFnSignature*>&
filterCandidatesInitial(Context* context,
                        std::vector<BorrowedIdsWithName> lst,
                        CallInfo call) {
  auto& result = filterCandidatesInitialGatherRejected(context, std::move(lst),
                                                       call, /* gatherRejected */ false);
  return result.first;
}

void
filterCandidatesInstantiating(Context* context,
                              const std::vector<const TypedFnSignature*>& lst,
                              const CallInfo& call,
                              const Scope* inScope,
                              const PoiScope* inPoiScope,
                              std::vector<const TypedFnSignature*>& result,
                              std::vector<ApplicabilityResult>* rejected) {

  // Performance: Would it help to make this a query?
  // (I left it not as a query since it runs some other queries
  //  and seems like it might have limited ability for reuse).
  const PoiScope* instantiationPoiScope = nullptr;

  for (const TypedFnSignature* typedSignature : lst) {
    if (typedSignature->needsInstantiation()) {
      if (instantiationPoiScope == nullptr) {
        instantiationPoiScope =
          pointOfInstantiationScope(context, inScope, inPoiScope);
      }

      auto instantiated =
        doIsCandidateApplicableInstantiating(context,
                                             typedSignature,
                                             call,
                                             instantiationPoiScope);
      if (instantiated.success()) {
        result.push_back(instantiated.candidate());
      } if (rejected) {
        rejected->push_back(std::move(instantiated));
      }
    } else {
      // if it's already concrete, we already know it is a candidate.
      result.push_back(typedSignature);
    }
  }
}

static
void accumulatePoisUsedByResolvingBody(Context* context,
                                       const TypedFnSignature* signature,
                                       const PoiScope* poiScope,
                                       PoiInfo& poiInfo) {

  if (signature == nullptr) {
    return;
  }

  if (signature->instantiatedFrom() == nullptr) {
    // if it's not an instantiation, no need to gather POIs
    return;
  }

  if (signature->needsInstantiation()) {
    // if it needs instantiation, it's not time to gather POIs yet
    return;
  }

  // resolve the body, if it is not already being resolved
  const ResolvedFunction* r = helpResolveFunction(context, signature, poiScope,
                                                  /* skipIfRunning */ true);
  if (r == nullptr) {
    // If it's a recursive call, track it in the PoiInfo
    poiInfo.accumulateRecursive(signature, poiScope);
  } else {
    // gather the POI scopes from instantiating the function body
    poiInfo.accumulate(r->poiInfo());
  }
}

// if the call's name matches a class management type construction,
// return the result or ErroneousType.
// returns nullptr if the class type is not handled here.
static const Type* getManagedClassType(Context* context,
                                       const AstNode* astForErr,
                                       const CallInfo& ci) {
  UniqueString name = ci.name();

  if (ci.hasQuestionArg()) {
    if (ci.numActuals() != 0) {
      context->error(astForErr, "invalid class type construction");
      return ErroneousType::get(context);
    } else if (name == USTR("owned")) {
      return AnyOwnedType::get(context);
    } else if (name == USTR("shared")) {
      return AnySharedType::get(context);
    } else if (name == USTR("unmanaged")) {
      return ClassType::get(context, AnyClassType::get(context), nullptr, ClassTypeDecorator(ClassTypeDecorator::UNMANAGED));
    } else if (name == USTR("borrowed")) {
      return ClassType::get(context, AnyClassType::get(context), nullptr, ClassTypeDecorator(ClassTypeDecorator::BORROWED));
    } else {
      // case not handled in here
      return nullptr;
    }
  }

  ClassTypeDecorator::ClassTypeDecoratorEnum de;
  const Type* manager = nullptr;

  if (name == USTR("owned")) {
    de = ClassTypeDecorator::MANAGED;
    manager = AnyOwnedType::get(context);
  } else if (name == USTR("shared")) {
    de = ClassTypeDecorator::MANAGED;
    manager = AnySharedType::get(context);
  } else if (name == USTR("unmanaged")) {
    de = ClassTypeDecorator::UNMANAGED;
    manager = nullptr;
  } else if (name == USTR("borrowed")) {
    de = ClassTypeDecorator::BORROWED;
    manager = nullptr;
  } else {
    // case not handled in here
    return nullptr;
  }

  auto d = ClassTypeDecorator(de);

  const Type* t = nullptr;
  if (ci.numActuals() > 0)
    t = ci.actual(0).type().type();

  if (t == nullptr || !(t->isManageableType() || t->isClassType())) {
    if (t != nullptr && !t->isUnknownType()) {
      context->error(astForErr, "invalid class type construction");
    }
    return ErroneousType::get(context);
  }

  const ManageableType* mt = nullptr;
  if (auto ct = t->toClassType()) {
    mt = ct->manageableType();
    // get nilability from ct
    if (ct->decorator().isNilable())
      d = d.addNilable();
    if (ct->decorator().isNonNilable())
      d = d.addNonNil();
  } else {
    mt = t->toManageableType();
  }

  CHPL_ASSERT(mt);
  return ClassType::get(context, mt, manager, d);
}

static const Type* getNumericType(Context* context,
                                  const AstNode* astForErr,
                                  const CallInfo& ci) {
  UniqueString name = ci.name();

  if (name == USTR("int") || name == USTR("uint") || name == USTR("bool") ||
      name == USTR("real") || name == USTR("imag") || name == USTR("complex")) {

    // Should we compute the generic version of the type (e.g. int(?))
    bool useGenericType = false;

    // There should be 0 or 1 actuals depending on if it is ?
    if (ci.hasQuestionArg()) {
      // handle int(?)
      if (ci.numActuals() != 0) {
        context->error(astForErr, "invalid numeric type construction");
        return ErroneousType::get(context);
      }
      useGenericType = true;
    } else {
      // handle int(?t) or int(16)
      if (ci.numActuals() != 1) {
        context->error(astForErr, "invalid numeric type construction");
        return ErroneousType::get(context);
      }

      QualifiedType qt = ci.actual(0).type();
      if (qt.type() && qt.type()->isAnyType()) {
        useGenericType = true;
      } else if (qt.isParam() && qt.param() == nullptr)  {
        useGenericType = true;
      }
    }

    if (useGenericType) {
      if (name == USTR("int")) {
        return AnyIntType::get(context);
      } else if (name == USTR("uint")) {
        return AnyUintType::get(context);
      } else if (name == USTR("real")) {
        return AnyRealType::get(context);
      } else if (name == USTR("imag")) {
        return AnyImagType::get(context);
      } else if (name == USTR("complex")) {
        return AnyComplexType::get(context);
      } else {
        CHPL_ASSERT(false && "should not be reachable");
        return nullptr;
      }
    }

    QualifiedType qt;
    if (ci.numActuals() > 0)
      qt = ci.actual(0).type();

    const Type* t = qt.type();
    if (t == nullptr) {
      // Details not yet known so return UnknownType
      return UnknownType::get(context);
    }
    if (t->isUnknownType() || t->isErroneousType()) {
      // Just propagate the Unknown / Erroneous type
      // without raising any errors
      return t;
    }
    if (qt.param() == nullptr) {
      // Details not yet known so return UnknownType
      return UnknownType::get(context);
    }

    if (!t->isIntType() || !qt.param()->isIntParam()) {
      // raise an error b/c of type mismatch
      context->error(astForErr, "invalid numeric type construction");
      return ErroneousType::get(context);
    }

    const Type* ret = nullptr;
    auto ip = qt.param()->toIntParam();
    auto value = ip->value();
    if (0 <= value && value <= 128) {
      ret = PrimitiveType::getWithNameAndWidth(context, name, value);
    }

    if (ret == nullptr) {
      context->error(astForErr, "invalid numeric type construction");
      return ErroneousType::get(context);
    }

    return ret;
  }

  return nullptr;
}

static const Type* getCPtrType(Context* context,
                               const AstNode* astForErr,
                               const CallInfo& ci) {
  UniqueString name = ci.name();

  if (name == USTR("c_ptr")) {
    // Should we compute the generic version of the type (e.g. c_ptr(?))
    bool useGenericType = false;

    // There should be 0 or 1 actuals depending on if it is ?
    if (ci.hasQuestionArg()) {
      // handle c_ptr(?)
      if (ci.numActuals() != 0) {
        context->error(astForErr, "invalid c_ptr type construction");
        return ErroneousType::get(context);
      }
      useGenericType = true;
    } else {
      // handle c_ptr(?t) or c_ptr(eltT)
      if (ci.numActuals() != 1) {
        context->error(astForErr, "invalid c_ptr type construction");
        return ErroneousType::get(context);
      }

      QualifiedType qt = ci.actual(0).type();
      if (qt.type() && qt.type()->isAnyType()) {
        useGenericType = true;
      }
    }

    if (useGenericType) {
      return CPtrType::get(context);
    }

    QualifiedType qt;
    if (ci.numActuals() > 0)
      qt = ci.actual(0).type();

    const Type* t = qt.type();
    if (t == nullptr) {
      // Details not yet known so return UnknownType
      return UnknownType::get(context);
    }
    if (t->isUnknownType() || t->isErroneousType()) {
      // Just propagate the Unknown / Erroneous type
      // without raising any errors
      return t;
    }

    if (!qt.isType()) {
      // raise an error b/c of type mismatch
      context->error(astForErr, "invalid c_ptr type construction");
      return ErroneousType::get(context);
    }

    return CPtrType::get(context, qt.type());
  }

  return nullptr;
}

static const Type*
convertClassTypeToNilable(Context* context, const Type* t) {
  const ClassType* ct = nullptr;

  if (auto bct = t->toManageableType()) {
    auto d = ClassTypeDecorator(ClassTypeDecorator::GENERIC_NONNIL);
    ct = ClassType::get(context, bct, nullptr, d);
  } else {
    ct = t->toClassType();
  }

  if (ct) {
    // get the nilable version of the class type
    ClassTypeDecorator d = ct->decorator().addNilable();
    auto ret= ct->withDecorator(context, d);
    return ret;
  }

  return nullptr;
}

// Resolving compiler-supported type-returning patterns
// 'call' and 'inPoiScope' are used for the location for error reporting.
static const Type* resolveBuiltinTypeCtor(Context* context,
                                          const AstNode* astForErr,
                                          const CallInfo& ci) {
  // none of the special type function calls are methods; we can stop here.
  if (ci.isMethodCall()) return nullptr;

  if (ci.name() == USTR("?")) {
    if (ci.numActuals() > 0) {
      if (const Type* t = ci.actual(0).type().type()) {
        if (auto nilable = convertClassTypeToNilable(context, t)) {
          return nilable;
        }
      }
    }
  }

  if (ci.name() == USTR("*") && ci.numActuals() == 2) {
    auto first = ci.actual(0).type();
    auto second = ci.actual(1).type();
    if (first.isParam() && first.type()->isIntType() &&
        second.isType()) {
      return TupleType::getStarTuple(context, first, second);
    }
  }

  if (auto t = getManagedClassType(context, astForErr, ci)) {
    return t;
  }

  if (auto t = getNumericType(context, astForErr, ci)) {
    return t;
  }

  if (auto t = getCPtrType(context, astForErr, ci)) {
    return t;
  }

  return nullptr;
}

static
bool resolvePostfixNilableAppliedToNew(Context* context, const Call* call,
                                       const CallInfo& ci,
                                       QualifiedType& exprTypeOut) {

  // First, pattern match to find something like 'new C()?'...
  if (!call || !call->isOpCall()) return false;

  auto opCall = call->toOpCall();
  if (opCall->op() != USTR("?") || opCall->numActuals() != 1) return false;

  auto newCall = opCall->actual(0)->toFnCall();
  if (!newCall || !newCall->calledExpression() ||
      !newCall->calledExpression()->isNew()) {
    return false;
  }

  // Now, adjust the type to be nilable, but not the kind.
  auto qtNewCall = ci.actual(0).type();

  if (qtNewCall.isUnknown() || qtNewCall.isErroneousType()) {
    exprTypeOut = qtNewCall;
  }

  auto convToNilable = convertClassTypeToNilable(context, qtNewCall.type());
  auto outType = convToNilable ? convToNilable : qtNewCall.type();

  exprTypeOut = QualifiedType(qtNewCall.kind(), outType);

  return true;
}

// Resolving calls for certain compiler-supported patterns
// without requiring module implementations exist at all.
static bool resolveFnCallSpecial(Context* context,
                                 const AstNode* astForErr,
                                 const CallInfo& ci,
                                 QualifiedType& exprTypeOut) {
  // TODO: cast
  // TODO: .borrow()
  // TODO: chpl__coerceCopy

  if ((ci.name() == USTR("==") || ci.name() == USTR("!=")) &&
      ci.numActuals() == 2) {
    auto lhs = ci.actual(0).type();
    auto rhs = ci.actual(1).type();

    bool bothType = lhs.kind() == QualifiedType::TYPE &&
                    rhs.kind() == QualifiedType::TYPE;
    bool bothParam = lhs.kind() == QualifiedType::PARAM &&
                     rhs.kind() == QualifiedType::PARAM;
    if (bothType || bothParam) {
      bool result = lhs == rhs;
      result = ci.name() == USTR("==") ? result : !result;
      exprTypeOut = QualifiedType(QualifiedType::PARAM, BoolType::get(context),
                                  BoolParam::get(context, result));
      return true;
    }
  }

  if (ci.isOpCall() && ci.name() == USTR("!") && ci.numActuals() == 1) {
    auto qt = ci.actual(0).type();
    if (qt.kind() == QualifiedType::PARAM && qt.hasParamPtr() &&
        qt.hasTypePtr() && qt.type()->isBoolType()) {
      exprTypeOut = qt.param()->fold(context,
                                     chpl::uast::PrimitiveTag::PRIM_UNARY_LNOT,
                                     qt, QualifiedType());
      return true;
    }
  }

  if (ci.name() == USTR("isCoercible")) {
    if (ci.numActuals() != 2) {
      context->error(astForErr, "bad call to %s", ci.name().c_str());
      exprTypeOut = QualifiedType(QualifiedType::UNKNOWN,
                                  ErroneousType::get(context));
      return true;
    }
    auto got = canPass(context, ci.actual(0).type(), ci.actual(1).type());
    bool result = got.passes();
    exprTypeOut = QualifiedType(QualifiedType::PARAM, BoolType::get(context),
                                BoolParam::get(context, result));
    return true;
  }

  return false;
}

static bool resolveFnCallSpecialType(Context* context,
                                     const Call* call,
                                     const CallInfo& ci,
                                     const Scope* inScope,
                                     const PoiScope* inPoiScope,
                                     CallResolutionResult& result) {
  if (ci.isMethodCall()) {
    return false;
  }

  // Types that can be computed without resolving other calls
  if (const Type* t = resolveBuiltinTypeCtor(context, call, ci)) {
    auto exprTypeOut = QualifiedType(QualifiedType::TYPE, t);
    result = CallResolutionResult(exprTypeOut);
    return true;
  }

  // Types that require resolving some kind of helper function to build
  // the type.
  //
  // TODO: sync, single
  if (ci.name() == "domain") {
    // TODO: a compiler-generated type constructor would be simpler, but we
    // don't support default values on compiler-generated methods because the
    // default values require existing AST.

    // Note: 'dmapped' is treated like a binary operator at the moment, so
    // we don't need to worry about distribution type for 'domain(...)' exprs.

    // Transform domain type expressions like `domain(arg1, ...)` into:
    //   _domain.static_type(arg1, ...)
    auto genericDom = DomainType::getGenericDomainType(context);
    auto recv = QualifiedType(QualifiedType::TYPE, genericDom);
    auto typeCtorName = UniqueString::get(context, "static_type");
    auto ctorCall = CallInfo::createWithReceiver(ci, recv, typeCtorName);

    result = resolveCall(context, call, ctorCall, inScope, inPoiScope);
    return true;
  } else if (ci.name() == "atomic") {
    auto newName = UniqueString::get(context, "chpl__atomicType");
    auto ctorCall = CallInfo::copyAndRename(ci, newName);
    result = resolveCall(context, call, ctorCall, inScope, inPoiScope);
    return true;
  }

  return false;
}

static void buildReaderWriterTypeCtor(Context* context,
                                      const CallInfo& ci,
                                      const TypedFnSignature* initial,
                                      CandidatesVec& initialCandidates) {
  std::vector<UntypedFnSignature::FormalDetail> formals;
  // Move 'kind' to the end and allow the first two args to just be
  // 'locking' and  '(de)serializerType'
  //
  // TODO: The '_serializerWrapper' arg should _not_ be considered
  // part of the type constructor...
  std::vector<int> order = {1, 2, 3, 0};
  for (auto i : order) {
    auto un = initial->untyped();
    auto d = UntypedFnSignature::FormalDetail(un->formalName(i),
                                              un->formalHasDefault(i),
                                              un->formalDecl(i),
                                              un->formalIsVarArgs(i));
    formals.push_back(d);
  }

  std::vector<types::QualifiedType> formalTypes;
  for (auto i : order) {
    formalTypes.push_back(initial->formalType(i));
  }

  auto untyped = UntypedFnSignature::get(context,
                                         initial->id(), ci.name(),
                                         /* isMethod */ false,
                                         /* isTypeConstructor */ true,
                                         /* isCompilerGenerated */ true,
                                         /* throws */ false,
                                         uast::asttags::Record,
                                         Function::PROC,
                                         std::move(formals),
                                         /* whereClause */ nullptr);

  auto result = TypedFnSignature::get(context,
                                      untyped,
                                      std::move(formalTypes),
                                      TypedFnSignature::WHERE_NONE,
                                      /* needsInstantiation */ true,
                                      /* instantiatedFrom */ nullptr,
                                      /* parentFn */ nullptr,
                                      /* formalsInstantiated */ Bitmap());

  initialCandidates.push_back(result);
}

static MostSpecificCandidates
resolveFnCallForTypeCtor(Context* context,
                         const CallInfo& ci,
                         const Scope* inScope,
                         const PoiScope* inPoiScope,
                         PoiInfo& poiInfo) {

  CandidatesVec initialCandidates;
  CandidatesVec candidates;

  CHPL_ASSERT(ci.calledType().type() != nullptr);
  CHPL_ASSERT(!ci.calledType().type()->isUnknownType());

  auto initial = typeConstructorInitial(context, ci.calledType().type());
  initialCandidates.push_back(initial);

  //
  // Adds an alternative type constructor for fileReader/Writer to support
  // the deprecated 'kind' field, as in PR #23007.
  //
  // TODO: Remove this code when the 'kind' field is finally removed.
  //
  if (auto rt = ci.calledType().type()->toRecordType()) {
    if (parsing::idIsInBundledModule(context, rt->id())) {
      if (ci.name() == "fileWriter" || ci.name() == "fileReader") {
        buildReaderWriterTypeCtor(context, ci, initial, initialCandidates);
      }
    }
  }

  // TODO: do something for partial instantiation

  filterCandidatesInstantiating(context,
                                initialCandidates,
                                ci,
                                inScope,
                                inPoiScope,
                                candidates,
                                /* rejected */ nullptr);


  ForwardingInfoVec forwardingInfo;

  // find most specific candidates / disambiguate
  // Note: at present there can only be one candidate here
  MostSpecificCandidates mostSpecific =
    findMostSpecificCandidates(context,
                               candidates, forwardingInfo,
                               ci, inScope, inPoiScope);

  return mostSpecific;
}

static void
considerCompilerGeneratedCandidates(Context* context,
                                   const CallInfo& ci,
                                   const Scope* inScope,
                                   const PoiScope* inPoiScope,
                                   CandidatesVec& candidates) {

  // only consider compiler-generated methods and opcalls, for now
  if (!ci.isMethodCall() && !ci.isOpCall()) return;

  // fetch the receiver type info
  CHPL_ASSERT(ci.numActuals() >= 1);
  auto& receiver = ci.actual(0);
  // TODO: This should be the QualifiedType in case of type methods
  auto receiverType = receiver.type().type();

  // if not compiler-generated, then nothing to do
  if (!needCompilerGeneratedMethod(context, receiverType, ci.name(),
                                   ci.isParenless())) {
    return;
  }

  // get the compiler-generated function, may be generic
  auto tfs = getCompilerGeneratedMethod(context, receiverType, ci.name(),
                                        ci.isParenless());
  CHPL_ASSERT(tfs);

  // check if the initial signature matches
  auto faMap = FormalActualMap(tfs->untyped(), ci);
  if (!isInitialTypedSignatureApplicable(context, tfs, faMap, ci).success()) {
    return;
  }

  // OK, already concrete, store and return
  if (!tfs->needsInstantiation()) {
    candidates.push_back(tfs);
    return;
  }

  // need to instantiate before storing
  auto poi = pointOfInstantiationScope(context, inScope, inPoiScope);
  auto instantiated = doIsCandidateApplicableInstantiating(context,
                                                           tfs,
                                                           ci,
                                                           poi);
  CHPL_ASSERT(instantiated.success());
  CHPL_ASSERT(instantiated.candidate()->untyped()->idIsFunction());
  CHPL_ASSERT(instantiated.candidate()->instantiatedFrom());

  candidates.push_back(instantiated.candidate());
}

static std::vector<BorrowedIdsWithName>
lookupCalledExpr(Context* context,
                 const Scope* scope,
                 const CallInfo& ci,
                 CheckedScopes& visited) {

  Resolver::ReceiverScopesVec receiverScopes;

  // For method calls, also consider the receiver scope.
  if (ci.isMethodCall() || ci.isOpCall()) {
    // TODO: should types of all arguments be considered for an op call?
    CHPL_ASSERT(ci.numActuals() >= 1);
    auto& qtReceiver = ci.actual(0).type();
    if (auto t = qtReceiver.type()) {
      if (auto compType = t->getCompositeType()) {
        receiverScopes =
          Resolver::gatherReceiverAndParentScopesForType(context, compType);
      }
    }
  }

  LookupConfig config = LOOKUP_DECLS | LOOKUP_IMPORT_AND_USE | LOOKUP_PARENTS;

  // For parenless non-method calls, only find the innermost match
  if (ci.isParenless() && !ci.isMethodCall()) {
    config |= LOOKUP_INNERMOST;
  }

  if (ci.isMethodCall()) {
    config |= LOOKUP_ONLY_METHODS_FIELDS;
  }

  if (ci.isOpCall()) {
    config |= LOOKUP_METHODS;
  }

  UniqueString name = ci.name();

  auto ret = lookupNameInScopeWithSet(context, scope, receiverScopes, name,
                                      config, visited);

  return ret;
}

static void helpComputeForwardingTo(const CallInfo& fci,
                                    size_t start,
                                    CandidatesVec& candidates,
                                    std::vector<QualifiedType>& forwardingTo) {
  QualifiedType forwardingReceiverActualType = fci.calledType();
  size_t n = candidates.size();
  forwardingTo.resize(start);
  for (size_t i = start; i < n; i++) {
    forwardingTo.push_back(forwardingReceiverActualType);
  }
}

// this function gathers candidates not from POI and candidates
// from POI into separate vectors.
// For each of these vectors, the corresponding forwardingTo vector
// will have an element for each of the returned candidates &
// indicates the actual type that is passed as the method receiver
// when using forwarding.
static void
gatherAndFilterCandidatesForwarding(Context* context,
                                    const Call* call,
                                    const CallInfo& ci,
                                    const Scope* inScope,
                                    const PoiScope* inPoiScope,
                                    CandidatesVec& nonPoiCandidates,
                                    CandidatesVec& poiCandidates,
                                    ForwardingInfoVec& nonPoiForwardingTo,
                                    ForwardingInfoVec& poiForwardingTo) {

  const Type* receiverType = ci.actual(0).type().type();

  // Resolve the forwarding expression's types & decide if we
  // want to consider forwarding.
  ResolvedFields forwards;
  UniqueString name = ci.name();
  if (name == USTR("init") || name == USTR("init=") || name == USTR("deinit")) {
    // these are exempt from forwarding
  } else if (auto ct = receiverType->getCompositeType()) {
    auto useDefaults = DefaultsPolicy::USE_DEFAULTS;
    const ResolvedFields& fields = fieldsForTypeDecl(context, ct,
                                                     useDefaults);
    const ResolvedFields& exprs = resolveForwardingExprs(context, ct);
    if (fields.numForwards() > 0 ||
        exprs.numForwards() > 0) {
      // and check for cycles
      bool cycleFound = emitErrorForForwardingCycles(context, ct);
      if (cycleFound == false) {
        forwards.addForwarding(fields);
        forwards.addForwarding(exprs);
      }
    }
  }

  if (forwards.numForwards() > 0) {
    // Construct CallInfos with the receiver replaced for each
    // of the forwarded-to types.
    std::vector<CallInfo> forwardingCis;

    int numForwards = forwards.numForwards();
    for (int i = 0; i < numForwards; i++) {
      QualifiedType forwardType = forwards.forwardingToType(i);

      // an error occurred, skip it
      if (forwardType.isUnknown() || forwardType.hasTypePtr() == false)
        continue;

      std::vector<CallInfoActual> actuals;
      // compute the actuals
      // first, the method receiver (from the forwarded type)
      actuals.push_back(CallInfoActual(forwardType, USTR("this")));

      // Check that ci was consistent with passing the receiver by name
      // If it turns out to not always be the case for a good reason,
      // this assert can be removed.
      CHPL_ASSERT(ci.actual(0).byName() == USTR("this"));

      // then, the other actuals, if any, from ci
      int numActuals = ci.numActuals();
      for (int j = 1; j < numActuals; j++) {
        actuals.push_back(ci.actual(j));
      }

      forwardingCis.push_back(CallInfo(ci.name(),
                                       forwardType,
                                       ci.isMethodCall(),
                                       ci.hasQuestionArg(),
                                       ci.isParenless(),
                                       std::move(actuals)));
    }

    // Now, consider each of the forwarding calls to gather and filter
    // candidates.
    // This should follow the same process as gatherAndFilterCandidates but:
    // * in each stage, consider all of the forwarded method receiver types
    //   equally as sources of candidates
    // * do not consider forwarding (since we are considering it now!)

    std::vector<CheckedScopes> visited;
    visited.resize(numForwards);

    for (const auto& fci : forwardingCis) {
      size_t start = nonPoiCandidates.size();
      // consider compiler-generated candidates
      considerCompilerGeneratedCandidates(context, fci, inScope, inPoiScope,
                                          nonPoiCandidates);
      // update forwardingTo
      helpComputeForwardingTo(fci, start, nonPoiCandidates, nonPoiForwardingTo);
    }

    // next, look for candidates without using POI.
    {
      int i = 0;
      for (const auto& fci : forwardingCis) {
        size_t start = nonPoiCandidates.size();
        // compute the potential functions that it could resolve to
        auto v = lookupCalledExpr(context, inScope, fci, visited[i]);

        // filter without instantiating yet
        const auto& initialCandidates =
          filterCandidatesInitial(context, std::move(v), fci);

        // find candidates, doing instantiation if necessary
        filterCandidatesInstantiating(context,
                                      initialCandidates,
                                      fci,
                                      inScope,
                                      inPoiScope,
                                      nonPoiCandidates,
                                      /* rejected */ nullptr);

        // update forwardingTo
        helpComputeForwardingTo(fci, start,
                                nonPoiCandidates, nonPoiForwardingTo);
        i++;
      }
    }

    // next, look for candidates using POI
    for (const PoiScope* curPoi = inPoiScope;
         curPoi != nullptr;
         curPoi = curPoi->inFnPoi()) {

      // stop if any candidate has been found.
      if (nonPoiCandidates.empty() == false || poiCandidates.empty() == false) {
        break;
      }


      int i = 0;
      for (const auto& fci : forwardingCis) {
        size_t start = poiCandidates.size();

        // compute the potential functions that it could resolve to
        auto v = lookupCalledExpr(context, curPoi->inScope(), fci, visited[i]);

        // filter without instantiating yet
        auto& initialCandidates =
          filterCandidatesInitial(context, std::move(v), fci);

        // find candidates, doing instantiation if necessary
        filterCandidatesInstantiating(context,
                                      initialCandidates,
                                      fci,
                                      inScope,
                                      inPoiScope,
                                      poiCandidates,
                                      /* rejected */ nullptr);

        // update forwardingTo
        helpComputeForwardingTo(fci, start, poiCandidates, poiForwardingTo);
        i++;
      }
    }

    // If no candidates were found and it's a method, try forwarding
    // This supports the forwarding-to-forwarding case.
    if (nonPoiCandidates.empty() && poiCandidates.empty()) {
      for (const auto& fci : forwardingCis) {
        if (fci.isMethodCall() && fci.numActuals() >= 1) {
          const Type* receiverType = fci.actual(0).type().type();
          if (typeUsesForwarding(context, receiverType)) {
            gatherAndFilterCandidatesForwarding(context, call, fci,
                                                inScope, inPoiScope,
                                                nonPoiCandidates,
                                                poiCandidates,
                                                nonPoiForwardingTo,
                                                poiForwardingTo);
          }
        }
      }
    }
  }
}

// TODO: Could/should this be a parsing query?
static bool isInsideForwarding(Context* context, const Call* call) {
  bool insideForwarding = false;
  if (call != nullptr) {
    auto p = parsing::parentAst(context, call);
    while (p != nullptr) {
      // If we encounter an aggregate or function, we're definitely not in
      // a forwarding statement.
      if (p->isAggregateDecl() || p->isFunction()) break;

      if (p->isForwardingDecl()) {
        insideForwarding = true;
        break;
      }

      p = parsing::parentAst(context, p);
    }
  }

  return insideForwarding;
}

// Returns candidates (including instantiating candidates)
// for resolving CallInfo 'ci'.
//
// call can be nullptr. in that event, ci.name() will be used
// to find the call with that name.
//
// forwardingTo is a vector that will be empty unless forwarding
// is used for some candidates.
//
// If forwarding is used, it will have an element for each of the returned
// candidates and will indicate the actual type that is passed
// to the 'this' receiver formal.
static CandidatesVec
gatherAndFilterCandidates(Context* context,
                          const Call* call,
                          const CallInfo& ci,
                          const Scope* inScope,
                          const PoiScope* inPoiScope,
                          size_t& firstPoiCandidate,
                          ForwardingInfoVec& forwardingInfo,
                          std::vector<ApplicabilityResult>* rejected) {
  CandidatesVec candidates;
  CheckedScopes visited;
  firstPoiCandidate = 0;

  // inject compiler-generated candidates in a manner similar to below
  // (note that any added candidates are already fully instantiated &
  //  the poiInfo from these is not gathered, because such methods should
  //  always be available in any scope that can refer to the type & are
  //  considered part of the custom type)
  considerCompilerGeneratedCandidates(context, ci, inScope, inPoiScope,
                                      candidates);

  // next, look for candidates without using POI.
  {
    // compute the potential functions that it could resolve to
    auto v = lookupCalledExpr(context, inScope, ci, visited);

    // filter without instantiating yet
    const auto& initialCandidatesAndRejections =
      filterCandidatesInitialGatherRejected(context, std::move(v), ci, rejected != nullptr);
    const auto& initialCandidates = initialCandidatesAndRejections.first;
    const auto& initialRejections = initialCandidatesAndRejections.second;

    if (rejected != nullptr) {
      rejected->insert(rejected->end(),
                       initialRejections.begin(),
                       initialRejections.end());
    }

    // find candidates, doing instantiation if necessary
    filterCandidatesInstantiating(context,
                                  initialCandidates,
                                  ci,
                                  inScope,
                                  inPoiScope,
                                  candidates,
                                  rejected);
  }

  // next, look for candidates using POI
  firstPoiCandidate = candidates.size();
  for (const PoiScope* curPoi = inPoiScope;
       curPoi != nullptr;
       curPoi = curPoi->inFnPoi()) {

    // stop if any candidate has been found.
    if (candidates.empty() == false) {
      break;
    }

    // compute the potential functions that it could resolve to
    auto v = lookupCalledExpr(context, curPoi->inScope(), ci, visited);

    // filter without instantiating yet
    const auto& initialCandidatesAndRejections =
      filterCandidatesInitialGatherRejected(context, std::move(v), ci, rejected != nullptr);
    const auto& initialCandidates = initialCandidatesAndRejections.first;
    const auto& initialRejections = initialCandidatesAndRejections.second;

    if (rejected != nullptr) {
      rejected->insert(rejected->end(),
                       initialRejections.begin(),
                       initialRejections.end());
    }

    // find candidates, doing instantiation if necessary
    filterCandidatesInstantiating(context,
                                  initialCandidates,
                                  ci,
                                  inScope,
                                  inPoiScope,
                                  candidates,
                                  rejected);
  }

  // If no candidates were found and it's a method, try forwarding
  if (candidates.empty() && ci.isMethodCall() && ci.numActuals() >= 1) {
    const Type* receiverType = ci.actual(0).type().type();

    // TODO: Should this information come as a boolean argument set by the
    // Resolver? It would be less expensive to set a boolean on Resolver once
    // we encounter a ForwardingDecl.
    //
    // Possible recursion here when resolving a function call in a forwarding
    // statement:
    //     record R { forwarding foo(); }
    // We need to try resolving 'foo()' as a method on 'R', which eventually
    // leads us back to this path here.
    //
    // By skipping the gathering of forwarding candidates below, we also
    // prevent forwarding statements from containing expressions that
    // themselves require forwarding. For example, if you had a couple of
    // forwarding statements like:
    //     forwarding b;
    //     forwarding bar();
    // The 'isInsideForwarding' check below would prevent resolving a method
    // 'bar()' on 'b'.

    if (typeUsesForwarding(context, receiverType) &&
        !isInsideForwarding(context, call)) {
      CandidatesVec nonPoiCandidates;
      CandidatesVec poiCandidates;
      ForwardingInfoVec nonPoiForwardingTo;
      ForwardingInfoVec poiForwardingTo;

      gatherAndFilterCandidatesForwarding(context, call, ci,
                                          inScope, inPoiScope,
                                          nonPoiCandidates, poiCandidates,
                                          nonPoiForwardingTo, poiForwardingTo);

      // append non-poi candidates
      candidates.insert(candidates.end(),
                        nonPoiCandidates.begin(), nonPoiCandidates.end());
      forwardingInfo.insert(forwardingInfo.end(),
                            nonPoiForwardingTo.begin(),
                            nonPoiForwardingTo.end());
      // append poi candidates
      firstPoiCandidate = candidates.size();
      candidates.insert(candidates.end(),
                        poiCandidates.begin(), poiCandidates.end());
      forwardingInfo.insert(forwardingInfo.end(),
                            poiForwardingTo.begin(),
                            poiForwardingTo.end());
    }
  }

  return candidates;
}

// * find the best candidates (disambiguate)
// * check signatures of selected candidates
// * gather POI info from any instantiations
static MostSpecificCandidates
findMostSpecificAndCheck(Context* context,
                         const CandidatesVec& candidates,
                         const ForwardingInfoVec& forwardingInfo,
                         size_t firstPoiCandidate,
                         const Call* call,
                         const CallInfo& ci,
                         const Scope* inScope,
                         const PoiScope* inPoiScope,
                         PoiInfo& poiInfo) {

  // find most specific candidates / disambiguate
  MostSpecificCandidates mostSpecific =
    findMostSpecificCandidates(context, candidates, forwardingInfo,
                               ci, inScope, inPoiScope);

  // perform fn signature checking for any instantiated candidates that are used
  for (const MostSpecificCandidate& candidate : mostSpecific) {
    if (candidate && candidate.fn()->instantiatedFrom()) {
      checkSignature(context, candidate.fn());
    }
  }

  // note any most-specific candidates from POI in poiInfo.
  {
    size_t n = candidates.size();
    for (size_t i = firstPoiCandidate; i < n; i++) {
      for (const MostSpecificCandidate& candidate : mostSpecific) {
        if (candidate.fn() == candidates[i]) {
          poiInfo.addIds(call->id(), candidate.fn()->id());
        }
      }
    }
  }

  return mostSpecific;
}


static MostSpecificCandidates
resolveFnCallFilterAndFindMostSpecific(Context* context,
                                       const Call* call,
                                       const CallInfo& ci,
                                       const Scope* inScope,
                                       const PoiScope* inPoiScope,
                                       PoiInfo& poiInfo,
                                       std::vector<ApplicabilityResult>* rejected) {

  // search for candidates at each POI until we have found candidate(s)
  size_t firstPoiCandidate = 0;
  ForwardingInfoVec forwardingInfo;
  CandidatesVec candidates = gatherAndFilterCandidates(context, call, ci,
                                                       inScope, inPoiScope,
                                                       firstPoiCandidate,
                                                       forwardingInfo,
                                                       rejected);

  // * find most specific candidates / disambiguate
  // * check signatures
  // * gather POI info

  MostSpecificCandidates mostSpecific =
    findMostSpecificAndCheck(context,
                             candidates, forwardingInfo, firstPoiCandidate,
                             call, ci,
                             inScope, inPoiScope, poiInfo);

  return mostSpecific;
}

// call can be nullptr. in that event ci.name() will be used to find
// what is called.
static
CallResolutionResult resolveFnCall(Context* context,
                                   const Call* call,
                                   const CallInfo& ci,
                                   const Scope* inScope,
                                   const PoiScope* inPoiScope,
                                   std::vector<ApplicabilityResult>* rejected) {
  PoiInfo poiInfo;
  MostSpecificCandidates mostSpecific;

  // Note: currently type constructors are not implemented as methods
  if (ci.calledType().kind() == QualifiedType::TYPE &&
      ci.isMethodCall() == false) {
    // handle invocation of a type constructor from a type
    // (note that we might have the type through a type alias)
    mostSpecific = resolveFnCallForTypeCtor(context, ci,
                                            inScope, inPoiScope,
                                            poiInfo);
  } else {
    // * search for candidates at each POI until we have found a candidate
    // * filter and instantiate
    // * disambiguate
    // * note any most specific candidates from POI in poiInfo.
    mostSpecific = resolveFnCallFilterAndFindMostSpecific(context, call, ci,
                                                          inScope, inPoiScope,
                                                          poiInfo, rejected);
  }

  // fully resolve each candidate function and gather poiScopesUsed.

  // figure out the poiScope to use
  const PoiScope* instantiationPoiScope = nullptr;
  bool anyInstantiated = false;

  for (const MostSpecificCandidate& candidate : mostSpecific) {
    if (candidate && candidate.fn()->instantiatedFrom() != nullptr) {
      anyInstantiated = true;
      break;
    }
  }

  if (anyInstantiated) {
    instantiationPoiScope =
      pointOfInstantiationScope(context, inScope, inPoiScope);
    poiInfo.setPoiScope(instantiationPoiScope);

    for (const MostSpecificCandidate& candidate : mostSpecific) {
      if (candidate) {
        if (candidate.fn()->untyped()->idIsFunction()) {
          // note: following call returns early if candidate not instantiated
          accumulatePoisUsedByResolvingBody(context, candidate.fn(),
                                            instantiationPoiScope, poiInfo);
        }
      }
    }
  }

  // infer types of generic 'out' formals from function bodies
  mostSpecific.inferOutFormals(context, instantiationPoiScope);

  // Make sure that we are resolving initializer bodies even when the
  // signature is concrete, because there are semantic checks.
  if (isCallInfoForInitializer(ci) && mostSpecific.numBest() == 1) {
    auto candidateFn = mostSpecific.only().fn();
    CHPL_ASSERT(isTfsForInitializer(candidateFn));

    // TODO: Can we move this into the 'InitVisitor'?
    if (!candidateFn->untyped()->isCompilerGenerated()) {
      std::ignore = resolveInitializer(context, candidateFn, inPoiScope);
    }
  }

  // compute the return types
  QualifiedType retType;
  bool retTypeSet = false;
  for (const MostSpecificCandidate& candidate : mostSpecific) {
    if (candidate.fn() != nullptr) {
      QualifiedType t = returnType(context, candidate.fn(), instantiationPoiScope);
      if (retTypeSet && retType.type() != t.type()) {
        context->error(candidate.fn(),
                       nullptr,
                       "return intent overload type does not match");
      }
      retType = t;
      retTypeSet = true;
    }
  }

  return CallResolutionResult(mostSpecific, retType, std::move(poiInfo));
}

static
CallResolutionResult resolveTupleExpr(Context* context,
                                      const Tuple* tuple,
                                      const CallInfo& ci,
                                      const Scope* inScope,
                                      const PoiScope* inPoiScope) {
  // resolve the tuple type from a tuple expression

  // per spec:
  // Tuple expressions are a form of referential tuple.
  // Like most other referential tuples, tuple expressions capture each
  // element based on the default argument intent of the element’s type.

  // check if the elements are all type or all value
  bool anyUnknown = false;
  bool allType = true;
  bool allValue = true;
  for (const auto& actual : ci.actuals()) {
    QualifiedType q = actual.type();
    const Type* t = q.type();
    if (t == nullptr || t->isUnknownType())
      anyUnknown = true;
    else if (q.kind() == QualifiedType::TYPE)
      allValue = false;
    else
      allType = false;
  }

  // if any argument is Unknown / null, return Unknown
  if (anyUnknown) {
    auto unk = UnknownType::get(context);
    return CallResolutionResult(QualifiedType(QualifiedType::CONST_VAR, unk));
  }

  // if there is a mix of value and type elements, error
  if (allType == false && allValue == false) {
    context->error(tuple, "Mix of value and type tuple elements in tuple expr");
    auto e = ErroneousType::get(context);
    return CallResolutionResult(QualifiedType(QualifiedType::CONST_VAR, e));
  }

  // otherwise, construct the tuple type
  std::vector<const Type*> eltTypes;

  QualifiedType::Kind kind = QualifiedType::UNKNOWN;
  if (allValue)
    kind = QualifiedType::CONST_VAR;
  else if (allType)
    kind = QualifiedType::TYPE;

  for (const auto& actual : ci.actuals()) {
    QualifiedType q = actual.type();
    const Type* t = q.type();
    eltTypes.push_back(t);
  }

  const TupleType* t = nullptr;
  if (allType)
    t = TupleType::getValueTuple(context, std::move(eltTypes));
  else
    t = TupleType::getReferentialTuple(context, std::move(eltTypes));

  return CallResolutionResult(QualifiedType(kind, t));
}

static bool shouldAttemptImplicitReceiver(const CallInfo& ci,
                                          QualifiedType implicitReceiver) {
  return !ci.isMethodCall() &&
         !ci.isOpCall() &&
         implicitReceiver.type() != nullptr &&
         // Assuming ci.name().isEmpty()==true implies a primitive call.
         // TODO: Add some kind of 'isPrimitive()' to CallInfo
         !ci.name().isEmpty() &&
         ci.name() != USTR("?") &&
         ci.name() != USTR("owned") &&
         ci.name() != USTR("shared") &&
         ci.name() != USTR("borrowed") &&
         ci.name() != USTR("unmanaged");
}

CallResolutionResult resolveCall(Context* context,
                                 const Call* call,
                                 const CallInfo& ci,
                                 const Scope* inScope,
                                 const PoiScope* inPoiScope,
                                 std::vector<ApplicabilityResult>* rejected) {
  if (call->isFnCall() || call->isOpCall()) {
    // see if the call is handled directly by the compiler
    QualifiedType tmpRetType;
    if (resolvePostfixNilableAppliedToNew(context, call, ci, tmpRetType)) {
      return CallResolutionResult(std::move(tmpRetType));
    }
    if (resolveFnCallSpecial(context, call, ci, tmpRetType)) {
      return CallResolutionResult(std::move(tmpRetType));
    }

    CallResolutionResult keywordRes;
    if (resolveFnCallSpecialType(context, call, ci,
                                 inScope, inPoiScope, keywordRes)) {
      return keywordRes;
    }

    // otherwise do regular call resolution
    return resolveFnCall(context, call, ci, inScope, inPoiScope, rejected);
  } else if (auto prim = call->toPrimCall()) {
    return resolvePrimCall(context, prim, ci, inScope, inPoiScope);
  } else if (auto tuple = call->toTuple()) {
    return resolveTupleExpr(context, tuple, ci, inScope, inPoiScope);
  }

  CHPL_ASSERT(false && "should not be reached");
  MostSpecificCandidates emptyCandidates;
  QualifiedType emptyType;
  PoiInfo emptyPoi;
  return CallResolutionResult(emptyCandidates, emptyType, emptyPoi);
}

CallResolutionResult resolveCallInMethod(Context* context,
                                         const Call* call,
                                         const CallInfo& ci,
                                         const Scope* inScope,
                                         const PoiScope* inPoiScope,
                                         QualifiedType implicitReceiver,
                                         std::vector<ApplicabilityResult>* rejected) {

  // If there is an implicit receiver and ci isn't written as a method,
  // construct a method call and use that instead. If that resolves,
  // it takes precedence over functions.
  if (shouldAttemptImplicitReceiver(ci, implicitReceiver)) {
    auto methodCi = CallInfo::createWithReceiver(ci, implicitReceiver);
    auto ret = resolveCall(context, call, methodCi, inScope, inPoiScope, rejected);
    if (ret.mostSpecific().foundCandidates()) {
      return ret;
    }
  }

  // otherwise, use normal resolution
  return resolveCall(context, call, ci, inScope, inPoiScope, rejected);
}

CallResolutionResult resolveGeneratedCall(Context* context,
                                          const AstNode* astForErr,
                                          const CallInfo& ci,
                                          const Scope* inScope,
                                          const PoiScope* inPoiScope,
                                          std::vector<ApplicabilityResult>* rejected) {
  // see if the call is handled directly by the compiler
  QualifiedType tmpRetType;
  if (resolveFnCallSpecial(context, astForErr, ci, tmpRetType)) {
    return CallResolutionResult(std::move(tmpRetType));
  }
  // otherwise do regular call resolution
  return resolveFnCall(context, /* call */ nullptr, ci, inScope, inPoiScope, rejected);
}

CallResolutionResult
resolveGeneratedCallInMethod(Context* context,
                             const AstNode* astForErr,
                             const CallInfo& ci,
                             const Scope* inScope,
                             const PoiScope* inPoiScope,
                             QualifiedType implicitReceiver) {
  // If there is an implicit receiver and ci isn't written as a method,
  // construct a method call and use that instead. If that resolves,
  // it takes precedence over functions.
  if (shouldAttemptImplicitReceiver(ci, implicitReceiver)) {
    auto methodCi = CallInfo::createWithReceiver(ci, implicitReceiver);
    auto ret = resolveGeneratedCall(context, astForErr, methodCi,
                                    inScope, inPoiScope);
    if (ret.mostSpecific().foundCandidates()) {
      return ret;
    }
  }

  // otherwise, resolve a regular function call
  return resolveGeneratedCall(context, astForErr, ci, inScope, inPoiScope);
}

static bool helpFieldNameCheck(const AstNode* ast,
                               UniqueString name) {
  if (auto var = ast->toVarLikeDecl()) {
    return var->name() == name;
  } else if (auto mult = ast->toMultiDecl()) {
    for (auto decl : mult->decls()) {
      bool found = helpFieldNameCheck(decl, name);
      if (found) {
        return true;
      }
    }
  } else if (auto tup = ast->toTupleDecl()) {
    for (auto decl : tup->decls()) {
      bool found = helpFieldNameCheck(decl, name);
      if (found) {
        return true;
      }
    }
  } else if (auto fwd = ast->toForwardingDecl()) {
    if (auto fwdVar = fwd->expr()->toVariable()) {
      return fwdVar->name() == name;
    }
  }
  return false;
}

static const CompositeType* const&
isNameOfFieldQuery(Context* context,
                   UniqueString name, const CompositeType* ct) {
  QUERY_BEGIN(isNameOfFieldQuery, context, name, ct);

  const CompositeType* result = nullptr;
  auto ast = parsing::idToAst(context, ct->id());
  CHPL_ASSERT(ast && ast->isAggregateDecl());
  auto ad = ast->toAggregateDecl();

  for (auto child: ad->children()) {
    // Ignore everything other than VarLikeDecl, MultiDecl, TupleDecl
    if (child->isVarLikeDecl() ||
        child->isMultiDecl() ||
        child->isTupleDecl() ||
        child->isForwardingDecl()) {
      bool found = helpFieldNameCheck(child, name);
      if (found) {
        result = ct;
        break;
      }
    }
  }

  if (result == nullptr) {
    // check also superclass fields
    if (auto bct = ct->toBasicClassType()) {
      const CompositeType* found =
        isNameOfField(context, name, bct->parentClassType());
      if (found != nullptr) {
        result = found;
      }
    }
  }

  return QUERY_END(result);
}

const CompositeType* isNameOfField(Context* context,
                                   UniqueString name,
                                   const Type* t) {

  if (t == nullptr) {
    return nullptr;
  }

  const CompositeType* ct = t->getCompositeType();

  if (ct == nullptr) {
    return nullptr;
  }

  if (auto bct = ct->toBasicClassType()) {
    if (bct->isObjectType()) {
      return nullptr;
    }
  }

  return isNameOfFieldQuery(context, name, ct);
}

// TODO: This is very early draft and is missing a lot, e.g.,
//    - No valid default-initializer present
//    - Instantiated generics must supply type/param arguments when
//      searching for a default-initializer
//    - Consideration of 'where' clauses
//    - Composites with compilerError'd default-initializers
//    - Mutually recursive class types
//    - Non-nil 'owned' classes
static bool
isTypeDefaultInitializableImpl(Context* context, const Type* t) {
  const auto g = t->genericity();

  switch (g) {
    case Type::CONCRETE: return true;
    case Type::GENERIC: return false;

    // For these, consider the fields.
    case Type::GENERIC_WITH_DEFAULTS:
    case Type::MAYBE_GENERIC:
      break;
  }

  CHPL_ASSERT(!t->isPrimitiveType());

  if (t->isBuiltinType()) {
    CHPL_ASSERT(false && "Not handled!");
  }

  if (auto ct = t->toCompositeType()) {
    const auto p = DefaultsPolicy::USE_DEFAULTS;
    auto& rf = fieldsForTypeDecl(context, ct, p);

    if (!rf.isGeneric()) return true;

    // TODO: Do I still need to consider field genericity, here? I.E., if
    // a field is marked 'GENERIC_WITH_DEFAULTS' is there more to do?
    // If I can tell the thing is concrete from the ResolvedFields, then
    // there's probably no need to recurse.
    if (rf.isGenericWithDefaults()) {
      for (int i = 0; i < rf.numFields(); i++) {
        auto ft = rf.fieldType(i).type();

        // TODO: Skipping avoids a recursive query but doesn't handle
        // mutually recursive classes.
        if (ft == t) continue;

        if (!isTypeDefaultInitializable(context, ft)) return false;
      }

      return true;
    }
  }

  return false;
}

static const bool&
isTypeDefaultInitializableQuery(Context* context, const Type* t) {
  QUERY_BEGIN(isTypeDefaultInitializableQuery, context, t);
  bool ret = isTypeDefaultInitializableImpl(context, t);
  return QUERY_END(ret);
}

bool isTypeDefaultInitializable(Context* context, const Type* t) {
  return isTypeDefaultInitializableQuery(context, t);
}


template <typename T>
QualifiedType paramTypeFromValue(Context* context, T value);

template <>
QualifiedType paramTypeFromValue<bool>(Context* context, bool value) {
  return QualifiedType(QualifiedType::PARAM,
                       BoolType::get(context),
                       BoolParam::get(context, value));
}

const std::unordered_map<UniqueString, QualifiedType>&
getCompilerGeneratedGlobals(Context* context) {
  QUERY_BEGIN(getCompilerGeneratedGlobals, context);

  auto& globals = compilerGlobals(context);
  std::unordered_map<UniqueString, QualifiedType> result;
  #define COMPILER_GLOBAL(TYPE__, IDENT__, NAME__)\
    result[UniqueString::get(context, IDENT__)] = \
      paramTypeFromValue<TYPE__>(context, globals.NAME__);
  #include "chpl/uast/compiler-globals-list.h"
  #undef COMPILER_GLOBAL

  return QUERY_END(result);
}

static const bool&
reportInvalidMultipleInheritanceImpl(Context* context,
                                     const uast::Class* node,
                                     const uast::AstNode* firstParent,
                                     const uast::AstNode* secondParent) {
  QUERY_BEGIN(reportInvalidMultipleInheritanceImpl, context, node, firstParent, secondParent);
  CHPL_REPORT(context, MultipleInheritance, node, firstParent, secondParent);
  auto result = false;
  return QUERY_END(result);
}

void
reportInvalidMultipleInheritance(Context* context,
                                 const uast::Class* node,
                                 const uast::AstNode* firstParent,
                                 const uast::AstNode* secondParent) {

  std::ignore = reportInvalidMultipleInheritanceImpl(context, node,
                                                     firstParent, secondParent);
}


} // end namespace resolution
} // end namespace chpl
