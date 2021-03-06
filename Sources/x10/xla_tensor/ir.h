/*
 * Copyright 2020 TensorFlow Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "tensorflow/compiler/tf2xla/xla_tensor/aten_compat.h"
#include "tensorflow/compiler/tf2xla/xla_tensor/swift_backtrace.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/xla_client/types.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"

namespace swift_xla {
namespace ir {

class Node;
class LoweringContext;

using NodePtr = std::shared_ptr<Node>;

using XlaOpVector = tensorflow::gtl::InlinedVector<xla::XlaOp, 1>;

// The base class for user defined metadata which is possible to attach to IR
// nodes.
struct UserMetaData {
  virtual ~UserMetaData() {}
};

struct MetaData {
  std::string scope;
  std::vector<SourceLocation> frame_info;
};

// Represents a specific output produced by a node. Since the output of a node
// can be composed by multiple outputs, the node+index coordinates fully qualify
// each single output.
struct Output {
  struct Hasher {
    size_t operator()(const Output& output) const;
  };

  Output() = default;
  explicit Output(const Node* node, size_t index = 0)
      : node(node), index(index) {}

  // Retrieves the shape of this output. If the IR Node generating the value is
  // a multi-output node, the shape returned by this API will not be the full
  // tuple shape, but only the shape at index referred by this value.
  // To retrieve the full tuple shape in that case, use the node_shape() API.
  const xla::Shape& shape() const;
  const xla::Shape& node_shape() const;

  xla::hash_t hash() const;

  bool operator==(const Output& rhs) const {
    return node == rhs.node && index == rhs.index;
  }
  bool operator!=(const Output& rhs) const { return !operator==(rhs); }

  std::string ToString() const;

  // The node providing the output.
  const Node* node = nullptr;
  // The index in the node's output this output refers to.
  size_t index = 0;
};

inline std::ostream& operator<<(std::ostream& stream, const Output& output) {
  stream << output.ToString();
  return stream;
}

using OutputSet = std::unordered_set<Output, Output::Hasher>;

template <typename T>
using OutputMap = std::unordered_map<Output, T, Output::Hasher>;

// Represents an input/operand for a Node object.
struct Value {
  Value() = default;
  Value(NodePtr node, size_t index = 0) : node(std::move(node)), index(index) {}

  // Retrieves the shape of this value. If the IR Node generating the value is a
  // multi-output node, the shape returned by this API will not be the full
  // tuple shape, but only the shape at index referred by this value.
  // To retrieve the full tuple shape in that case, use the node_shape() API.
  const xla::Shape& shape() const;
  const xla::Shape& node_shape() const;

  xla::hash_t hash() const;

  operator bool() const { return node != nullptr; }

  operator Output() const { return Output(node.get(), index); }

  Node* operator->() const { return node.get(); }

  NodePtr node;
  size_t index = 0;
};

// The Kind of operation a Node can be associated to.
struct OpKind {
  OpKind() = default;
  explicit OpKind(c10::Symbol op) : op(std::move(op)) {}

  bool operator==(const OpKind& rhs) const { return op == rhs.op; }
  bool operator!=(const OpKind& rhs) const { return !operator==(rhs); }
  bool operator<(const OpKind& rhs) const {
    return c10::unique_t(op) < c10::unique_t(rhs.op);
  }

  xla::hash_t hash() const;

  std::string ToString() const { return op.toQualString(); }

  // Retrieves an existing operation object, or creates a new one. Operations
  // that are specific to the XLA side, should live within the 'xla::'
  // namespace.
  static OpKind Get(const std::string& name);

  c10::Symbol op;
};

inline std::ostream& operator<<(std::ostream& stream, const OpKind& op) {
  stream << op.ToString();
  return stream;
}

using OpList = absl::Span<const Value>;

// A node in the graph. Nodes for operations which requires extra data to be
// stored for lowering, should inherit from this class and add operation
// specific member there. For example, a constant might create a new
// NodeConstant class (inheriting from Node) with an extra xla::Literal field,
// or a tensor value might create a new NodeTensor with computation client data
// handle in it.
class Node {
 public:
  // Creates a new node with the given op name. The op is a unique identifier
  // for the operation. The num_outputs tells how many outputs a given operation
  // generates.
  Node(OpKind op, OpList operands, xla::Shape shape, size_t num_outputs = 1,
       xla::hash_t hash_seed = 0x5a2d296e9);

  // Same as the constructor above, but the shape is generated by a function,
  // only if needed (shape cache miss).
  Node(OpKind op, OpList operands, const std::function<xla::Shape()>& shape_fn,
       size_t num_outputs = 1, xla::hash_t hash_seed = 0x5a2d296e9);

  // Contructor used to create leaf nodes.
  Node(OpKind op, xla::Shape shape, size_t num_outputs, xla::hash_t hash_seed);

  virtual ~Node() {}

  const OpKind& op() const { return op_; }

  size_t num_outputs() const { return num_outputs_; }

  // Retrieves the full shape of the IR Node. Note that if this is a
  // multi-output node, the returned shape will be a tuple.
  const xla::Shape& shape() const { return shape_; }

  // Retrieves the shape of the output at a given index. If the node is not a
  // multi-output node, output_index must be zero.
  const xla::Shape& shape(size_t output_index) const;

  const absl::InlinedVector<Output, 4>& operands() const {
    return operands_as_outputs_;
  }

  const absl::InlinedVector<NodePtr, 4>& operand_nodes() const {
    return operands_;
  }

  const Output& operand(size_t i) const { return operands_as_outputs_.at(i); }

  xla::hash_t node_hash() const { return node_hash_; }

  xla::hash_t hash() const { return hash_; }

  const MetaData& metadata() const { return metadata_; }

  virtual std::string ToString() const;

  virtual NodePtr Clone(OpList operands) const;

  virtual XlaOpVector Lower(LoweringContext* loctx) const;

  XlaOpVector ReturnOp(xla::XlaOp op, LoweringContext* loctx) const;

  XlaOpVector ReturnOps(absl::Span<const xla::XlaOp> ops,
                        LoweringContext* loctx) const;

 private:
  // Adds node's index output number as operand.
  void AddOperand(NodePtr node, size_t index = 0);

  xla::Shape GetOpShape(const std::function<xla::Shape()>& shape_fn) const;

  static xla::hash_t GetOpHash(OpKind op, const xla::Shape& shape,
                               xla::hash_t hash_seed);

  // The ID of the operation captured by this node.
  OpKind op_;
  size_t num_outputs_ = 1;
  xla::Shape shape_;
  // A node holds a real reference to its operands.
  absl::InlinedVector<NodePtr, 4> operands_;
  // Outputs do not hold references on the nodes, and neither do the uses, since
  // otherwise we get into circular reference counting.
  absl::InlinedVector<Output, 4> operands_as_outputs_;
  // The hash value of this node.
  xla::hash_t node_hash_ = 0;
  // The hash value of the graph rooted at this node.
  xla::hash_t hash_ = 0;
  // The IR specific metadata attached to the IR node.
  MetaData metadata_;

 public:
  static bool s_log_graph_changes_;
};

// RAII data structure to be used a stack variable to enter a new IR scope. IR
// scope names will appear in the IR and will help identifying the source of the
// single IR nodes.
struct ScopePusher {
  explicit ScopePusher(const std::string& name);
  ~ScopePusher();

  static void ResetScopes();
};

inline std::ostream& operator<<(std::ostream& stream, const Node& node) {
  stream << node.ToString();
  return stream;
}

template <typename T, typename... Args>
NodePtr MakeNode(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

template <typename T>
T* NodeCast(const Node* node, OpKind op) {
  if (op != node->op()) {
    return nullptr;
  }
  const T* casted;
#ifdef NDEBUG
  casted = static_cast<const T*>(node);
#else
  casted = &dynamic_cast<const T&>(*node);
#endif
  return const_cast<T*>(casted);
}

}  // namespace ir
}  // namespace swift_xla
