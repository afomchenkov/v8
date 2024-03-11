// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CCTEST_COMPILER_TURBOSHAFT_CODEGEN_TESTER_H_
#define V8_CCTEST_COMPILER_TURBOSHAFT_CODEGEN_TESTER_H_

#include "src/codegen/assembler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/backend/instruction-selector.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/compiler/linkage.h"
#include "src/compiler/pipeline-data-inl.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/turboshaft/assembler.h"
#include "src/compiler/turboshaft/instruction-selection-phase.h"
#include "src/compiler/turboshaft/phase.h"
#include "src/compiler/turboshaft/representations.h"
#include "src/compiler/zone-stats.h"
#include "src/objects/code-inl.h"
#include "test/cctest/cctest.h"
#include "test/common/call-tester.h"

namespace v8::internal::compiler::turboshaft {

class DataHolder {
 public:
  template <typename... ParamMachTypes>
  DataHolder(Isolate* isolate, Zone* zone, MachineType return_type,
             ParamMachTypes... p)
      : isolate_(isolate),
        graph_zone_(zone),
        info_(zone->New<OptimizedCompilationInfo>(base::ArrayVector("testing"),
                                                  zone, CodeKind::FOR_TESTING)),
        zone_stats_(isolate->allocator()),
        pipeline_data_(&zone_stats_, info_, isolate, isolate->allocator(),
                       nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                       AssemblerOptions::Default(isolate), nullptr),
        ts_pipeline_data_(pipeline_data_.GetTurboshaftPipelineData(
            turboshaft::TurboshaftPipelineKind::kJS)),
        descriptor_(Linkage::GetSimplifiedCDescriptor(
            zone, CSignature::New(zone, return_type, p...),
            CallDescriptor::kInitializeRootRegister)) {}

  compiler::PipelineData* pipeline_data() { return &pipeline_data_; }

  PipelineData& ts_pipeline_data() { return ts_pipeline_data_; }

  Isolate* isolate() { return isolate_; }
  Zone* zone() { return graph_zone_; }
  Graph& graph() { return ts_pipeline_data_.graph(); }
  CallDescriptor* call_descriptor() { return descriptor_; }
  OptimizedCompilationInfo* info() { return info_; }

 private:
  Isolate* isolate_;
  Zone* graph_zone_;
  OptimizedCompilationInfo* info_;
  // zone_stats_ must be destroyed after pipeline_data_, so it's declared
  // before.
  ZoneStats zone_stats_;
  compiler::PipelineData pipeline_data_;
  turboshaft::PipelineData& ts_pipeline_data_;
  CallDescriptor* descriptor_;
};

template <typename ReturnType>
class RawMachineAssemblerTester : public HandleAndZoneScope,
                                  public CallHelper<ReturnType>,
                                  public DataHolder,
                                  public TSAssembler<> {
  using Assembler = TSAssembler<>;

 public:
  template <typename... ParamMachTypes>
  explicit RawMachineAssemblerTester(ParamMachTypes... p)
      : HandleAndZoneScope(kCompressGraphZone),
        CallHelper<ReturnType>(
            main_isolate(),
            CSignature::New(main_zone(), MachineTypeForC<ReturnType>(), p...)),
        DataHolder(main_isolate(), main_zone(), MachineTypeForC<ReturnType>(),
                   p...),
        TSAssembler<>(graph(), graph(), zone()),
        data_scope_(ts_pipeline_data()) {
    Init();
  }

  template <typename... ParamMachTypes>
  RawMachineAssemblerTester(CodeKind kind, ParamMachTypes... p)
      : HandleAndZoneScope(kCompressGraphZone),
        CallHelper<ReturnType>(
            main_isolate(),
            CSignature::New(main_zone(), MachineTypeForC<ReturnType>(), p...)),
        DataHolder(main_isolate(), main_zone(), MachineTypeForC<ReturnType>(),
                   p...),
        TSAssembler<>(graph(), graph(), zone()),
        kind_(kind),
        data_scope_(ts_pipeline_data()) {
    Init();
  }

  ~RawMachineAssemblerTester() override = default;

  void CheckNumber(double expected, Tagged<Object> number) {
    CHECK(Object::SameValue(*this->isolate()->factory()->NewNumber(expected),
                            number));
  }

  void CheckString(const char* expected, Tagged<Object> string) {
    CHECK(Object::SameValue(
        *this->isolate()->factory()->InternalizeUtf8String(expected), string));
  }

  void GenerateCode() { Generate(); }

  Handle<Code> GetCode() {
    Generate();
    return code_.ToHandleChecked();
  }

  using CallHelper<ReturnType>::Call;
  using Assembler::Call;

  // A few Assembler helpers.
  using Assembler::Parameter;
  OpIndex Parameter(int i) {
    return Parameter(i, RegisterRepresentation::FromMachineType(
                            call_descriptor()->GetParameterType(i)));
  }
  OpIndex PointerConstant(void* value) {
    return IntPtrConstant(reinterpret_cast<intptr_t>(value));
  }
  OpIndex LoadFromPointer(void* address, MachineType type, int32_t offset = 0) {
#if V8_TARGET_ARCH_ARM64 || V8_TARGET_ARCH_ARM || V8_TARGET_ARCH_RISCV64 || \
    V8_TARGET_ARCH_LOONG64 || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
    // These architectures require a valid Index and no offset.
    return Load(PointerConstant(address), IntPtrConstant(offset),
                LoadOp::Kind::RawAligned(),
                MemoryRepresentation::FromMachineType(type));
#else
    // Otherwise, we can use an offset instead of an Index.
    return Load(PointerConstant(address), LoadOp::Kind::RawAligned(),
                MemoryRepresentation::FromMachineType(type), offset);
#endif
  }
  void StoreToPointer(void* address, MachineRepresentation rep, OpIndex value) {
#if V8_TARGET_ARCH_ARM64 || V8_TARGET_ARCH_ARM || V8_TARGET_ARCH_RISCV64 || \
    V8_TARGET_ARCH_LOONG64 || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
    // These architectures require a valid Index and no offset.
    return Store(PointerConstant(address), IntPtrConstant(0), value,
                 StoreOp::Kind::RawAligned(),
                 MemoryRepresentation::FromMachineRepresentation(rep),
                 WriteBarrierKind::kNoWriteBarrier);
#else
    // Otherwise, we can use an offset instead of an Index.
    return Store(PointerConstant(address), value, StoreOp::Kind::RawAligned(),
                 MemoryRepresentation::FromMachineRepresentation(rep),
                 WriteBarrierKind::kNoWriteBarrier);
#endif
  }
  V<Word32> Int32GreaterThan(V<Word32> a, V<Word32> b) {
    return Int32LessThan(b, a);
  }
  V<Word32> Int32GreaterThanOrEqual(V<Word32> a, V<Word32> b) {
    return Int32LessThanOrEqual(b, a);
  }
  V<Word32> Uint32GreaterThan(V<Word32> a, V<Word32> b) {
    return Uint32LessThan(b, a);
  }
  V<Word32> Uint32GreaterThanOrEqual(V<Word32> a, V<Word32> b) {
    return Uint32LessThanOrEqual(b, a);
  }

 protected:
  Address Generate() override {
    if (code_.is_null()) {
      code_ = Pipeline::GenerateTurboshaftCodeForTesting(
          info(), main_isolate(), call_descriptor(), pipeline_data(),
          AssemblerOptions::Default(main_isolate()));
    }
    return code_.ToHandleChecked()->instruction_start();
  }

 private:
  void Init() {
    // We bind a block right at the start so that the test can start emitting
    // operations without always needing to bind a block first.
    Block* start_block = NewBlock();
    Bind(start_block);
  }

  CodeKind kind_ = CodeKind::FOR_TESTING;
  MaybeHandle<Code> code_;
  turboshaft::PipelineData::Scope data_scope_;
};

template <typename ReturnType>
class BufferedRawMachineAssemblerTester
    : public RawMachineAssemblerTester<int32_t> {
 public:
  template <typename... ParamMachTypes>
  explicit BufferedRawMachineAssemblerTester(ParamMachTypes... p)
      : RawMachineAssemblerTester<int32_t>(
            MachineType::Pointer(), ((void)p, MachineType::Pointer())...),
        test_graph_signature_(
            CSignature::New(this->main_zone(), MachineType::Int32(), p...)),
        return_parameter_index_(sizeof...(p)) {
    static_assert(sizeof...(p) <= arraysize(parameter_nodes_),
                  "increase parameter_nodes_ array");
    std::array<MachineType, sizeof...(p)> p_arr{{p...}};
    for (size_t i = 0; i < p_arr.size(); ++i) {
      parameter_nodes_[i] = Load(p_arr[i], Parameter(i));
    }
  }

  Address Generate() override { return RawMachineAssemblerTester::Generate(); }

  // The BufferedRawMachineAssemblerTester does not pass parameters directly
  // to the constructed IR graph. Instead it passes a pointer to the parameter
  // to the IR graph, and adds Load nodes to the IR graph to load the
  // parameters from memory. Thereby it is possible to pass 64 bit parameters
  // to the IR graph.
  OpIndex Parameter(size_t index) {
    CHECK_GT(arraysize(parameter_nodes_), index);
    return parameter_nodes_[index];
  }

  // The BufferedRawMachineAssemblerTester adds a Store node to the IR graph
  // to store the graph's return value in memory. The memory address for the
  // Store node is provided as a parameter. By storing the return value in
  // memory it is possible to return 64 bit values.
  void Return(OpIndex input) {
    if (COMPRESS_POINTERS_BOOL && MachineTypeForC<ReturnType>().IsTagged()) {
      // Since we are returning values via storing to off-heap location
      // generate full-word store here.
      Store(MachineType::PointerRepresentation(),
            Parameter(return_parameter_index_), BitcastTaggedToWordPtr(input),
            kNoWriteBarrier);

    } else {
      Store(MachineTypeForC<ReturnType>().representation(),
            Parameter(return_parameter_index_), input, kNoWriteBarrier);
    }
    Return(Word32Constant(1234));
  }

  template <typename... Params>
  ReturnType Call(Params... p) {
    uintptr_t zap_data[] = {kZapValue, kZapValue};
    ReturnType return_value;
    static_assert(sizeof(return_value) <= sizeof(zap_data));
    MemCopy(&return_value, &zap_data, sizeof(return_value));
    CSignature::VerifyParams<Params...>(test_graph_signature_);
    CallHelper<int32_t>::Call(reinterpret_cast<void*>(&p)...,
                              reinterpret_cast<void*>(&return_value));
    return return_value;
  }

 private:
  CSignature* test_graph_signature_;
  OpIndex parameter_nodes_[4];
  uint32_t return_parameter_index_;
};

template <>
class BufferedRawMachineAssemblerTester<void>
    : public RawMachineAssemblerTester<void> {
 public:
  template <typename... ParamMachTypes>
  explicit BufferedRawMachineAssemblerTester(ParamMachTypes... p)
      : RawMachineAssemblerTester<void>(((void)p, MachineType::Pointer())...),
        test_graph_signature_(
            CSignature::New(RawMachineAssemblerTester<void>::main_zone(),
                            MachineType::None(), p...)) {
    static_assert(sizeof...(p) <= arraysize(parameter_nodes_),
                  "increase parameter_nodes_ array");
    std::array<MachineType, sizeof...(p)> p_arr{{p...}};
    for (size_t i = 0; i < p_arr.size(); ++i) {
      parameter_nodes_[i] = Load(p_arr[i], Parameter(i));
    }
  }

  Address Generate() override { return RawMachineAssemblerTester::Generate(); }

  // The BufferedRawMachineAssemblerTester does not pass parameters directly
  // to the constructed IR graph. Instead it passes a pointer to the parameter
  // to the IR graph, and adds Load nodes to the IR graph to load the
  // parameters from memory. Thereby it is possible to pass 64 bit parameters
  // to the IR graph.
  OpIndex Parameter(size_t index) {
    CHECK_GT(arraysize(parameter_nodes_), index);
    return parameter_nodes_[index];
  }

  template <typename... Params>
  void Call(Params... p) {
    CSignature::VerifyParams<Params...>(test_graph_signature_);
    CallHelper<void>::Call(reinterpret_cast<void*>(&p)...);
  }

 private:
  CSignature* test_graph_signature_;
  OpIndex parameter_nodes_[4];
};

static const bool USE_RESULT_BUFFER = true;
static const bool USE_RETURN_REGISTER = false;
static const int32_t CHECK_VALUE = 0x99BEEDCE;

// TODO(titzer): use the C-style calling convention, or any register-based
// calling convention for binop tests.
template <typename CType, bool use_result_buffer>
class BinopTester {
 public:
  explicit BinopTester(RawMachineAssemblerTester<int32_t>* tester,
                       MachineType type)
      : T(tester),
        param0(T->LoadFromPointer(&p0, type)),
        param1(T->LoadFromPointer(&p1, type)),
        type(type),
        p0(static_cast<CType>(0)),
        p1(static_cast<CType>(0)),
        result(static_cast<CType>(0)) {}

  RawMachineAssemblerTester<int32_t>* T;
  OpIndex param0;
  OpIndex param1;

  CType call(CType a0, CType a1) {
    p0 = a0;
    p1 = a1;
    if (use_result_buffer) {
      CHECK_EQ(CHECK_VALUE, T->Call());
      return result;
    } else {
      return static_cast<CType>(T->Call());
    }
  }

  void AddReturn(OpIndex val) {
    if (use_result_buffer) {
      T->Store(type.representation(), T->PointerConstant(&result),
               T->Word32Constant(0), val, kNoWriteBarrier);
      T->Return(T->Word32Constant(CHECK_VALUE));
    } else {
      T->Return(val);
    }
  }

  template <typename Ci, typename Cj, typename Fn>
  void Run(const Ci& ci, const Cj& cj, const Fn& fn) {
    typename Ci::const_iterator i;
    typename Cj::const_iterator j;
    for (i = ci.begin(); i != ci.end(); ++i) {
      for (j = cj.begin(); j != cj.end(); ++j) {
        CHECK_EQ(fn(*i, *j), this->call(*i, *j));
      }
    }
  }

 protected:
  MachineType type;
  CType p0;
  CType p1;
  CType result;
};

// A helper class for testing code sequences that take two int parameters and
// return an int value.
class Int32BinopTester : public BinopTester<int32_t, USE_RETURN_REGISTER> {
 public:
  explicit Int32BinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<int32_t, USE_RETURN_REGISTER>(tester,
                                                  MachineType::Int32()) {}
};

// A helper class for testing code sequences that take two int parameters and
// return an int value.
class Int64BinopTester : public BinopTester<int64_t, USE_RETURN_REGISTER> {
 public:
  explicit Int64BinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<int64_t, USE_RETURN_REGISTER>(tester,
                                                  MachineType::Int64()) {}
};

// A helper class for testing code sequences that take two uint parameters and
// return an uint value.
class Uint32BinopTester : public BinopTester<uint32_t, USE_RETURN_REGISTER> {
 public:
  explicit Uint32BinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<uint32_t, USE_RETURN_REGISTER>(tester,
                                                   MachineType::Uint32()) {}

  uint32_t call(uint32_t a0, uint32_t a1) {
    p0 = a0;
    p1 = a1;
    return static_cast<uint32_t>(T->Call());
  }
};

// A helper class for testing code sequences that take two float parameters and
// return a float value.
class Float32BinopTester : public BinopTester<float, USE_RESULT_BUFFER> {
 public:
  explicit Float32BinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<float, USE_RESULT_BUFFER>(tester, MachineType::Float32()) {}
};

// A helper class for testing code sequences that take two double parameters and
// return a double value.
class Float64BinopTester : public BinopTester<double, USE_RESULT_BUFFER> {
 public:
  explicit Float64BinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<double, USE_RESULT_BUFFER>(tester, MachineType::Float64()) {
  }
};

// A helper class for testing code sequences that take two pointer parameters
// and return a pointer value.
// TODO(titzer): pick word size of pointers based on V8_TARGET.
template <typename Type>
class PointerBinopTester : public BinopTester<Type, USE_RETURN_REGISTER> {
 public:
  explicit PointerBinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<Type, USE_RETURN_REGISTER>(tester, MachineType::Pointer()) {
  }
};

// A helper class for testing code sequences that take two tagged parameters and
// return a tagged value.
template <typename Type>
class TaggedBinopTester : public BinopTester<Type, USE_RETURN_REGISTER> {
 public:
  explicit TaggedBinopTester(RawMachineAssemblerTester<int32_t>* tester)
      : BinopTester<Type, USE_RETURN_REGISTER>(tester,
                                               MachineType::AnyTagged()) {}
};

#if 0

#define BINOP_LIST(V) \
  V(Word32Add)        \
  V(Word32Sub)        \
  V(Word32Mul)        \
  V(Word32And)        \
  V(Word32Or)         \
  V(Word32Xor)        \
  V(Word64Add)        \
  V(Word64Sub)        \
  V(Word64Mul)        \
  V(Word64And)        \
  V(Word64Or)         \
  V(Word64Xor)

enum class TurboshaftBinop {
#define DEF(kind) k##kind,
  BINOP_LIST(DEF)
#undef DEF
};

// A helper class for integer binary operations. Wraps a machine opcode and
// provides evaluation routines and the operators.
template <typename T>
class IntBinopWrapper {
 public:
  explicit IntBinopWrapper(TurboshaftBinop kind) : kind(kind) {}

  const Operator* op(MachineOperatorBuilder* machine) const {
    switch (opcode) {
      case IrOpcode::kInt32Add:
        return machine->Int32Add();
      case IrOpcode::kInt32Sub:
        return machine->Int32Sub();
      case IrOpcode::kInt32Mul:
        return machine->Int32Mul();
      case IrOpcode::kWord32And:
        return machine->Word32And();
      case IrOpcode::kWord32Or:
        return machine->Word32Or();
      case IrOpcode::kWord32Xor:
        return machine->Word32Xor();
      case IrOpcode::kInt64Add:
        return machine->Int64Add();
      case IrOpcode::kInt64Sub:
        return machine->Int64Sub();
      case IrOpcode::kInt64Mul:
        return machine->Int64Mul();
      case IrOpcode::kWord64And:
        return machine->Word64And();
      case IrOpcode::kWord64Or:
        return machine->Word64Or();
      case IrOpcode::kWord64Xor:
        return machine->Word64Xor();
      default:
        UNREACHABLE();
    }
  }

  T eval(T a, T b) const {
    switch (kind) {
      case TurboshaftBinop::kWord32Add:
      case TurboshaftBinop::kWord64Add:
        return a + b;
      case TurboshaftBinop::kWord32Sub:
      case TurboshaftBinop::kWord64Sub:
        return a - b;
      case TurboshaftBinop::kWord32Mul:
      case TurboshaftBinop::kWord64Mul:
        return a * b;
      case TurboshaftBinop::kWord32And:
      case TurboshaftBinop::kWord64And:
        return a & b;
      case TurboshaftBinop::kWord32Or:
      case TurboshaftBinop::kWord64Or:
        return a | b;
      case TurboshaftBinop::kWord32Xor:
      case TurboshaftBinop::kWord64Xor:
        return a ^ b;
    }
  }
  TurboshaftBinop kind;
};

#endif

#define COMPARE_LIST(V)    \
  V(Word32Equal)           \
  V(Int32LessThan)         \
  V(Int32LessThanOrEqual)  \
  V(Uint32LessThan)        \
  V(Uint32LessThanOrEqual) \
  V(Word64Equal)           \
  V(Int64LessThan)         \
  V(Int64LessThanOrEqual)  \
  V(Uint64LessThan)        \
  V(Uint64LessThanOrEqual) \
  V(Float64Equal)          \
  V(Float64LessThan)       \
  V(Float64LessThanOrEqual)

enum class TurboshaftComparison {
#define DEF(kind) k##kind,
  COMPARE_LIST(DEF)
#undef DEF
};

// A helper class for testing compares. Wraps a machine opcode and provides
// evaluation routines and the operators.
class CompareWrapper {
 public:
  explicit CompareWrapper(TurboshaftComparison op) : op(op) {}

  V<Word32> MakeNode(RawMachineAssemblerTester<int32_t>* m, OpIndex a,
                     OpIndex b) {
    switch (op) {
#define CASE(kind)                    \
  case TurboshaftComparison::k##kind: \
    return m->kind(a, b);
      COMPARE_LIST(CASE)
#undef CASE
    }
  }

  bool Int32Compare(int32_t a, int32_t b) const {
    switch (op) {
      case TurboshaftComparison::kWord32Equal:
        return a == b;
      case TurboshaftComparison::kInt32LessThan:
        return a < b;
      case TurboshaftComparison::kInt32LessThanOrEqual:
        return a <= b;
      case TurboshaftComparison::kUint32LessThan:
        return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
      case TurboshaftComparison::kUint32LessThanOrEqual:
        return static_cast<uint32_t>(a) <= static_cast<uint32_t>(b);
      default:
        UNREACHABLE();
    }
  }

  bool Int64Compare(int64_t a, int64_t b) const {
    switch (op) {
      case TurboshaftComparison::kWord64Equal:
        return a == b;
      case TurboshaftComparison::kInt64LessThan:
        return a < b;
      case TurboshaftComparison::kInt64LessThanOrEqual:
        return a <= b;
      case TurboshaftComparison::kUint64LessThan:
        return static_cast<uint64_t>(a) < static_cast<uint64_t>(b);
      case TurboshaftComparison::kUint64LessThanOrEqual:
        return static_cast<uint64_t>(a) <= static_cast<uint64_t>(b);
      default:
        UNREACHABLE();
    }
  }

  bool Float64Compare(double a, double b) const {
    switch (op) {
      case TurboshaftComparison::kFloat64Equal:
        return a == b;
      case TurboshaftComparison::kFloat64LessThan:
        return a < b;
      case TurboshaftComparison::kFloat64LessThanOrEqual:
        return a <= b;
      default:
        UNREACHABLE();
    }
  }

  TurboshaftComparison op;
};

// A small closure class to generate code for a function of two inputs that
// produces a single output so that it can be used in many different contexts.
// The {expected()} method should compute the expected output for a given
// pair of inputs.
template <typename T>
class BinopGen {
 public:
  virtual void gen(RawMachineAssemblerTester<int32_t>* m, OpIndex a,
                   OpIndex b) = 0;
  virtual T expected(T a, T b) = 0;
  virtual ~BinopGen() = default;
};

// A helper class to generate various combination of input shape combinations
// and run the generated code to ensure it produces the correct results.
class Int32BinopInputShapeTester {
 public:
  explicit Int32BinopInputShapeTester(BinopGen<int32_t>* g)
      : gen(g), input_a(0), input_b(0) {}

  void TestAllInputShapes();

 private:
  BinopGen<int32_t>* gen;
  int32_t input_a;
  int32_t input_b;

  void Run(RawMachineAssemblerTester<int32_t>* m);
  void RunLeft(RawMachineAssemblerTester<int32_t>* m);
  void RunRight(RawMachineAssemblerTester<int32_t>* m);
};

}  // namespace v8::internal::compiler::turboshaft

#endif  // V8_CCTEST_COMPILER_TURBOSHAFT_CODEGEN_TESTER_H_