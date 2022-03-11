//===- ExpandWhens.cpp - Expand WhenOps into muxed operations ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the ExpandWhens pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Support/FieldRef.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"

using namespace circt;
using namespace firrtl;

/// Move all operations from a source block in to a destination block. Leaves
/// the source block empty.
static void mergeBlock(Block &destination, Block::iterator insertPoint,
                       Block &source) {
  destination.getOperations().splice(insertPoint, source.getOperations());
}

/// This is a stack of hashtables, if lookup fails in the top-most hashtable,
/// it will attempt to lookup in lower hashtables.  This class is used instead
/// of a ScopedHashTable so we can manually pop off a scope and keep it around.
///
/// This only allows inserting into the outermost scope.
template <typename KeyT, typename ValueT>
struct HashTableStack {
  using ScopeT = typename llvm::MapVector<KeyT, ValueT>;
  using StackT = typename llvm::SmallVector<ScopeT, 3>;

  HashTableStack() {
    // We require at least one scope.
    pushScope();
  }

  // TODO: This class will need its own iterator eventually.
  using iterator = typename ScopeT::iterator;

  iterator end() { return iterator(); }

  iterator find(const KeyT &key) {
    // Try to find a hashtable with the missing value.
    for (auto &map : llvm::reverse(mapStack)) {
      auto it = map.find(key);
      if (it != map.end())
        return it;
    }
    return iterator();
  }

  ScopeT &getLastScope() { return mapStack.back(); }

  void pushScope() { mapStack.emplace_back(); }

  ScopeT popScope() {
    assert(mapStack.size() > 1 && "Cannot pop the last scope");
    return mapStack.pop_back_val();
  }

  // This class lets you insert into the top scope.
  ValueT &operator[](const KeyT &key) { return mapStack.back()[key]; }

private:
  StackT mapStack;
};

/// This is a determistic mapping of a FieldRef to the last operation which set
/// a value to it.
using ScopedDriverMap = HashTableStack<FieldRef, Operation *>;
using DriverMap = ScopedDriverMap::ScopeT;

//===----------------------------------------------------------------------===//
// Last Connect Resolver
//===----------------------------------------------------------------------===//

namespace {
/// This visitor visits process a block resolving last connect semantics
/// and recursively expanding WhenOps.
template <typename ConcreteT>
class LastConnectResolver : public FIRRTLVisitor<ConcreteT> {
protected:
  /// Map of destinations and the operation which is driving a value to it in
  /// the current scope. This is used for resolving last connect semantics, and
  /// for retrieving the responsible connect operation.
  ScopedDriverMap &driverMap;

public:
  LastConnectResolver(ScopedDriverMap &driverMap) : driverMap(driverMap) {}

  using FIRRTLVisitor<ConcreteT>::visitExpr;
  using FIRRTLVisitor<ConcreteT>::visitDecl;
  using FIRRTLVisitor<ConcreteT>::visitStmt;

  /// Records a connection to a destination in the current scope. This will
  /// delete a previous connection to a destination if there was one. Returns
  /// true if an old connect was erased.
  bool setLastConnect(FieldRef dest, Operation *connection) {
    // Try to insert, if it doesn't insert, replace the previous value.
    auto itAndInserted = driverMap.getLastScope().insert({dest, connection});
    if (!std::get<1>(itAndInserted)) {
      auto iterator = std::get<0>(itAndInserted);
      auto changed = false;
      // Delete the old connection if it exists. Null connections are inserted
      // on declarations.
      if (auto *oldConnect = iterator->second) {
        oldConnect->erase();
        changed = true;
      }
      iterator->second = connection;
      return changed;
    }
    return false;
  }

  /// Get the destination value from a connection.  This supports any operation
  /// which is capable of driving a value.
  static Value getDestinationValue(Operation *op) {
    if (auto connect = dyn_cast<ConnectOp>(op))
      return connect.dest();
    return cast<StrictConnectOp>(op).dest();
  }

  /// Get the source value from a connection. This supports any operation which
  /// is capable of driving a value.
  static Value getConnectedValue(Operation *op) {
    if (auto connect = dyn_cast<ConnectOp>(op))
      return connect.src();
    return cast<StrictConnectOp>(op).src();
  }

  /// For every leaf field in the sink, record that it exists and should be
  /// initialized.
  void declareSinks(Value value, Flow flow) {
    auto type = value.getType();
    unsigned id = 0;

    // Recurse through a bundle and declare each leaf sink node.
    std::function<void(Type, Flow)> declare = [&](Type type, Flow flow) {
      // If this is a bundle type, recurse to each of the fields.
      if (auto bundleType = type.dyn_cast<BundleType>()) {
        for (auto &element : bundleType.getElements()) {
          id++;
          if (element.isFlip)
            declare(element.type, swapFlow(flow));
          else
            declare(element.type, flow);
        }
        return;
      }

      // If this is a vector type, recurse to each of the elements.
      if (auto vectorType = type.dyn_cast<FVectorType>()) {
        for (unsigned i = 0; i < vectorType.getNumElements(); ++i) {
          id++;
          declare(vectorType.getElementType(), flow);
        }
        return;
      }

      // If this is an analog type, it does not need to be tracked.
      if (auto analogType = type.dyn_cast<AnalogType>())
        return;

      // If it is a leaf node with Flow::Sink or Flow::Duplex, it must be
      // initialized.
      if (flow != Flow::Source)
        driverMap[{value, id}] = nullptr;
    };
    declare(type, flow);
  }

  /// Take two connection operations and merge them into a new connect under a
  /// condition.  Destination of both connects should be `dest`.
  ConnectOp flattenConditionalConnections(OpBuilder &b, Location loc,
                                          Value dest, Value cond,
                                          Operation *whenTrueConn,
                                          Operation *whenFalseConn) {
    auto fusedLoc =
        b.getFusedLoc({loc, whenTrueConn->getLoc(), whenFalseConn->getLoc()});
    auto whenTrue = getConnectedValue(whenTrueConn);
    auto trueIsInvalid =
        isa_and_nonnull<InvalidValueOp>(whenTrue.getDefiningOp());
    auto whenFalse = getConnectedValue(whenFalseConn);
    auto falseIsInvalid =
        isa_and_nonnull<InvalidValueOp>(whenFalse.getDefiningOp());
    // If one of the branches of the mux is an invalid value, we optimize the
    // mux to be the non-invalid value.  This optimization can only be
    // performed while lowering when-ops into muxes, and would not be legal as
    // a more general mux folder.
    // mux(cond, invalid, x) -> x
    // mux(cond, x, invalid) -> x
    Value newValue = whenTrue;
    if (trueIsInvalid == falseIsInvalid)
      newValue = b.createOrFold<MuxPrimOp>(fusedLoc, cond, whenTrue, whenFalse);
    else if (trueIsInvalid)
      newValue = whenFalse;
    return b.create<ConnectOp>(loc, dest, newValue);
  }

  void visitDecl(WireOp op) { declareSinks(op.result(), Flow::Duplex); }

  /// Take an aggregate value and construct ground subelements recursively.
  /// And then apply function `fn`.
  void foreachSubelement(OpBuilder &builder, Value value,
                         llvm::function_ref<void(Value)> fn) {
    TypeSwitch<Type>(value.getType())
        .template Case<BundleType>([&](BundleType bundle) {
          for (auto i : llvm::seq(0u, (unsigned)bundle.getNumElements())) {
            auto subfield =
                builder.create<SubfieldOp>(value.getLoc(), value, i);
            foreachSubelement(builder, subfield, fn);
          }
        })
        .template Case<FVectorType>([&](FVectorType vector) {
          for (auto i : llvm::seq((size_t)0, vector.getNumElements())) {
            auto subindex =
                builder.create<SubindexOp>(value.getLoc(), value, i);
            foreachSubelement(builder, subindex, fn);
          }
        })
        .Default([&](auto) { fn(value); });
  }

  void visitDecl(RegOp op) {
    // Registers are initialized to themselves. If the register has an
    // aggergate type, connect each ground type element.
    auto builder = OpBuilder(op->getBlock(), ++Block::iterator(op));
    auto fn = [&](Value value) {
      auto connect = builder.create<ConnectOp>(value.getLoc(), value, value);
      driverMap[getFieldRefFromValue(value)] = connect;
    };
    foreachSubelement(builder, op.result(), fn);
  }

  void visitDecl(RegResetOp op) {
    // Registers are initialized to themselves. If the register has an
    // aggergate type, connect each ground type element.
    auto builder = OpBuilder(op->getBlock(), ++Block::iterator(op));
    auto fn = [&](Value value) {
      auto connect = builder.create<ConnectOp>(value.getLoc(), value, value);
      driverMap[getFieldRefFromValue(value)] = connect;
    };
    foreachSubelement(builder, op.result(), fn);
  }

  void visitDecl(InstanceOp op) {
    // Track any instance inputs which need to be connected to for init
    // coverage.
    for (auto result : llvm::enumerate(op.results()))
      if (op.getPortDirection(result.index()) == Direction::Out)
        declareSinks(result.value(), Flow::Source);
      else
        declareSinks(result.value(), Flow::Sink);
  }

  void visitDecl(MemOp op) {
    // Track any memory inputs which require connections.
    for (auto result : op.results())
      declareSinks(result, Flow::Sink);
  }

  void visitStmt(PartialConnectOp op) {
    llvm_unreachable("PartialConnectOps should have been removed.");
  }

  void visitStmt(ConnectOp op) {
    setLastConnect(getFieldRefFromValue(op.dest()), op);
  }

  void visitStmt(StrictConnectOp op) {
    setLastConnect(getFieldRefFromValue(op.dest()), op);
  }

  void processWhenOp(WhenOp whenOp, Value outerCondition);

  /// Combine the connect statements from each side of the block. There are 5
  /// cases to consider. If all are set, last connect semantics dictate that it
  /// is actually the third case.
  ///
  /// Prev | Then | Else | Outcome
  /// -----|------|------|-------
  ///      |  set |      | then
  ///      |      |  set | else
  ///  set |  set |  set | mux(p, then, else)
  ///      |  set |  set | impossible
  ///  set |  set |      | mux(p, then, prev)
  ///  set |      |  set | mux(p, prev, else)
  ///
  /// If the value was declared in the block, then it does not need to have been
  /// assigned a previous value.  If the value was declared before the block,
  /// then there is an incomplete initialization error.
  void mergeScopes(Location loc, DriverMap &thenScope, DriverMap &elseScope,
                   Value thenCondition) {

    // Process all connects in the `then` block.
    for (auto &destAndConnect : thenScope) {
      auto dest = std::get<0>(destAndConnect);
      auto thenConnect = std::get<1>(destAndConnect);

      auto outerIt = driverMap.find(dest);
      if (outerIt == driverMap.end()) {
        // `dest` is set in `then` only. This indicates it was created in the
        // `then` block, so just copy it into the outer scope.
        driverMap[dest] = thenConnect;
        continue;
      }

      auto elseIt = elseScope.find(dest);
      if (elseIt != elseScope.end()) {
        // `dest` is set in `then` and `else`. We need to combine them into and
        // delete any previous connect.

        // Create a new connect with `mux(p, then, else)`.
        auto &elseConnect = std::get<1>(*elseIt);
        OpBuilder connectBuilder(elseConnect);
        auto newConnect = flattenConditionalConnections(
            connectBuilder, loc, getDestinationValue(thenConnect),
            thenCondition, thenConnect, elseConnect);

        // Delete all old connections.
        thenConnect->erase();
        elseConnect->erase();
        setLastConnect(dest, newConnect);

        // Do not process connect in the else scope.
        elseScope.erase(dest);
        continue;
      }

      auto &outerConnect = std::get<1>(*outerIt);
      if (!outerConnect) {
        // `dest` is null in the outer scope. This indicate an initialization
        // problem: `mux(p, then, nullptr)`. Just delete the broken connect.
        thenConnect->erase();
        continue;
      }

      // `dest` is set in `then` and the outer scope.  Create a new connect with
      // `mux(p, then, outer)`.
      OpBuilder connectBuilder(thenConnect);
      auto newConnect = flattenConditionalConnections(
          connectBuilder, loc, getDestinationValue(thenConnect), thenCondition,
          thenConnect, outerConnect);

      // Delete all old connections.
      thenConnect->erase();
      setLastConnect(dest, newConnect);
    }

    // Process all connects in the `else` block.
    for (auto &destAndConnect : elseScope) {
      auto dest = std::get<0>(destAndConnect);
      auto elseConnect = std::get<1>(destAndConnect);

      auto outerIt = driverMap.find(dest);
      if (outerIt == driverMap.end()) {
        // `dest` is set in `else` only. This indicates it was created in the
        // `else` block, so just copy it into the outer scope.
        driverMap[dest] = elseConnect;
        continue;
      }

      auto &outerConnect = std::get<1>(*outerIt);
      if (!outerConnect) {
        // `dest` is null in the outer scope. This indicate an initialization
        // problem: `mux(p, null, else)`. Just delete the broken connect.
        elseConnect->erase();
        continue;
      }

      // `dest` is set in the `else` and outer scope. Create a new connect with
      // `mux(p, outer, else)`.
      OpBuilder connectBuilder(elseConnect);
      auto newConnect = flattenConditionalConnections(
          connectBuilder, loc, getDestinationValue(outerConnect), thenCondition,
          outerConnect, elseConnect);

      // Delete all old connections.
      elseConnect->erase();
      setLastConnect(dest, newConnect);
    }
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// WhenOpVisitor
//===----------------------------------------------------------------------===//

/// This extends the LastConnectVisitor to handle all Simulation related
/// constructs which do not neet any processing at the module scope, but need to
/// be processed inside of a WhenOp.
namespace {
class WhenOpVisitor : public LastConnectResolver<WhenOpVisitor> {

public:
  WhenOpVisitor(ScopedDriverMap &driverMap, Value condition)
      : LastConnectResolver<WhenOpVisitor>(driverMap), condition(condition) {}

  using LastConnectResolver<WhenOpVisitor>::visitExpr;
  using LastConnectResolver<WhenOpVisitor>::visitDecl;
  using LastConnectResolver<WhenOpVisitor>::visitStmt;

  /// Process a block, recording each declaration, and expanding all whens.
  void process(Block &block);

  /// Simulation Constructs.
  void visitStmt(AssertOp op);
  void visitStmt(AssumeOp op);
  void visitStmt(CoverOp op);
  void visitStmt(ModuleOp op);
  void visitStmt(PrintFOp op);
  void visitStmt(StopOp op);
  void visitStmt(WhenOp op);

private:
  /// And a 1-bit value with the current condition.  If we are in the outer
  /// scope, i.e. not in a WhenOp region, then there is no condition.
  Value andWithCondition(Operation *op, Value value) {
    // 'and' the value with the current condition.
    return OpBuilder(op).createOrFold<AndPrimOp>(
        condition.getLoc(), condition.getType(), condition, value);
  }

private:
  /// The current wrapping condition. If null, we are in the outer scope.
  Value condition;
};
} // namespace

void WhenOpVisitor::process(Block &block) {
  for (auto &op : llvm::make_early_inc_range(block)) {
    dispatchVisitor(&op);
  }
}

void WhenOpVisitor::visitStmt(PrintFOp op) {
  op.condMutable().assign(andWithCondition(op, op.cond()));
}

void WhenOpVisitor::visitStmt(StopOp op) {
  op.condMutable().assign(andWithCondition(op, op.cond()));
}

void WhenOpVisitor::visitStmt(AssertOp op) {
  op.enableMutable().assign(andWithCondition(op, op.enable()));
}

void WhenOpVisitor::visitStmt(AssumeOp op) {
  op.enableMutable().assign(andWithCondition(op, op.enable()));
}

void WhenOpVisitor::visitStmt(CoverOp op) {
  op.enableMutable().assign(andWithCondition(op, op.enable()));
}

void WhenOpVisitor::visitStmt(WhenOp whenOp) {
  processWhenOp(whenOp, condition);
}

/// This is a common helper that is dispatched to by the concrete visitors.
/// This condition should be the conjunction of all surrounding WhenOp
/// condititions.
///
/// This requires WhenOpVisitor to be fully defined.
template <typename ConcreteT>
void LastConnectResolver<ConcreteT>::processWhenOp(WhenOp whenOp,
                                                   Value outerCondition) {
  OpBuilder b(whenOp);
  auto loc = whenOp.getLoc();
  Block *parentBlock = whenOp->getBlock();
  auto condition = whenOp.condition();
  auto ui1Type = condition.getType();

  // Process both sides of the the WhenOp, fixing up all simulation
  // contructs, and resolving last connect semantics in each block. This
  // process returns the set of connects in each side of the when op.

  // Process the `then` block. If we are already in a whenblock, the we need to
  // conjoin ('and') the outer conditions.
  auto thenCondition = whenOp.condition();
  if (outerCondition)
    thenCondition =
        b.createOrFold<AndPrimOp>(loc, ui1Type, outerCondition, thenCondition);

  auto &thenBlock = whenOp.getThenBlock();
  driverMap.pushScope();
  WhenOpVisitor(driverMap, thenCondition).process(thenBlock);
  mergeBlock(*parentBlock, Block::iterator(whenOp), thenBlock);
  auto thenScope = driverMap.popScope();

  // Process the `else` block.
  DriverMap elseScope;
  if (whenOp.hasElseRegion()) {
    // Else condition is the compliment of the then condition.
    auto elseCondition =
        b.createOrFold<NotPrimOp>(loc, condition.getType(), condition);
    // Conjoin the when condition with the outer condition.
    if (outerCondition)
      elseCondition = b.createOrFold<AndPrimOp>(loc, ui1Type, outerCondition,
                                                elseCondition);
    auto &elseBlock = whenOp.getElseBlock();
    driverMap.pushScope();
    WhenOpVisitor(driverMap, elseCondition).process(elseBlock);
    mergeBlock(*parentBlock, Block::iterator(whenOp), elseBlock);
    elseScope = driverMap.popScope();
  }

  mergeScopes(loc, thenScope, elseScope, condition);

  // Delete the now empty WhenOp.
  whenOp.erase();
}

//===----------------------------------------------------------------------===//
// ModuleOpVisitor
//===----------------------------------------------------------------------===//

namespace {
/// This extends the LastConnectResolver to track if anything has changed.
class ModuleVisitor : public LastConnectResolver<ModuleVisitor> {
public:
  ModuleVisitor() : LastConnectResolver<ModuleVisitor>(driverMap) {}

  using LastConnectResolver<ModuleVisitor>::visitExpr;
  using LastConnectResolver<ModuleVisitor>::visitDecl;
  using LastConnectResolver<ModuleVisitor>::visitStmt;
  void visitStmt(WhenOp whenOp);
  void visitStmt(ConnectOp connectOp);
  void visitStmt(StrictConnectOp connectOp);

  bool run(FModuleOp op);
  LogicalResult checkInitialization();

private:
  /// The outermost scope of the module body.
  ScopedDriverMap driverMap;

  /// Tracks if anything in the IR has changed.
  bool anythingChanged = false;
};
} // namespace

/// Run expand whens on the Module.  This will emit an error for each
/// incomplete initialization found. If an initialiazation error was detected,
/// this will return failure and leave the IR in an inconsistent state.
bool ModuleVisitor::run(FModuleOp module) {
  // Track any results (flipped arguments) of the module for init coverage.
  for (auto it : llvm::enumerate(module.getArguments())) {
    auto flow = module.getPortDirection(it.index()) == Direction::In
                    ? Flow::Source
                    : Flow::Sink;
    declareSinks(it.value(), flow);
  }

  // Process the body of the module.
  for (auto &op : llvm::make_early_inc_range(*module.getBody())) {
    dispatchVisitor(&op);
  }
  return anythingChanged;
}

void ModuleVisitor::visitStmt(ConnectOp op) {
  anythingChanged |= setLastConnect(getFieldRefFromValue(op.dest()), op);
}

void ModuleVisitor::visitStmt(StrictConnectOp op) {
  anythingChanged |= setLastConnect(getFieldRefFromValue(op.dest()), op);
}

void ModuleVisitor::visitStmt(WhenOp whenOp) {
  // If we are deleting a WhenOp something definitely changed.
  anythingChanged = true;
  processWhenOp(whenOp, /*outerCondition=*/{});
}

/// Perform initialization checking.  This uses the built up state from
/// running on a module. Returns failure in the event of bad initialization.
LogicalResult ModuleVisitor::checkInitialization() {
  for (auto destAndConnect : driverMap.getLastScope()) {
    // If there is valid connection to this destination, everything is good.
    auto *connect = std::get<1>(destAndConnect);
    if (connect)
      continue;

    // Get the op which defines the sink, and emit an error.
    auto dest = std::get<0>(destAndConnect);
    dest.getDefiningOp()->emitError("sink \"" + getFieldName(dest) +
                                    "\" not fully initialized");
    return failure();
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
class ExpandWhensPass : public ExpandWhensBase<ExpandWhensPass> {
  void runOnOperation() override;
};
} // end anonymous namespace

void ExpandWhensPass::runOnOperation() {
  ModuleVisitor visitor;
  if (!visitor.run(getOperation()))
    markAllAnalysesPreserved();
  if (failed(visitor.checkInitialization()))
    signalPassFailure();
}

std::unique_ptr<mlir::Pass> circt::firrtl::createExpandWhensPass() {
  return std::make_unique<ExpandWhensPass>();
}
