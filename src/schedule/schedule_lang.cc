/*!
 *  Copyright (c) 2016 by Contributors
 * \file schedule_lang.cc
 */
#include <tvm/schedule.h>
#include <tvm/ir_mutator.h>
#include <unordered_set>
#include "./graph.h"

namespace tvm {

namespace {

// find first occurance location in leaf
template<typename T>
size_t FindNodeRef(ArrayNode* array_node, const T& v) {
  const Node* n = v.get();
  for (size_t i = 0; i < array_node->data.size(); ++i) {
    if (array_node->data[i].get() == n) return i;
  }
  return array_node->data.size();
}

size_t FindLeafVar(ArrayNode* all_vars, ArrayNode* leaf_vars, const IterVar& v) {
  size_t pos = FindNodeRef(leaf_vars, v);
  if (pos < leaf_vars->data.size()) return pos;

  if (FindNodeRef(all_vars, v) < all_vars->data.size()) {
    LOG(FATAL) << "Operate on iter var " << v
               << "that has already been splitted";
  } else {
    LOG(FATAL) << "Operate on iter var " << v
               << "that is not part of the schedule";
  }
  return 0;
}

void Split(StageNode* self, IterVar parent,
           IterVar outer, IterVar inner, Expr factor) {
  if (self->attach_type == kScanUpdate) {
    CHECK(!parent.same_as(self->all_iter_vars[0]))
        << "Cannot split on axis[0] of scan update";
  }
  ArrayNode* all_vars = self->all_iter_vars.CopyOnWrite();
  ArrayNode* leaf_vars = self->leaf_iter_vars.CopyOnWrite();
  size_t pos = FindLeafVar(all_vars, leaf_vars, parent);

  self->relations.push_back(SplitNode::make(parent, outer, inner, factor));
  // add vars to all vars
  all_vars->data.push_back(outer.node_);
  all_vars->data.push_back(inner.node_);
  // replace the position.
  leaf_vars->data.erase(leaf_vars->data.begin() + pos);
  leaf_vars->data.insert(leaf_vars->data.begin() + pos, inner.node_);
  leaf_vars->data.insert(leaf_vars->data.begin() + pos, outer.node_);
}

}  // namespace

TVM_STATIC_IR_FUNCTOR(IRPrinter, vtable)
.set_dispatch<StageNode>([](const StageNode *op, IRPrinter *p) {
  p->stream << "stage("
            << op->op
            << ")";
});

TVM_STATIC_IR_FUNCTOR(IRPrinter, vtable)
.set_dispatch<IterVarAttrNode>([](const IterVarAttrNode *op, IRPrinter *p) {
    switch (op->iter_type) {
      case kUnrolled: p->stream << "unroll"; break;
      case kVectorized: p->stream << "vectorize"; break;
    }
  });

Stage::Stage(Operation op) {
  auto n = std::make_shared<StageNode>();
  n->op = op;
  n->origin_op = op;
  n->all_iter_vars = op->root_iter_vars();
  n->leaf_iter_vars = n->all_iter_vars;
  node_ = n;
}

Stage& Stage::set_scope(std::string scope) {  // NOLINT(*)
  (*this)->scope = scope;
  return *this;
}

Stage& Stage::compute_at(Stage parent, IterVar scope) {   // NOLINT(*)
  CHECK_NE((*this)->attach_type, kScanUpdate)
      << "Cannot specify compute_at for scan updates";
  (*this)->attach_type = kScope;
  (*this)->attach_ivar = scope;
  (*this)->attach_stage = parent;
  bool found = false;
  for (size_t i = 0; i < parent->leaf_iter_vars.size(); ++i) {
    if (scope == parent->leaf_iter_vars[i]) {
      found = true; break;
    }
  }
  CHECK(found)
      << "Cannot find the axis " << scope
      << " in parent's leaf_iter_vars or outermost_threads:"
      << " parent=" << parent;
  return *this;
}

Stage& Stage::compute_inline() {   // NOLINT(*)
  CHECK_NE((*this)->attach_type, kScanUpdate)
      << "Cannot specify compute_at for scan updates";
  (*this)->attach_type = kInline;
  return *this;
}

Stage& Stage::compute_root() {   // NOLINT(*)
  CHECK_NE((*this)->attach_type, kScanUpdate)
      << "Cannot specify compute_at for scan updates";
  (*this)->attach_type = kRoot;
  return *this;
}

Stage& Stage::split(
    IterVar parent, IterVar* p_outer, IterVar* p_inner, Expr factor) {  // NOLINT(*)
  // place holder for the splitted results.
  IterVar outer(Range(), parent->var->name_hint + ".outer");
  IterVar inner(Range(), parent->var->name_hint + ".inner");
  *p_outer = outer; *p_inner = inner;

  Split(operator->(), parent, outer, inner, factor);
  return *this;
}

Stage& Stage::split(IterVar parent, IterVar outer, IterVar* p_inner, Expr factor) { // NOLINT(*)
  // place holder for the splitted results.
  IterVar inner(Range(), parent->var->name_hint + ".inner");
  *p_inner = inner;
  Split(operator->(), parent, outer, inner, factor);

  return *this;
}

Stage& Stage::fuse(IterVar inner, IterVar outer, IterVar* p_target) {  // NOLINT(*)
  StageNode* self = operator->();
  if (self->attach_type == kScanUpdate) {
    CHECK(!inner.same_as(self->all_iter_vars[0]))
        << "Cannot split on axis[0] of scan update";
    CHECK(!outer.same_as(self->all_iter_vars[0]))
        << "Cannot split on axis[0] of scan update";
  }
  IterVar fused(Range(), outer->var->name_hint + "." + inner->var->name_hint + ".fused");
  *p_target = fused;
  ArrayNode* all_vars = self->all_iter_vars.CopyOnWrite();
  ArrayNode* leaf_vars = self->leaf_iter_vars.CopyOnWrite();

  self->relations.push_back(FuseNode::make(inner, outer, fused));
  all_vars->data.push_back(fused.node_);

  size_t pos_inner = FindLeafVar(all_vars, leaf_vars, inner);
  size_t pos_outer = FindLeafVar(all_vars, leaf_vars, outer);
  CHECK_EQ(pos_inner, pos_outer + 1)
      << "Can only fuse iterations that are consecutive between each other";
  leaf_vars->data.erase(leaf_vars->data.begin() + pos_outer,
                        leaf_vars->data.begin() + pos_inner + 1);
  leaf_vars->data.insert(leaf_vars->data.begin() + pos_outer,
                         fused.node_);
  return *this;
}

Stage& Stage::reorder(const Array<IterVar>& order) {  // NOLINT(*)
  StageNode* self = operator->();
  CHECK(!self->op.as<ScanOpNode>())
      << "Cannot reorder axis of scan";
  ArrayNode* all_vars = self->all_iter_vars.CopyOnWrite();
  ArrayNode* leaf_vars = self->leaf_iter_vars.CopyOnWrite();
  std::vector<size_t> pos;

  for (size_t i = 0; i < order.size(); ++i) {
    if ((*this)->attach_type == kScanUpdate) {
      CHECK(!order[i].same_as(self->all_iter_vars[0]))
          << "Cannot split on axis[0] of scan update";
    }
    pos.push_back(FindLeafVar(all_vars, leaf_vars, order[i]));
  }
  std::vector<std::shared_ptr<Node> > temp;
  for (size_t i = 0; i < pos.size(); ++i) {
    temp.emplace_back(leaf_vars->data[pos[i]]);
  }
  std::sort(pos.begin(), pos.end());
  for (size_t i = 0; i < pos.size(); ++i) {
    leaf_vars->data[pos[i]] = temp[i];
  }
  return *this;
}

Stage& Stage::tile(IterVar x_parent, IterVar y_parent,
                   IterVar* p_x_outer, IterVar* p_y_outer,
                   IterVar* p_x_inner, IterVar* p_y_inner,
                   Expr x_factor, Expr y_factor) { // NOLINT(*)
  split(x_parent, p_x_outer, p_x_inner, x_factor);
  split(y_parent, p_y_outer, p_y_inner, y_factor);
  reorder(Array<IterVar>({*p_x_outer, *p_y_outer, *p_x_inner, *p_y_inner}));
  return *this;
}

Stage& Stage::outermost_threads(Array<IterVar> threads) {
  StageNode* self = operator->();
  CHECK(self->op.as<ScanOpNode>())
      << "outermost_threads is only valid for composite ops such as ScanOp";
  CHECK_EQ(self->outermost_threads.size(), 0U)
      << "Already set outermost_threads";
  ArrayNode* leaf_vars = self->leaf_iter_vars.CopyOnWrite();
  ArrayNode* all_vars = self->all_iter_vars.CopyOnWrite();
  std::vector<std::shared_ptr<Node> > temp;
  for (IterVar iv : threads) {
    temp.push_back(iv.node_);
  }
  leaf_vars->data.insert(
      leaf_vars->data.begin(), temp.begin(), temp.end());
  all_vars->data.insert(
      all_vars->data.end(), temp.begin(), temp.end());
  (*this)->outermost_threads = threads;
  return *this;
}

inline void SetAttr(StageNode* self, IterVar var, IterVarAttr attr) {
  ArrayNode* all_vars = self->all_iter_vars.CopyOnWrite();
  ArrayNode* leaf_vars = self->leaf_iter_vars.CopyOnWrite();
  FindLeafVar(all_vars, leaf_vars, var);
  auto it = self->iter_var_attrs.find(var);
  if (it != self->iter_var_attrs.end()) {
    CHECK_EQ((*it).second->iter_type, attr->iter_type)
        << "IterVar's is already set to "
        << (*it).second << " instead of " << attr;
  } else {
    self->iter_var_attrs.Set(var, attr);
  }
}

Stage& Stage::vectorize(IterVar var) {   // NOLINT(*)
  SetAttr(operator->(), var, IterVarAttr(kVectorized));
  return *this;
}

Stage& Stage::unroll(IterVar var) {   // NOLINT(*)
  SetAttr(operator->(), var, IterVarAttr(kUnrolled));
  return *this;
}

Schedule::Schedule(Array<Operation> ops) {
  auto n = std::make_shared<ScheduleNode>();
  n->outputs = ops;
  auto g = schedule::CreateReadGraph(n->outputs);
  Array<Operation> post_order = schedule::PostDFSOrder(n->outputs, g);
  // output set.
  std::unordered_set<Operation> output_set;
  for (Operation x : ops) {
    output_set.insert(x);
  }
  for (Operation op : post_order) {
    Stage stage(op);
    stage->is_output = output_set.count(op);
    n->stages.push_back(stage);
    n->stage_map.Set(op, stage);
    // mark scan updates.
    if (op.as<ScanOpNode>()) {
      const ScanOpNode* scan = op.as<ScanOpNode>();
      for (size_t i = 0; i < scan->update.size(); ++i) {
        Stage s = n->stage_map[scan->update[i]->op];
        s->attach_type = kScanUpdate;
        s->attach_stage = stage;
      }
    }
  }
  node_ = std::move(n);
}

Stage Schedule::operator[](const Operation& op) {
  auto it = (*this)->stage_map.find(op);
  CHECK(it != (*this)->stage_map.end())
      << "Cannot find Stage for operator " << op
      << " in the schedule";
  return (*it).second;
}

IterVarRelation SplitNode::make(
    IterVar parent, IterVar outer,
    IterVar inner, Expr factor) {
  auto n = std::make_shared<SplitNode>();
  n->parent = parent;
  n->outer = outer;
  n->inner = inner;
  n->factor = factor;
  return IterVarRelation(n);
}

IterVarRelation FuseNode::make(
    IterVar outer, IterVar inner, IterVar fused) {
  auto n = std::make_shared<FuseNode>();
  n->outer = outer;
  n->inner = inner;
  n->fused = fused;
  return IterVarRelation(n);
}

IterVarRelation RebaseNode::make(IterVar parent, IterVar rebased) {
  auto n = std::make_shared<RebaseNode>();
  n->parent = parent;
  n->rebased = rebased;
  return IterVarRelation(n);
}

IterVarAttr::IterVarAttr(IterVarType t) {
  std::shared_ptr<IterVarAttrNode> n = std::make_shared<IterVarAttrNode>();
  n->iter_type = t;
  node_ = n;
}

TVM_REGISTER_NODE_TYPE(StageNode);
TVM_REGISTER_NODE_TYPE(IterVarAttrNode);
TVM_REGISTER_NODE_TYPE(SplitNode);
TVM_REGISTER_NODE_TYPE(FuseNode);
TVM_REGISTER_NODE_TYPE(RebaseNode);
TVM_REGISTER_NODE_TYPE(ScheduleNode);

}  // namespace tvm
