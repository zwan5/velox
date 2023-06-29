/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
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
#include "velox/exec/Aggregate.h"
#include "velox/functions/prestosql/aggregates/AddressableNonNullValueList.h"
#include "velox/functions/prestosql/aggregates/AggregateNames.h"
#include "velox/functions/prestosql/aggregates/Strings.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::aggregate::prestosql {

namespace {

/// Maintains a set of unique values. Non-null values are stored in F14FastSet.
/// A separate flag tracks presence of the null value.
template <
    typename T,
    typename Hash = std::hash<T>,
    typename EqualTo = std::equal_to<T>>
struct Accumulator {
  bool hasNull{false};
  folly::F14FastSet<T, Hash, EqualTo, AlignedStlAllocator<T, 16>> uniqueValues;

  Accumulator(const TypePtr& /*type*/, HashStringAllocator* allocator)
      : uniqueValues{AlignedStlAllocator<T, 16>(allocator)} {}

  Accumulator(Hash hash, EqualTo equalTo, HashStringAllocator* allocator)
      : uniqueValues{0, hash, equalTo, AlignedStlAllocator<T, 16>(allocator)} {}

  /// Adds value if new. No-op if the value was added before.
  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* /*allocator*/) {
    if (decoded.isNullAt(index)) {
      hasNull = true;
    } else {
      uniqueValues.insert(decoded.valueAt<T>(index));
    }
  }

  /// Adds new values from an array.
  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  /// Returns number of unique values including null.
  size_t size() const {
    return uniqueValues.size() + (hasNull ? 1 : 0);
  }

  /// Copies the unique values and null into the specified vector starting at
  /// the specified offset.
  vector_size_t extractValues(FlatVector<T>& values, vector_size_t offset) {
    vector_size_t index = offset;
    for (auto value : uniqueValues) {
      values.set(index++, value);
    }

    if (hasNull) {
      values.setNull(index++, true);
    }

    return index - offset;
  }
};

/// Maintains a set of unique strings.
struct StringViewAccumulator {
  /// A set of unique StringViews pointing to storage managed by 'strings'.
  Accumulator<StringView> base;

  /// Stores unique non-null non-inline strings.
  Strings strings;

  StringViewAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{type, allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    if (decoded.isNullAt(index)) {
      base.hasNull = true;
    } else {
      auto value = decoded.valueAt<StringView>(index);
      if (!value.isInline()) {
        if (base.uniqueValues.contains(value)) {
          return;
        }
        value = strings.append(value, *allocator);
      }
      base.uniqueValues.insert(value);
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(
      FlatVector<StringView>& values,
      vector_size_t offset) {
    return base.extractValues(values, offset);
  }
};

/// Maintains a set of unique arrays, maps or structs.
struct ComplexTypeAccumulator {
  /// A set of pointers to values stored in AddressableNonNullValueList.
  Accumulator<
      HashStringAllocator::Position,
      AddressableNonNullValueList::Hash,
      AddressableNonNullValueList::EqualTo>
      base;

  /// Stores unique non-null values.
  AddressableNonNullValueList values;

  ComplexTypeAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{
            AddressableNonNullValueList::Hash{},
            AddressableNonNullValueList::EqualTo{type},
            allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    if (decoded.isNullAt(index)) {
      base.hasNull = true;
    } else {
      auto position = values.append(decoded, index, allocator);

      if (!base.uniqueValues.insert(position).second) {
        values.removeLast(position);
      }
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(BaseVector& values, vector_size_t offset) {
    vector_size_t index = offset;
    for (const auto& position : base.uniqueValues) {
      AddressableNonNullValueList::read(position, values, index++);
    }

    if (base.hasNull) {
      values.setNull(index++, true);
    }

    return index - offset;
  }
};

template <typename T>
struct AccumulatorTypeTraits {
  using AccumulatorType = Accumulator<T>;
};

template <>
struct AccumulatorTypeTraits<StringView> {
  using AccumulatorType = StringViewAccumulator;
};

template <>
struct AccumulatorTypeTraits<ComplexType> {
  using AccumulatorType = ComplexTypeAccumulator;
};

template <typename T>
class SetBaseAggregate : public exec::Aggregate {
 public:
  explicit SetBaseAggregate(const TypePtr& resultType)
      : exec::Aggregate(resultType) {}

  using AccumulatorType = typename AccumulatorTypeTraits<T>::AccumulatorType;

  int32_t accumulatorFixedWidthSize() const override {
    return sizeof(AccumulatorType);
  }

  bool isFixedSize() const override {
    return false;
  }

  void initializeNewGroups(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    const auto& type = resultType()->childAt(0);
    exec::Aggregate::setAllNulls(groups, indices);
    for (auto i : indices) {
      new (groups[i] + offset_) AccumulatorType(type, allocator_);
    }
  }

  void extractValues(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    auto arrayVector = (*result)->as<ArrayVector>();
    arrayVector->resize(numGroups);

    auto* rawOffsets = arrayVector->offsets()->asMutable<vector_size_t>();
    auto* rawSizes = arrayVector->sizes()->asMutable<vector_size_t>();

    vector_size_t numValues = 0;
    uint64_t* rawNulls = getRawNulls(arrayVector);
    for (auto i = 0; i < numGroups; ++i) {
      auto* group = groups[i];
      if (isNull(group)) {
        arrayVector->setNull(i, true);
      } else {
        clearNull(rawNulls, i);

        const auto size = value(group)->size();

        rawOffsets[i] = numValues;
        rawSizes[i] = size;

        numValues += size;
      }
    }

    if constexpr (std::is_same_v<T, ComplexType>) {
      auto values = arrayVector->elements();
      values->resize(numValues);

      vector_size_t offset = 0;
      for (auto i = 0; i < numGroups; ++i) {
        auto* group = groups[i];
        if (!isNull(group)) {
          offset += value(group)->extractValues(*values, offset);
        }
      }
    } else {
      auto values = arrayVector->elements()->as<FlatVector<T>>();
      values->resize(numValues);

      vector_size_t offset = 0;
      for (auto i = 0; i < numGroups; ++i) {
        auto* group = groups[i];
        if (!isNull(group)) {
          offset += value(group)->extractValues(*values, offset);
        }
      }
    }
  }

  void extractAccumulators(char** groups, int32_t numGroups, VectorPtr* result)
      override {
    return extractValues(groups, numGroups, result);
  }

  void addIntermediateResults(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    decoded_.decode(*args[0], rows);

    auto baseArray = decoded_.base()->template as<ArrayVector>();
    decodedElements_.decode(*baseArray->elements());

    rows.applyToSelected([&](vector_size_t i) {
      if (decoded_.isNullAt(i)) {
        return;
      }

      auto* group = groups[i];
      clearNull(group);

      auto tracker = trackRowSize(group);

      auto decodedIndex = decoded_.index(i);
      value(group)->addValues(
          *baseArray, decodedIndex, decodedElements_, allocator_);
    });
  }

  void addSingleGroupIntermediateResults(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    decoded_.decode(*args[0], rows);

    auto baseArray = decoded_.base()->template as<ArrayVector>();

    decodedElements_.decode(*baseArray->elements());

    auto* accumulator = value(group);

    auto tracker = trackRowSize(group);
    rows.applyToSelected([&](vector_size_t i) {
      if (decoded_.isNullAt(i)) {
        return;
      }

      clearNull(group);

      auto decodedIndex = decoded_.index(i);
      accumulator->addValues(
          *baseArray, decodedIndex, decodedElements_, allocator_);
    });
  }

  void destroy(folly::Range<char**> groups) override {
    if constexpr (std::is_same_v<T, StringView>) {
      for (auto* group : groups) {
        if (!isNull(group)) {
          value(group)->strings.free(*allocator_);
        }
      }
    }

    if constexpr (std::is_same_v<T, ComplexType>) {
      for (auto* group : groups) {
        if (!isNull(group)) {
          value(group)->values.free(*allocator_);
        }
      }
    }
  }

 protected:
  inline AccumulatorType* value(char* group) {
    return reinterpret_cast<AccumulatorType*>(group + Aggregate::offset_);
  }

  DecodedVector decoded_;
  DecodedVector decodedElements_;
};

template <typename T>
class SetAggAggregate : public SetBaseAggregate<T> {
 public:
  explicit SetAggAggregate(const TypePtr& resultType)
      : SetBaseAggregate<T>(resultType) {}

  using Base = SetBaseAggregate<T>;

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    Base::decoded_.decode(*args[0], rows);

    rows.applyToSelected([&](vector_size_t i) {
      auto* group = groups[i];
      Base::clearNull(group);

      auto tracker = Base::trackRowSize(group);
      Base::value(group)->addValue(Base::decoded_, i, Base::allocator_);
    });
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool /*mayPushdown*/) override {
    Base::decoded_.decode(*args[0], rows);

    Base::clearNull(group);
    auto* accumulator = Base::value(group);

    auto tracker = Base::trackRowSize(group);
    rows.applyToSelected([&](vector_size_t i) {
      accumulator->addValue(Base::decoded_, i, Base::allocator_);
    });
  }
};

template <typename T>
class SetUnionAggregate : public SetBaseAggregate<T> {
 public:
  explicit SetUnionAggregate(const TypePtr& resultType)
      : SetBaseAggregate<T>(resultType) {}

  using Base = SetBaseAggregate<T>;

  void addRawInput(
      char** groups,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    Base::addIntermediateResults(groups, rows, args, mayPushdown);
  }

  void addSingleGroupRawInput(
      char* group,
      const SelectivityVector& rows,
      const std::vector<VectorPtr>& args,
      bool mayPushdown) override {
    Base::addSingleGroupIntermediateResults(group, rows, args, mayPushdown);
  }
};

template <template <typename T> class Aggregate>
std::unique_ptr<exec::Aggregate> create(
    TypeKind typeKind,
    const TypePtr& resultType) {
  switch (typeKind) {
    case TypeKind::BOOLEAN:
      return std::make_unique<Aggregate<bool>>(resultType);
    case TypeKind::TINYINT:
      return std::make_unique<Aggregate<int8_t>>(resultType);
    case TypeKind::SMALLINT:
      return std::make_unique<Aggregate<int16_t>>(resultType);
    case TypeKind::INTEGER:
      return std::make_unique<Aggregate<int32_t>>(resultType);
    case TypeKind::BIGINT:
      return std::make_unique<Aggregate<int64_t>>(resultType);
    case TypeKind::REAL:
      return std::make_unique<Aggregate<float>>(resultType);
    case TypeKind::DOUBLE:
      return std::make_unique<Aggregate<double>>(resultType);
    case TypeKind::TIMESTAMP:
      return std::make_unique<Aggregate<Timestamp>>(resultType);
    case TypeKind::DATE:
      return std::make_unique<Aggregate<Date>>(resultType);
    case TypeKind::VARCHAR:
      return std::make_unique<Aggregate<StringView>>(resultType);
    case TypeKind::ARRAY:
    case TypeKind::MAP:
    case TypeKind::ROW:
      return std::make_unique<Aggregate<ComplexType>>(resultType);
    default:
      VELOX_UNREACHABLE("Unexpected type {}", mapTypeKindToName(typeKind));
  }
}

exec::AggregateRegistrationResult registerSetAgg(const std::string& name) {
  std::vector<std::shared_ptr<exec::AggregateFunctionSignature>> signatures = {
      exec::AggregateFunctionSignatureBuilder()
          .typeVariable("T")
          .returnType("array(T)")
          .intermediateType("array(T)")
          .argumentType("T")
          .build()};

  return exec::registerAggregateFunction(
      name,
      std::move(signatures),
      [name](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType) -> std::unique_ptr<exec::Aggregate> {
        VELOX_CHECK_EQ(argTypes.size(), 1);

        const TypeKind typeKind = exec::isRawInput(step)
            ? argTypes[0]->kind()
            : argTypes[0]->childAt(0)->kind();

        return create<SetAggAggregate>(typeKind, resultType);
      });
}

exec::AggregateRegistrationResult registerSetUnion(const std::string& name) {
  std::vector<std::shared_ptr<exec::AggregateFunctionSignature>> signatures = {
      exec::AggregateFunctionSignatureBuilder()
          .typeVariable("T")
          .returnType("array(T)")
          .intermediateType("array(T)")
          .argumentType("array(T)")
          .build()};

  return exec::registerAggregateFunction(
      name,
      std::move(signatures),
      [name](
          core::AggregationNode::Step /*step*/,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType) -> std::unique_ptr<exec::Aggregate> {
        VELOX_CHECK_EQ(argTypes.size(), 1);

        const TypeKind typeKind = argTypes[0]->childAt(0)->kind();

        return create<SetUnionAggregate>(typeKind, resultType);
      });
}

} // namespace

void registerSetAggAggregate(const std::string& prefix) {
  registerSetAgg(prefix + kSetAgg);
}

void registerSetUnionAggregate(const std::string& prefix) {
  registerSetUnion(prefix + kSetUnion);
}

} // namespace facebook::velox::aggregate::prestosql
