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

#include <folly/Math.h>
#include <re2/re2.h>

#include "folly/experimental/EventCount.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/file/FileSystems.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/dwio/common/tests/utils/BatchMaker.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/HashAggregation.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/RowContainer.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/SumNonPODAggregate.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

namespace facebook::velox::exec::test {

using core::QueryConfig;
using facebook::velox::test::BatchMaker;
using namespace common::testutil;

/// No-op implementation of Aggregate. Provides public access to following
/// base class methods: setNull, clearNull and isNull.
class AggregateFunc : public Aggregate {
 public:
  explicit AggregateFunc(TypePtr resultType) : Aggregate(resultType) {}

  int32_t accumulatorFixedWidthSize() const override {
    return 0;
  }

  bool setNullTest(char* group) {
    return Aggregate::setNull(group);
  }

  bool clearNullTest(char* group) {
    return Aggregate::clearNull(group);
  }

  bool isNullTest(char* group) const {
    return Aggregate::isNull(group);
  }

  void initializeNewGroups(
      char** /*groups*/,
      folly::Range<const vector_size_t*> /*indices*/) override {}

  void addRawInput(
      char** /*groups*/,
      const SelectivityVector& /*rows*/,
      const std::vector<VectorPtr>& /*args*/,
      bool /*mayPushdown*/) override {}

  void extractValues(
      char** /*groups*/,
      int32_t /*numGroups*/,
      VectorPtr* /*result*/) override {}

  void addIntermediateResults(
      char** /*groups*/,
      const SelectivityVector& /*rows*/,
      const std::vector<VectorPtr>& /*args*/,
      bool /*mayPushdown*/) override {}

  void addSingleGroupRawInput(
      char* /*group*/,
      const SelectivityVector& /*rows*/,
      const std::vector<VectorPtr>& /*args*/,
      bool /*mayPushdown*/) override {}

  void addSingleGroupIntermediateResults(
      char* /*group*/,
      const SelectivityVector& /*rows*/,
      const std::vector<VectorPtr>& /*args*/,
      bool /*mayPushdown*/) override {}

  void extractAccumulators(
      char** /*groups*/,
      int32_t /*numGroups*/,
      VectorPtr* /*result*/) override {}
};

class AggregationTest : public OperatorTestBase {
 protected:
  static void SetUpTestCase() {
    OperatorTestBase::SetUpTestCase();
    TestValue::enable();
  }

  void SetUp() override {
    OperatorTestBase::SetUp();
    filesystems::registerLocalFileSystem();
    registerSumNonPODAggregate("sumnonpod", 64);
  }

  std::vector<RowVectorPtr>
  makeVectors(const RowTypePtr& rowType, vector_size_t size, int numVectors) {
    std::vector<RowVectorPtr> vectors;
    for (int32_t i = 0; i < numVectors; ++i) {
      auto vector = std::dynamic_pointer_cast<RowVector>(
          velox::test::BatchMaker::createBatch(rowType, size, *pool_));
      vectors.push_back(vector);
    }
    return vectors;
  }

  template <typename T>
  void testSingleKey(
      const std::vector<RowVectorPtr>& vectors,
      const std::string& keyName,
      bool ignoreNullKeys,
      bool distinct) {
    NonPODInt64::clearStats();
    std::vector<std::string> aggregates;
    if (!distinct) {
      aggregates = {
          "sum(15)", "sum(0.1)", "sum(c1)",     "sum(c2)", "sum(c4)", "sum(c5)",
          "min(15)", "min(0.1)", "min(c1)",     "min(c2)", "min(c3)", "min(c4)",
          "min(c5)", "max(15)",  "max(0.1)",    "max(c1)", "max(c2)", "max(c3)",
          "max(c4)", "max(c5)",  "sumnonpod(1)"};
    }

    auto op = PlanBuilder()
                  .values(vectors)
                  .aggregation(
                      {keyName},
                      aggregates,
                      {},
                      core::AggregationNode::Step::kPartial,
                      ignoreNullKeys)
                  .planNode();

    std::string fromClause = "FROM tmp";
    if (ignoreNullKeys) {
      fromClause += " WHERE " + keyName + " IS NOT NULL";
    }
    if (distinct) {
      assertQuery(op, "SELECT distinct " + keyName + " " + fromClause);
    } else {
      assertQuery(
          op,
          "SELECT " + keyName +
              ", sum(15), sum(cast(0.1 as double)), sum(c1), sum(c2), sum(c4), sum(c5) , min(15), min(0.1), min(c1), min(c2), min(c3), min(c4), min(c5), max(15), max(0.1), max(c1), max(c2), max(c3), max(c4), max(c5), sum(1) " +
              fromClause + " GROUP BY " + keyName);
    }
    EXPECT_EQ(NonPODInt64::constructed, NonPODInt64::destructed);
  }

  void testMultiKey(
      const std::vector<RowVectorPtr>& vectors,
      bool ignoreNullKeys,
      bool distinct) {
    std::vector<std::string> aggregates;
    if (!distinct) {
      aggregates = {
          "sum(15)",
          "sum(0.1)",
          "sum(c4)",
          "sum(c5)",
          "min(15)",
          "min(0.1)",
          "min(c3)",
          "min(c4)",
          "min(c5)",
          "max(15)",
          "max(0.1)",
          "max(c3)",
          "max(c4)",
          "max(c5)",
          "sumnonpod(1)"};
    }
    auto op = PlanBuilder()
                  .values(vectors)
                  .aggregation(
                      {"c0", "c1", "c6"},
                      aggregates,
                      {},
                      core::AggregationNode::Step::kPartial,
                      ignoreNullKeys)
                  .planNode();

    std::string fromClause = "FROM tmp";
    if (ignoreNullKeys) {
      fromClause +=
          " WHERE c0 IS NOT NULL AND c1 IS NOT NULL AND c6 IS NOT NULL";
    }
    if (distinct) {
      assertQuery(op, "SELECT distinct c0, c1, c6 " + fromClause);
    } else {
      assertQuery(
          op,
          "SELECT c0, c1, c6, sum(15), sum(cast(0.1 as double)), sum(c4), sum(c5), min(15), min(0.1), min(c3), min(c4), min(c5), max(15), max(0.1), max(c3), max(c4), max(c5), sum(1) " +
              fromClause + " GROUP BY c0, c1, c6");
    }
    EXPECT_EQ(NonPODInt64::constructed, NonPODInt64::destructed);
  }

  template <typename T>
  void setTestKey(
      int64_t value,
      int32_t multiplier,
      vector_size_t row,
      FlatVector<T>* vector) {
    vector->set(row, value * multiplier);
  }

  template <typename T>
  void setKey(
      int32_t column,
      int32_t cardinality,
      int32_t multiplier,
      int32_t row,
      RowVector* batch) {
    auto vector = batch->childAt(column)->asUnchecked<FlatVector<T>>();
    auto value = folly::Random::rand32(rng_) % cardinality;
    setTestKey(value, multiplier, row, vector);
  }

  void makeModeTestKeys(
      TypePtr rowType,
      int32_t numRows,
      int32_t c0,
      int32_t c1,
      int32_t c2,
      int32_t c3,
      int32_t c4,
      int32_t c5,
      std::vector<RowVectorPtr>& batches) {
    RowVectorPtr rowVector;
    for (auto count = 0; count < numRows; ++count) {
      if (count % 1000 == 0) {
        rowVector = BaseVector::create<RowVector>(
            rowType, std::min(1000, numRows - count), pool_.get());
        batches.push_back(rowVector);
        for (auto& child : rowVector->children()) {
          child->resize(1000);
        }
      }
      setKey<int64_t>(0, c0, 6, count % 1000, rowVector.get());
      setKey<int16_t>(1, c1, 1, count % 1000, rowVector.get());
      setKey<int8_t>(2, c2, 1, count % 1000, rowVector.get());
      setKey<StringView>(3, c3, 2, count % 1000, rowVector.get());
      setKey<StringView>(4, c4, 5, count % 1000, rowVector.get());
      setKey<StringView>(5, c5, 8, count % 1000, rowVector.get());
    }
  }

  // Inserts 'key' into 'order' with random bits and a serial
  // number. The serial number makes repeats of 'key' unique and the
  // random bits randomize the order in the set.
  void insertRandomOrder(
      int64_t key,
      int64_t serial,
      folly::F14FastSet<uint64_t>& order) {
    // The word has 24 bits of grouping key, 8 random bits and 32 bits of serial
    // number.
    order.insert(
        ((folly::Random::rand32(rng_) & 0xff) << 24) | key | (serial << 32));
  }

  // Returns the key from a value inserted with insertRandomOrder().
  int32_t randomOrderKey(uint64_t key) {
    return key & ((1 << 24) - 1);
  }

  void addBatch(
      int32_t count,
      RowVectorPtr rows,
      BufferPtr& dictionary,
      std::vector<RowVectorPtr>& batches) {
    std::vector<VectorPtr> children;
    dictionary->setSize(count * sizeof(vector_size_t));
    children.push_back(BaseVector::wrapInDictionary(
        BufferPtr(nullptr), dictionary, count, rows->childAt(0)));
    children.push_back(BaseVector::wrapInDictionary(
        BufferPtr(nullptr), dictionary, count, rows->childAt(1)));
    children.push_back(children[1]);
    batches.push_back(vectorMaker_.rowVector(children));
    dictionary = AlignedBuffer::allocate<vector_size_t>(
        dictionary->capacity() / sizeof(vector_size_t), rows->pool());
  }

  // Makes batches which reference rows in 'rows' via dictionary. The
  // dictionary indices are given by 'order', wich has values with
  // indices plus random bits so as to create randomly scattered,
  // sometimes repeated values.
  void makeBatches(
      RowVectorPtr rows,
      folly::F14FastSet<uint64_t>& order,
      std::vector<RowVectorPtr>& batches) {
    constexpr int32_t kBatch = 1000;
    BufferPtr dictionary =
        AlignedBuffer::allocate<vector_size_t>(kBatch, rows->pool());
    auto rawIndices = dictionary->asMutable<vector_size_t>();
    int32_t counter = 0;
    for (auto& n : order) {
      rawIndices[counter++] = randomOrderKey(n);
      if (counter == kBatch) {
        addBatch(counter, rows, dictionary, batches);
        rawIndices = dictionary->asMutable<vector_size_t>();
        counter = 0;
      }
    }
    if (counter > 0) {
      addBatch(counter, rows, dictionary, batches);
    }
  }

  std::unique_ptr<RowContainer> makeRowContainer(
      const std::vector<TypePtr>& keyTypes,
      const std::vector<TypePtr>& dependentTypes) {
    return std::make_unique<RowContainer>(
        keyTypes,
        false,
        std::vector<Accumulator>{},
        dependentTypes,
        false,
        false,
        true,
        true,
        pool_.get());
  }

  RowTypePtr rowType_{
      ROW({"c0", "c1", "c2", "c3", "c4", "c5", "c6"},
          {BIGINT(),
           SMALLINT(),
           INTEGER(),
           BIGINT(),
           REAL(),
           DOUBLE(),
           VARCHAR()})};
  folly::Random::DefaultGenerator rng_;
};

template <>
void AggregationTest::setTestKey(
    int64_t value,
    int32_t multiplier,
    vector_size_t row,
    FlatVector<StringView>* vector) {
  std::string chars;
  if (multiplier == 2) {
    chars.resize(2);
    chars[0] = (value % 64) + 32;
    chars[1] = ((value / 64) % 64) + 32;
  } else {
    chars = fmt::format("{}", value);
    for (int i = 2; i < multiplier; ++i) {
      chars = chars + fmt::format("{}", i * value);
    }
  }
  vector->set(row, StringView(chars));
}

TEST_F(AggregationTest, missingFunctionOrSignature) {
  auto data = makeRowVector({
      makeFlatVector<int64_t>({1, 2, 3}),
      makeFlatVector<bool>({true, true, false}),
  });

  // (smallint, varchar) -> bigint
  registerAggregateFunction(
      "test_aggregate",
      {AggregateFunctionSignatureBuilder()
           .returnType("bigint")
           .intermediateType("tinyint")
           .argumentType("smallint")
           .argumentType("varchar")
           .build()},
      [&](core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType,
          const core::QueryConfig& /*config*/)
          -> std::unique_ptr<exec::Aggregate> { VELOX_UNREACHABLE(); });

  std::vector<core::TypedExprPtr> inputs = {
      std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "c0"),
      std::make_shared<core::FieldAccessTypedExpr>(BOOLEAN(), "c1"),
  };
  auto missingFunc = std::make_shared<core::CallTypedExpr>(
      BIGINT(), inputs, "missing-function");
  auto wrongInputTypes =
      std::make_shared<core::CallTypedExpr>(BIGINT(), inputs, "test_aggregate");
  auto missingInputs = std::make_shared<core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{}, "test_aggregate");

  auto makePlan = [&](const core::CallTypedExprPtr& aggExpr) {
    return PlanBuilder()
        .values({data})
        .addNode([&](auto nodeId, auto source) -> core::PlanNodePtr {
          std::vector<core::AggregationNode::Aggregate> aggregates{
              {aggExpr, nullptr, {}, {}}};

          return std::make_shared<core::AggregationNode>(
              nodeId,
              core::AggregationNode::Step::kSingle,
              std::vector<core::FieldAccessTypedExprPtr>{},
              std::vector<core::FieldAccessTypedExprPtr>{},
              std::vector<std::string>{"agg"},
              aggregates,
              false,
              std::move(source));
        })
        .planNode();
  };

  CursorParameters params;
  params.planNode = makePlan(missingFunc);
  VELOX_ASSERT_THROW(
      readCursor(params, [](Task*) {}),
      "Aggregate function not registered: missing-function");

  params.planNode = makePlan(wrongInputTypes);
  VELOX_ASSERT_THROW(
      readCursor(params, [](Task*) {}),
      "Aggregate function signature is not supported: test_aggregate(BIGINT, BOOLEAN). "
      "Supported signatures: (smallint,varchar) -> tinyint -> bigint.");

  params.planNode = makePlan(missingInputs);
  VELOX_ASSERT_THROW(
      readCursor(params, [](Task*) {}),
      "Aggregate function signature is not supported: test_aggregate(). "
      "Supported signatures: (smallint,varchar) -> tinyint -> bigint.");
}

TEST_F(AggregationTest, missingLambdaFunction) {
  auto data = makeRowVector({
      makeFlatVector<int64_t>({1, 2, 3}),
  });

  auto field = [](const std::string& name) {
    return std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), name);
  };

  std::vector<core::TypedExprPtr> inputs = {
      field("c0"),
      // (a, b) -> a + b.
      std::make_shared<core::LambdaTypedExpr>(
          ROW({"a", "b"}, {BIGINT(), BIGINT()}),
          std::make_shared<core::CallTypedExpr>(
              BIGINT(),
              std::vector<core::TypedExprPtr>{field("a"), field("b")},
              "multiply")),
  };

  auto plan = PlanBuilder()
                  .values({data})
                  .addNode([&](auto nodeId, auto source) -> core::PlanNodePtr {
                    std::vector<core::AggregationNode::Aggregate> aggregates{
                        {std::make_shared<core::CallTypedExpr>(
                             BIGINT(), inputs, "missing-lambda"),
                         nullptr,
                         {},
                         {}}};

                    return std::make_shared<core::AggregationNode>(
                        nodeId,
                        core::AggregationNode::Step::kSingle,
                        std::vector<core::FieldAccessTypedExprPtr>{},
                        std::vector<core::FieldAccessTypedExprPtr>{},
                        std::vector<std::string>{"agg"},
                        aggregates,
                        false,
                        std::move(source));
                  })
                  .planNode();

  CursorParameters params;
  params.planNode = plan;
  VELOX_ASSERT_THROW(
      readCursor(params, [](Task*) {}),
      "Aggregate function not registered: missing-lambda");
}

TEST_F(AggregationTest, global) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);

  auto op = PlanBuilder()
                .values(vectors)
                .aggregation(
                    {},
                    {"sum(15)",
                     "sum(c1)",
                     "sum(c2)",
                     "sum(c4)",
                     "sum(c5)",
                     "min(15)",
                     "min(c1)",
                     "min(c2)",
                     "min(c3)",
                     "min(c4)",
                     "min(c5)",
                     "max(15)",
                     "max(c1)",
                     "max(c2)",
                     "max(c3)",
                     "max(c4)",
                     "max(c5)",
                     "sumnonpod(1)"},
                    {},
                    core::AggregationNode::Step::kPartial,
                    false)
                .planNode();

  assertQuery(
      op,
      "SELECT sum(15), sum(c1), sum(c2), sum(c4), sum(c5), "
      "min(15), min(c1), min(c2), min(c3), min(c4), min(c5), "
      "max(15), max(c1), max(c2), max(c3), max(c4), max(c5), sum(1) FROM tmp");

  EXPECT_EQ(NonPODInt64::constructed, NonPODInt64::destructed);
}

TEST_F(AggregationTest, singleBigintKey) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  testSingleKey<int64_t>(vectors, "c0", false, false);
  testSingleKey<int64_t>(vectors, "c0", true, false);
}

TEST_F(AggregationTest, singleBigintKeyDistinct) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  testSingleKey<int64_t>(vectors, "c0", false, true);
  testSingleKey<int64_t>(vectors, "c0", true, true);
}

TEST_F(AggregationTest, singleStringKey) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  testSingleKey<StringView>(vectors, "c6", false, false);
  testSingleKey<StringView>(vectors, "c6", true, false);
}

TEST_F(AggregationTest, singleStringKeyDistinct) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  testSingleKey<StringView>(vectors, "c6", false, true);
  testSingleKey<StringView>(vectors, "c6", true, true);
}

TEST_F(AggregationTest, multiKey) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  testMultiKey(vectors, false, false);
  testMultiKey(vectors, true, false);
}

TEST_F(AggregationTest, multiKeyDistinct) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  testMultiKey(vectors, false, true);
  testMultiKey(vectors, true, true);
}

TEST_F(AggregationTest, aggregateOfNulls) {
  auto rowVector = makeRowVector({
      BatchMaker::createVector<TypeKind::BIGINT>(
          rowType_->childAt(0), 100, *pool_),
      makeNullConstant(TypeKind::SMALLINT, 100),
  });

  auto vectors = {rowVector};
  createDuckDbTable(vectors);

  auto op = PlanBuilder()
                .values(vectors)
                .aggregation(
                    {"c0"},
                    {"sum(c1)", "min(c1)", "max(c1)"},
                    {},
                    core::AggregationNode::Step::kPartial,
                    false)
                .planNode();

  assertQuery(op, "SELECT c0, sum(c1), min(c1), max(c1) FROM tmp GROUP BY c0");

  // global aggregation
  op = PlanBuilder()
           .values(vectors)
           .aggregation(
               {},
               {"sum(c1)", "min(c1)", "max(c1)"},
               {},
               core::AggregationNode::Step::kPartial,
               false)
           .planNode();

  assertQuery(op, "SELECT sum(c1), min(c1), max(c1) FROM tmp");
}

// Verify behavior of setNull method.
TEST_F(AggregationTest, setNull) {
  AggregateFunc aggregate(BIGINT());
  int32_t nullOffset = 0;
  aggregate.setOffsets(
      0,
      RowContainer::nullByte(nullOffset),
      RowContainer::nullMask(nullOffset),
      0);
  char group{0};
  aggregate.clearNullTest(&group);
  EXPECT_FALSE(aggregate.isNullTest(&group));

  // Verify setNull returns true if value is non null.
  EXPECT_TRUE(aggregate.setNullTest(&group));
  EXPECT_TRUE(aggregate.isNullTest(&group));

  // Verify setNull returns false if value is already null.
  EXPECT_FALSE(aggregate.setNullTest(&group));
  EXPECT_TRUE(aggregate.isNullTest(&group));
}

TEST_F(AggregationTest, hashmodes) {
  rng_.seed(1);
  auto rowType =
      ROW({"c0", "c1", "c2", "c3", "c4", "c5"},
          {BIGINT(), SMALLINT(), TINYINT(), VARCHAR(), VARCHAR(), VARCHAR()});

  std::vector<RowVectorPtr> batches;

  // 20K rows with all at low cardinality.
  makeModeTestKeys(rowType, 20000, 2, 2, 2, 4, 4, 4, batches);
  // 20K rows with all at slightly higher cardinality, still in array range.
  makeModeTestKeys(rowType, 20000, 2, 2, 2, 4, 16, 4, batches);
  // 100K rows with cardinality outside of array range. We transit to
  // generic hash table from normalized keys when running out of quota
  // for distinct string storage for the sixth key.
  makeModeTestKeys(rowType, 100000, 1000000, 2, 2, 4, 4, 1000000, batches);
  createDuckDbTable(batches);
  auto op =
      PlanBuilder()
          .values(batches)
          .singleAggregation({"c0", "c1", "c2", "c3", "c4", "c5"}, {"sum(1)"})
          .planNode();

  assertQuery(
      op,
      "SELECT c0, c1, C2, C3, C4, C5, sum(1) FROM tmp "
      " GROUP BY c0, c1, c2, c3, c4, c5");
}

TEST_F(AggregationTest, rangeToDistinct) {
  rng_.seed(1);
  auto rowType =
      ROW({"c0", "c1", "c2", "c3", "c4", "c5"},
          {BIGINT(), SMALLINT(), TINYINT(), VARCHAR(), VARCHAR(), VARCHAR()});

  std::vector<RowVectorPtr> batches;
  // 20K rows with all at low cardinality. c0 is a range.
  makeModeTestKeys(rowType, 20000, 2000, 2, 2, 4, 4, 4, batches);
  // 20 rows that make c0 represented as distincts.
  makeModeTestKeys(rowType, 20, 200000000, 2, 2, 4, 4, 4, batches);
  // More keys in the low cardinality range. We see if these still hit
  // after the re-encoding of c0.
  makeModeTestKeys(rowType, 10000, 2000, 2, 2, 4, 4, 4, batches);

  createDuckDbTable(batches);
  auto op =
      PlanBuilder()
          .values(batches)
          .singleAggregation({"c0", "c1", "c2", "c3", "c4", "c5"}, {"sum(1)"})
          .planNode();

  assertQuery(
      op,
      "SELECT c0, c1, c2, c3, c4, c5, sum(1) FROM tmp "
      " GROUP BY c0, c1, c2, c3, c4, c5");
}

TEST_F(AggregationTest, allKeyTypes) {
  // Covers different key types. Unlike the integer/string tests, the
  // hash table begins life in the generic mode, not array or
  // normalized key. Add types here as they become supported.
  auto rowType =
      ROW({"c0", "c1", "c2", "c3", "c4", "c5"},
          {DOUBLE(), REAL(), BIGINT(), INTEGER(), BOOLEAN(), VARCHAR()});

  std::vector<RowVectorPtr> batches;
  for (auto i = 0; i < 10; ++i) {
    batches.push_back(std::static_pointer_cast<RowVector>(
        BatchMaker::createBatch(rowType, 100, *pool_)));
  }
  createDuckDbTable(batches);
  auto op =
      PlanBuilder()
          .values(batches)
          .singleAggregation({"c0", "c1", "c2", "c3", "c4", "c5"}, {"sum(1)"})
          .planNode();

  assertQuery(
      op,
      "SELECT c0, c1, c2, c3, c4, c5, sum(1) FROM tmp "
      " GROUP BY c0, c1, c2, c3, c4, c5");
}

TEST_F(AggregationTest, partialAggregationMemoryLimit) {
  auto vectors = {
      makeRowVector({makeFlatVector<int32_t>(
          100, [](auto row) { return row; }, nullEvery(5))}),
      makeRowVector({makeFlatVector<int32_t>(
          110, [](auto row) { return row + 29; }, nullEvery(7))}),
      makeRowVector({makeFlatVector<int32_t>(
          90, [](auto row) { return row - 71; }, nullEvery(7))}),
  };

  createDuckDbTable(vectors);

  // Set an artificially low limit on the amount of data to accumulate in
  // the partial aggregation.

  // Distinct aggregation.
  core::PlanNodeId aggNodeId;
  auto task = AssertQueryBuilder(duckDbQueryRunner_)
                  .config(QueryConfig::kMaxPartialAggregationMemory, "100")
                  .plan(PlanBuilder()
                            .values(vectors)
                            .partialAggregation({"c0"}, {})
                            .capturePlanNodeId(aggNodeId)
                            .finalAggregation()
                            .planNode())
                  .assertResults("SELECT distinct c0 FROM tmp");
  EXPECT_GT(
      toPlanStats(task->taskStats())
          .at(aggNodeId)
          .customStats.at("flushRowCount")
          .sum,
      0);
  EXPECT_GT(
      toPlanStats(task->taskStats())
          .at(aggNodeId)
          .customStats.at("flushRowCount")
          .max,
      0);

  // Count aggregation.
  task = AssertQueryBuilder(duckDbQueryRunner_)
             .config(QueryConfig::kMaxPartialAggregationMemory, "1")
             .plan(PlanBuilder()
                       .values(vectors)
                       .partialAggregation({"c0"}, {"count(1)"})
                       .capturePlanNodeId(aggNodeId)
                       .finalAggregation()
                       .planNode())
             .assertResults("SELECT c0, count(1) FROM tmp GROUP BY 1");
  EXPECT_GT(
      toPlanStats(task->taskStats())
          .at(aggNodeId)
          .customStats.at("flushRowCount")
          .count,
      0);
  EXPECT_GT(
      toPlanStats(task->taskStats())
          .at(aggNodeId)
          .customStats.at("flushRowCount")
          .max,
      0);

  // Global aggregation.
  task = AssertQueryBuilder(duckDbQueryRunner_)
             .config(QueryConfig::kMaxPartialAggregationMemory, "1")
             .plan(PlanBuilder()
                       .values(vectors)
                       .partialAggregation({}, {"sum(c0)"})
                       .capturePlanNodeId(aggNodeId)
                       .finalAggregation()
                       .planNode())
             .assertResults("SELECT sum(c0) FROM tmp");
  EXPECT_EQ(
      0,
      toPlanStats(task->taskStats())
          .at(aggNodeId)
          .customStats.count("flushRowCount"));
}

TEST_F(AggregationTest, partialDistinctWithAbandon) {
  auto vectors = {
      // 1st batch will produce 100 distinct groups from 10 rows.
      makeRowVector(
          {makeFlatVector<int32_t>(100, [](auto row) { return row; })}),
      // 2st batch will trigger abandon partial aggregation event with no new
      // distinct values.
      makeRowVector({makeFlatVector<int32_t>(1, [](auto row) { return row; })}),
      // 3rd batch will not produce any new distinct values.
      makeRowVector(
          {makeFlatVector<int32_t>(50, [](auto row) { return row; })}),
      // 4th batch will not produce 10 new distinct values.
      makeRowVector(
          {makeFlatVector<int32_t>(200, [](auto row) { return row % 110; })}),
  };

  createDuckDbTable(vectors);

  // We are setting abandon partial aggregation config properties to low values,
  // so they are triggered on the second batch.

  // Distinct aggregation.
  auto task = AssertQueryBuilder(duckDbQueryRunner_)
                  .config(QueryConfig::kAbandonPartialAggregationMinRows, "100")
                  .config(QueryConfig::kAbandonPartialAggregationMinPct, "50")
                  .config("max_drivers_per_task", "1")
                  .plan(PlanBuilder()
                            .values(vectors)
                            .partialAggregation({"c0"}, {})
                            .finalAggregation()
                            .planNode())
                  .assertResults("SELECT distinct c0 FROM tmp");

  // with aggregation, just in case.
  task = AssertQueryBuilder(duckDbQueryRunner_)
             .config(QueryConfig::kAbandonPartialAggregationMinRows, "100")
             .config(QueryConfig::kAbandonPartialAggregationMinPct, "50")
             .config("max_drivers_per_task", "1")
             .plan(PlanBuilder()
                       .values(vectors)
                       .partialAggregation({"c0"}, {"sum(c0)"})
                       .finalAggregation()
                       .planNode())
             .assertResults("SELECT distinct c0, sum(c0) FROM tmp group by c0");
}

TEST_F(AggregationTest, largeValueRangeArray) {
  // We have keys that map to integer range. The keys are
  // a little under max array hash table size apart. This wastes 16MB of
  // memory for the array hash table. Every batch will overflow the
  // max partial memory. We check that when detecting the first
  // overflow, the partial agg rehashes itself not to use a value
  // range array hash mode and will accept more batches without
  // flushing.
  std::string string1k;
  string1k.resize(1000);
  std::vector<RowVectorPtr> vectors;
  // Make two identical ectors. The first one overflows the max size
  // but gets rehashed to smaller by using value ids instead of
  // ranges. The next vector fits in the space made freed.
  for (auto i = 0; i < 2; ++i) {
    vectors.push_back(makeRowVector(
        {makeFlatVector<int64_t>(
             1000, [](auto row) { return row % 2 == 0 ? 100 : 1000000; }),
         makeFlatVector<StringView>(
             1000, [&](auto /*row*/) { return StringView(string1k); })}));
  }
  std::vector<RowVectorPtr> expected = {makeRowVector(
      {makeFlatVector<int64_t>({100, 1000000}),
       makeFlatVector<int64_t>({1000, 1000})})};

  core::PlanNodeId partialAggId;
  core::PlanNodeId finalAggId;
  auto op = PlanBuilder()
                .values({vectors})
                .partialAggregation({"c0"}, {"array_agg(c1)"})
                .capturePlanNodeId(partialAggId)
                .finalAggregation()
                .capturePlanNodeId(finalAggId)
                .project({"c0", "cardinality(a0) as l"})
                .planNode();
  auto task = test::assertQuery(op, expected);
  auto stats = toPlanStats(task->taskStats());
  auto runtimeStats = stats.at(partialAggId).customStats;

  // The partial agg is expected to exceed max size after the first batch and
  // see that it has an oversize range based array with just 2 entries. It is
  // then expected to change hash mode and rehash.
  EXPECT_EQ(1, runtimeStats.at("hashtable.numRehashes").count);

  // The partial agg is expected to flush just once. The final agg gets one
  // batch.
  EXPECT_EQ(1, stats.at(finalAggId).inputVectors);
}

TEST_F(AggregationTest, partialAggregationMemoryLimitIncrease) {
  constexpr int64_t kGB = 1 << 30;
  constexpr int64_t kB = 1 << 10;
  auto vectors = {
      makeRowVector({makeFlatVector<int32_t>(
          100, [](auto row) { return row; }, nullEvery(5))}),
      makeRowVector({makeFlatVector<int32_t>(
          110, [](auto row) { return row + 29; }, nullEvery(7))}),
      makeRowVector({makeFlatVector<int32_t>(
          90, [](auto row) { return row - 71; }, nullEvery(7))}),
  };

  createDuckDbTable(vectors);

  struct {
    int64_t initialPartialMemoryLimit;
    int64_t extendedPartialMemoryLimit;
    bool expectedPartialOutputFlush;
    bool expectedPartialAggregationMemoryLimitIncrease;

    std::string debugString() const {
      return fmt::format(
          "initialPartialMemoryLimit: {}, extendedPartialMemoryLimit: {}, expectedPartialOutputFlush: {}, expectedPartialAggregationMemoryLimitIncrease: {}",
          initialPartialMemoryLimit,
          extendedPartialMemoryLimit,
          expectedPartialOutputFlush,
          expectedPartialAggregationMemoryLimitIncrease);
    }
  } testSettings[] = {// Set with a large initial partial aggregation memory
                      // limit and expect no flush and memory limit bump.
                      {kGB, 2 * kGB, false, false},
                      // Set with a very small initial and extended partial
                      // aggregation memory limit.
                      {100, 100, true, false},
                      // Set with a very small initial partial aggregation
                      // memory limit but large extended memory limit.
                      {100, kGB, true, true}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    // Distinct aggregation.
    core::PlanNodeId aggNodeId;
    auto task = AssertQueryBuilder(duckDbQueryRunner_)
                    .config(
                        QueryConfig::kMaxPartialAggregationMemory,
                        std::to_string(testData.initialPartialMemoryLimit))
                    .config(
                        QueryConfig::kMaxExtendedPartialAggregationMemory,
                        std::to_string(testData.extendedPartialMemoryLimit))
                    .plan(PlanBuilder()
                              .values(vectors)
                              .partialAggregation({"c0"}, {})
                              .capturePlanNodeId(aggNodeId)
                              .finalAggregation()
                              .planNode())
                    .assertResults("SELECT distinct c0 FROM tmp");
    const auto runtimeStats =
        toPlanStats(task->taskStats()).at(aggNodeId).customStats;
    if (testData.expectedPartialOutputFlush > 0) {
      EXPECT_LT(0, runtimeStats.at("flushRowCount").count);
      EXPECT_LT(0, runtimeStats.at("flushRowCount").max);
      EXPECT_LT(0, runtimeStats.at("partialAggregationPct").max);
    } else {
      EXPECT_EQ(0, runtimeStats.count("flushRowCount"));
      EXPECT_EQ(0, runtimeStats.count("partialAggregationPct"));
    }
    if (testData.expectedPartialAggregationMemoryLimitIncrease) {
      EXPECT_LT(
          testData.initialPartialMemoryLimit,
          runtimeStats.at("maxExtendedPartialAggregationMemoryUsage").max);
      EXPECT_GE(
          testData.extendedPartialMemoryLimit,
          runtimeStats.at("maxExtendedPartialAggregationMemoryUsage").max);
    } else {
      EXPECT_EQ(
          0, runtimeStats.count("maxExtendedPartialAggregationMemoryUsage"));
    }
  }
}

TEST_F(AggregationTest, partialAggregationMaybeReservationReleaseCheck) {
  auto vectors = {
      makeRowVector({makeFlatVector<int32_t>(
          100, [](auto row) { return row; }, nullEvery(5))}),
      makeRowVector({makeFlatVector<int32_t>(
          110, [](auto row) { return row + 29; }, nullEvery(7))}),
      makeRowVector({makeFlatVector<int32_t>(
          90, [](auto row) { return row - 71; }, nullEvery(7))}),
  };

  createDuckDbTable(vectors);

  constexpr int64_t kGB = 1 << 30;
  const int64_t kMaxPartialMemoryUsage = 1 * kGB;
  const int64_t kMaxUserMemoryUsage = 2 * kMaxPartialMemoryUsage;
  // Make sure partial aggregation runs out of memory after first batch.
  CursorParameters params;
  params.queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
  params.queryCtx->testingOverrideConfigUnsafe({
      {QueryConfig::kMaxPartialAggregationMemory,
       std::to_string(kMaxPartialMemoryUsage)},
      {QueryConfig::kMaxExtendedPartialAggregationMemory,
       std::to_string(kMaxPartialMemoryUsage)},
  });
  {
    static_cast<memory::MemoryPoolImpl*>(params.queryCtx->pool())
        ->testingSetCapacity(kMaxUserMemoryUsage);
  }
  core::PlanNodeId aggNodeId;
  params.planNode = PlanBuilder()
                        .values(vectors)
                        .partialAggregation({"c0"}, {})
                        .capturePlanNodeId(aggNodeId)
                        .finalAggregation()
                        .planNode();
  auto task = assertQuery(params, "SELECT distinct c0 FROM tmp");
  const auto runtimeStats =
      toPlanStats(task->taskStats()).at(aggNodeId).customStats;
  EXPECT_EQ(0, runtimeStats.count("flushRowCount"));
  EXPECT_EQ(0, runtimeStats.count("maxExtendedPartialAggregationMemoryUsage"));
  EXPECT_EQ(0, runtimeStats.count("partialAggregationPct"));
  // Check all the reserved memory have been released.
  EXPECT_EQ(0, task->pool()->availableReservation());
  EXPECT_GT(kMaxPartialMemoryUsage, task->pool()->currentBytes());
}

TEST_F(AggregationTest, spillWithMemoryLimit) {
  constexpr int32_t kNumDistinct = 2000;
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  rng_.seed(1);
  rowType_ = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({}, pool());
  const int32_t numBatches = 5;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType_));
  }
  struct {
    uint64_t aggregationMemLimit;
    bool expectSpill;

    std::string debugString() const {
      return fmt::format(
          "aggregationMemLimit:{}, expectSpill:{}",
          aggregationMemLimit,
          expectSpill);
    }
  } testSettings[] = {// Memory limit is disabled so spilling is not triggered.
                      {0, false},
                      // Memory limit is too small so always trigger spilling.
                      {1, true},
                      // Memory limit is too large so spilling is not triggered.
                      {1'000'000'000, false}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto results = AssertQueryBuilder(
                       PlanBuilder()
                           .values(batches)
                           .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                           .planNode())
                       .queryCtx(queryCtx)
                       .copyResults(pool_.get());
    auto task = AssertQueryBuilder(
                    PlanBuilder()
                        .values(batches)
                        .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                        .planNode())
                    .queryCtx(queryCtx)
                    .spillDirectory(tempDirectory->path)
                    .config(QueryConfig::kSpillEnabled, "true")
                    .config(QueryConfig::kAggregationSpillEnabled, "true")
                    .config(
                        QueryConfig::kAggregationSpillMemoryThreshold,
                        std::to_string(testData.aggregationMemLimit))
                    .assertResults(results);

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(testData.expectSpill, stats[0].operatorStats[1].spilledBytes > 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, DISABLED_spillWithEmptyPartition) {
  constexpr int32_t kNumDistinct = 100'000;
  constexpr int64_t kMaxBytes = 20LL << 20; // 20 MB
  rowType_ = ROW({"c0", "a"}, {INTEGER(), VARCHAR()});
  // Used to calculate the aggregation spilling partition number.
  const int kPartitionStartBit = 29;
  const int kPartitionsBits = 2;
  const HashBitRange hashBits{
      kPartitionStartBit, kPartitionStartBit + kPartitionsBits};
  const int kNumPartitions = hashBits.numPartitions();
  std::vector<uint64_t> hashes(1);

  for (int emptyPartitionNum : {0, 1, 3}) {
    SCOPED_TRACE(fmt::format("emptyPartitionNum: {}", emptyPartitionNum));
    rng_.seed(1);
    // The input batch has kNumDistinct distinct keys. The repeat count of a key
    // is given by min(1, (k % 100) - 90). The batch is repeated 3 times, each
    // time in a different order.
    auto rowVector =
        BaseVector::create<RowVector>(rowType_, kNumDistinct, pool_.get());
    SelectivityVector allRows(kNumDistinct);
    const TypePtr keyType = rowVector->type()->childAt(0);
    const TypePtr valueType = rowVector->type()->childAt(1);
    auto rowContainer = makeRowContainer({keyType}, {valueType});
    // Used to check hash aggregation partition.
    char* testRow = rowContainer->newRow();
    std::vector<char*> testRows(1, testRow);
    const auto testRowSet = folly::Range<char**>(testRows.data(), 1);

    folly::F14FastSet<uint64_t> order1;
    folly::F14FastSet<uint64_t> order2;
    folly::F14FastSet<uint64_t> order3;

    auto keyVector = rowVector->childAt(0)->as<FlatVector<int32_t>>();
    keyVector->resize(kNumDistinct);
    auto valueVector = rowVector->childAt(1)->as<FlatVector<StringView>>();
    valueVector->resize(kNumDistinct);

    DecodedVector decodedVector(*keyVector, allRows);
    int32_t totalCount = 0;
    for (int key = 0, index = 0; index < kNumDistinct; ++key) {
      keyVector->set(index, key);
      // Skip the empty partition.
      rowContainer->store(decodedVector, index, testRow, 0);
      // Calculate hashes for this batch of spill candidates.
      rowContainer->hash(0, testRowSet, false, hashes.data());
      const int partitionNum = hashBits.partition(hashes[0], kNumPartitions);
      if (partitionNum == emptyPartitionNum) {
        continue;
      }
      std::string str = fmt::format("{}{}", key, key);
      valueVector->set(index, StringView(str));
      const int numRepeats = std::max(1, (index % 100) - 90);
      // We make random permutations of the data by adding the indices into a
      // set with a random 6 high bits followed by a serial number. These are
      // inlined in the F14FastSet in an order that depends on the hash number.
      for (auto i = 0; i < numRepeats; ++i) {
        ++totalCount;
        insertRandomOrder(index, totalCount, order1);
        insertRandomOrder(index, totalCount, order2);
        insertRandomOrder(index, totalCount, order3);
      }
      ++index;
    }
    std::vector<RowVectorPtr> batches;
    makeBatches(rowVector, order1, batches);
    makeBatches(rowVector, order2, batches);
    makeBatches(rowVector, order3, batches);
    auto results =
        AssertQueryBuilder(PlanBuilder()
                               .values(batches)
                               .singleAggregation({"c0"}, {"array_agg(c1)"})
                               .planNode())
            .copyResults(pool_.get());

    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));

    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Spiller",
        std::function<void(const HashBitRange*)>(
            ([&](const HashBitRange* spillerBitRange) {
              ASSERT_EQ(kPartitionStartBit, spillerBitRange->begin());
              ASSERT_EQ(
                  kPartitionStartBit + kPartitionsBits, spillerBitRange->end());
            })));

    auto task =
        AssertQueryBuilder(PlanBuilder()
                               .values(batches)
                               .singleAggregation({"c0"}, {"array_agg(c1)"})
                               .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(QueryConfig::kSpillEnabled, "true")
            .config(QueryConfig::kAggregationSpillEnabled, "true")
            .config(QueryConfig::kMinSpillRunSize, std::to_string(1000'000'000))
            .config(
                QueryConfig::kSpillPartitionBits,
                std::to_string(kPartitionsBits))
            .config(
                QueryConfig::kSpillStartPartitionBit,
                std::to_string(kPartitionStartBit))
            .config(QueryConfig::kPreferredOutputBatchBytes, "1024")
            .assertResults(results);

    auto stats = task->taskStats().pipelineStats;
    // Check spilled bytes.
    EXPECT_LT(0, stats[0].operatorStats[1].spilledBytes);
    EXPECT_GE(kNumPartitions - 1, stats[0].operatorStats[1].spilledPartitions);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

TEST_F(AggregationTest, spillWithNonSpillingPartition) {
  constexpr int32_t kNumDistinct = 100'000;
  constexpr int64_t kMaxBytes = 20LL << 20; // 20 MB
  rowType_ = ROW({"c0", "a"}, {INTEGER(), VARCHAR()});
  // Used to calculate the aggregation spilling partition number.
  const int kPartitionsBits = 2;
  const HashBitRange hashBits{29, 31};
  const int kNumPartitions = hashBits.numPartitions();
  std::vector<uint64_t> hashes(1);

  // Build two partitions one with large amount of data and the other with a
  // small amount of data (only one row).
  const int kLargePartitionNum = 1;
  const int kSmallPartitionNum = 0;
  rng_.seed(1);
  // The input batch has kNumDistinct distinct keys. The repeat count of a key
  // is given by min(1, (k % 100) - 90). The batch is repeated 3 times, each
  // time in a different order.
  auto rowVector =
      BaseVector::create<RowVector>(rowType_, kNumDistinct, pool_.get());
  SelectivityVector allRows(kNumDistinct);
  const TypePtr keyType = rowVector->type()->childAt(0);
  const TypePtr valueType = rowVector->type()->childAt(1);
  auto rowContainer = makeRowContainer({keyType}, {valueType});
  // Used to check hash aggregation partition.
  char* testRow = rowContainer->newRow();
  std::vector<char*> testRows(1, testRow);
  const auto testRowSet = folly::Range<char**>(testRows.data(), 1);

  folly::F14FastSet<uint64_t> order1;
  folly::F14FastSet<uint64_t> order2;
  folly::F14FastSet<uint64_t> order3;

  auto keyVector = rowVector->childAt(0)->as<FlatVector<int32_t>>();
  keyVector->resize(kNumDistinct);
  auto valueVector = rowVector->childAt(1)->as<FlatVector<StringView>>();
  valueVector->resize(kNumDistinct);

  DecodedVector decodedVector(*keyVector, allRows);
  int32_t totalCount = 0;
  int32_t numRowsFromSmallPartition = 0;
  for (int key = 0, index = 0; index < kNumDistinct; ++key) {
    keyVector->set(index, key);
    // Skip the empty partition.
    rowContainer->store(decodedVector, index, testRow, 0);
    // Calculate hashes for this batch of spill candidates.
    rowContainer->hash(0, testRowSet, false, hashes.data());
    const int partitionNum = hashBits.partition(hashes[0], kNumPartitions);
    if (partitionNum != kSmallPartitionNum &&
        partitionNum != kLargePartitionNum) {
      continue;
    }
    if (partitionNum == kSmallPartitionNum && numRowsFromSmallPartition > 0) {
      continue;
    }
    numRowsFromSmallPartition += partitionNum == kSmallPartitionNum;
    std::string str = fmt::format("{}{}", key, key);
    valueVector->set(index, StringView(str));
    const int numRepeats = std::max(1, (index % 100) - 90);
    // We make random permutations of the data by adding the indices into a
    // set with a random 6 high bits followed by a serial number. These are
    // inlined in the F14FastSet in an order that depends on the hash number.
    for (auto i = 0; i < numRepeats; ++i) {
      ++totalCount;
      insertRandomOrder(index, totalCount, order1);
      insertRandomOrder(index, totalCount, order2);
      insertRandomOrder(index, totalCount, order3);
    }
    ++index;
  }
  std::vector<RowVectorPtr> batches;
  makeBatches(rowVector, order1, batches);
  makeBatches(rowVector, order2, batches);
  makeBatches(rowVector, order3, batches);
  auto results =
      AssertQueryBuilder(PlanBuilder()
                             .values(batches)
                             .singleAggregation({"c0"}, {"array_agg(c1)"})
                             .planNode())
          .copyResults(pool_.get());

  auto tempDirectory = exec::test::TempDirectoryPath::create();
  auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
  queryCtx->testingOverrideMemoryPool(
      memory::defaultMemoryManager().addRootPool(
          queryCtx->queryId(), kMaxBytes));

  auto task =
      AssertQueryBuilder(PlanBuilder()
                             .values(batches)
                             .singleAggregation({"c0"}, {"array_agg(c1)"})
                             .planNode())
          .queryCtx(queryCtx)
          .spillDirectory(tempDirectory->path)
          .config(QueryConfig::kSpillEnabled, "true")
          .config(QueryConfig::kAggregationSpillEnabled, "true")
          .config(
              QueryConfig::kSpillPartitionBits, std::to_string(kPartitionsBits))
          // Set to increase the hash table a little bit to only trigger spill
          // on the partition with most spillable data.
          .config(QueryConfig::kSpillableReservationGrowthPct, "25")
          .config(QueryConfig::kPreferredOutputBatchBytes, "1024")
          .assertResults(results);

  auto stats = task->taskStats().pipelineStats;
  // Check spilled bytes.
  EXPECT_LT(0, stats[0].operatorStats[1].spilledBytes);
  EXPECT_EQ(1, stats[0].operatorStats[1].spilledPartitions);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

/// Verify number of memory allocations in the HashAggregation operator.
TEST_F(AggregationTest, memoryAllocations) {
  vector_size_t size = 1'024;
  std::vector<RowVectorPtr> data;
  for (auto i = 0; i < 10; ++i) {
    data.push_back(makeRowVector({
        makeFlatVector<int64_t>(size, [](auto row) { return row; }),
        makeFlatVector<int64_t>(size, [](auto row) { return row + 3; }),
    }));
  }

  createDuckDbTable(data);

  core::PlanNodeId projectNodeId;
  core::PlanNodeId aggNodeId;
  auto plan = PlanBuilder()
                  .values(data)
                  .project({"c0 + c1"})
                  .capturePlanNodeId(projectNodeId)
                  .singleAggregation({}, {"sum(p0)"})
                  .capturePlanNodeId(aggNodeId)
                  .planNode();

  auto task = assertQuery(plan, "SELECT sum(c0 + c1) FROM tmp");

  // Verify memory allocations. Project operator should allocate a single vector
  // and re-use it. Aggregation should make 2 allocations: 1 for the
  // RowContainer holding single accumulator and 1 for the result.
  auto planStats = toPlanStats(task->taskStats());
  ASSERT_EQ(1, planStats.at(projectNodeId).numMemoryAllocations);
  ASSERT_EQ(2, planStats.at(aggNodeId).numMemoryAllocations);

  plan = PlanBuilder()
             .values(data)
             .project({"c0", "c0 + c1"})
             .capturePlanNodeId(projectNodeId)
             .singleAggregation({"c0"}, {"sum(p1)"})
             .capturePlanNodeId(aggNodeId)
             .planNode();

  task = assertQuery(plan, "SELECT c0, sum(c0 + c1) FROM tmp GROUP BY 1");

  // Verify memory allocations. Project operator should allocate a single vector
  // and re-use it. Aggregation should make 5 allocations: 1 for the hash table,
  // 1 for the RowContainer holding accumulators, 3 for results (2 for values
  // and nulls buffers of the grouping key column, 1 for sum column).
  planStats = toPlanStats(task->taskStats());
  ASSERT_EQ(1, planStats.at(projectNodeId).numMemoryAllocations);
  ASSERT_EQ(5, planStats.at(aggNodeId).numMemoryAllocations);
}

TEST_F(AggregationTest, groupingSets) {
  vector_size_t size = 1'000;
  auto data = makeRowVector(
      {"k1", "k2", "a", "b"},
      {
          makeFlatVector<int64_t>(size, [](auto row) { return row % 11; }),
          makeFlatVector<int64_t>(size, [](auto row) { return row % 17; }),
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          makeFlatVector<StringView>(
              size,
              [](auto row) {
                auto str = std::string(row % 12, 'x');
                return StringView(str);
              }),
      });

  createDuckDbTable({data});

  auto plan =
      PlanBuilder()
          .values({data})
          .groupId({{"k1"}, {"k2"}}, {"a", "b"})
          .singleAggregation(
              {"k1", "k2", "group_id"},
              {"count(1) as count_1", "sum(a) as sum_a", "max(b) as max_b"})
          .project({"k1", "k2", "count_1", "sum_a", "max_b"})
          .planNode();

  assertQuery(
      plan,
      "SELECT k1, k2, count(1), sum(a), max(b) FROM tmp GROUP BY GROUPING SETS ((k1), (k2))");

  // Compute a subset of aggregates per grouping set by using masks based on
  // group_id column.
  plan = PlanBuilder()
             .values({data})
             .groupId({{"k1"}, {"k2"}}, {"a", "b"})
             .project(
                 {"k1",
                  "k2",
                  "group_id",
                  "a",
                  "b",
                  "group_id = 0 as mask_a",
                  "group_id = 1 as mask_b"})
             .singleAggregation(
                 {"k1", "k2", "group_id"},
                 {"count(1) as count_1", "sum(a) as sum_a", "max(b) as max_b"},
                 {"", "mask_a", "mask_b"})
             .project({"k1", "k2", "count_1", "sum_a", "max_b"})
             .planNode();

  assertQuery(
      plan,
      "SELECT k1, null, count(1), sum(a), null FROM tmp GROUP BY k1 "
      "UNION ALL "
      "SELECT null, k2, count(1), null, max(b) FROM tmp GROUP BY k2");

  // Cube.
  plan = PlanBuilder()
             .values({data})
             .groupId({{"k1", "k2"}, {"k1"}, {"k2"}, {}}, {"a", "b"})
             .singleAggregation(
                 {"k1", "k2", "group_id"},
                 {"count(1) as count_1", "sum(a) as sum_a", "max(b) as max_b"})
             .project({"k1", "k2", "count_1", "sum_a", "max_b"})
             .planNode();

  assertQuery(
      plan,
      "SELECT k1, k2, count(1), sum(a), max(b) FROM tmp GROUP BY CUBE (k1, k2)");

  // Rollup.
  plan = PlanBuilder()
             .values({data})
             .groupId({{"k1", "k2"}, {"k1"}, {}}, {"a", "b"})
             .singleAggregation(
                 {"k1", "k2", "group_id"},
                 {"count(1) as count_1", "sum(a) as sum_a", "max(b) as max_b"})
             .project({"k1", "k2", "count_1", "sum_a", "max_b"})
             .planNode();

  assertQuery(
      plan,
      "SELECT k1, k2, count(1), sum(a), max(b) FROM tmp GROUP BY ROLLUP (k1, k2)");
}

TEST_F(AggregationTest, groupingSetsOutput) {
  vector_size_t size = 1'000;
  auto data = makeRowVector(
      {"k1", "k2", "a", "b"},
      {
          makeFlatVector<int64_t>(size, [](auto row) { return row % 11; }),
          makeFlatVector<int64_t>(size, [](auto row) { return row % 17; }),
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          makeFlatVector<StringView>(
              size,
              [](auto row) {
                auto str = std::string(row % 12, 'x');
                return StringView(str);
              }),
      });

  createDuckDbTable({data});

  core::PlanNodePtr reversedOrderGroupIdNode;
  core::PlanNodePtr orderGroupIdNode;
  auto reversedOrderPlan =
      PlanBuilder()
          .values({data})
          .groupId({{"k2", "k1"}, {}}, {"a", "b"})
          .capturePlanNode(reversedOrderGroupIdNode)
          .singleAggregation(
              {"k2", "k1", "group_id"},
              {"count(1) as count_1", "sum(a) as sum_a", "max(b) as max_b"})
          .project({"k1", "k2", "count_1", "sum_a", "max_b"})
          .planNode();

  auto orderPlan =
      PlanBuilder()
          .values({data})
          .groupId({{"k1", "k2"}, {}}, {"a", "b"})
          .capturePlanNode(orderGroupIdNode)
          .singleAggregation(
              {"k1", "k2", "group_id"},
              {"count(1) as count_1", "sum(a) as sum_a", "max(b) as max_b"})
          .project({"k1", "k2", "count_1", "sum_a", "max_b"})
          .planNode();

  auto reversedOrderExpectedRowType =
      ROW({"k2", "k1", "a", "b", "group_id"},
          {BIGINT(), BIGINT(), BIGINT(), VARCHAR(), BIGINT()});
  auto orderExpectedRowType =
      ROW({"k1", "k2", "a", "b", "group_id"},
          {BIGINT(), BIGINT(), BIGINT(), VARCHAR(), BIGINT()});
  ASSERT_EQ(
      *reversedOrderGroupIdNode->outputType(), *reversedOrderExpectedRowType);
  ASSERT_EQ(*orderGroupIdNode->outputType(), *orderExpectedRowType);

  CursorParameters orderParams;
  orderParams.planNode = orderPlan;
  auto orderResult = readCursor(orderParams, [](Task*) {});

  CursorParameters reversedOrderParams;
  reversedOrderParams.planNode = reversedOrderPlan;
  auto reversedOrderResult = readCursor(reversedOrderParams, [](Task*) {});

  assertEqualResults(orderResult.second, reversedOrderResult.second);
}

TEST_F(AggregationTest, outputBatchSizeCheckWithSpill) {
  rowType_ = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer::Options options;
  options.vectorSize = 10;
  VectorFuzzer fuzzer(options, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType_));
  }

  auto plan = PlanBuilder()
                  .values(batches)
                  .singleAggregation({"c0", "c1"}, {"sum(c2)"})
                  .planNode();
  auto results = AssertQueryBuilder(plan).copyResults(pool_.get());

  {
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    uint64_t outputBufferSize = 10UL << 20;
    SCOPED_TRACE(fmt::format("outputBufferSize: {}", outputBufferSize));

    auto task = AssertQueryBuilder(plan)
                    .spillDirectory(tempDirectory->path)
                    .config(
                        QueryConfig::kPreferredOutputBatchBytes,
                        std::to_string(outputBufferSize))
                    .config(QueryConfig::kSpillEnabled, "true")
                    .config(QueryConfig::kAggregationSpillEnabled, "true")
                    // Set one spill partition to avoid the test flakiness.
                    .config(QueryConfig::kSpillPartitionBits, "0")
                    // Set the memory trigger limit to be a very small value.
                    .config(QueryConfig::kAggregationSpillMemoryThreshold, "1")
                    .assertResults(results);

    const auto opStats = task->taskStats().pipelineStats[0].operatorStats[1];
    ASSERT_EQ(opStats.outputVectors, 1);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
  {
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    uint64_t outputBufferSize = 1;
    SCOPED_TRACE(fmt::format("outputBufferSize: {}", outputBufferSize));

    auto task = AssertQueryBuilder(plan)
                    .spillDirectory(tempDirectory->path)
                    .config(
                        QueryConfig::kPreferredOutputBatchBytes,
                        std::to_string(outputBufferSize))
                    .config(QueryConfig::kSpillEnabled, "true")
                    .config(QueryConfig::kAggregationSpillEnabled, "true")
                    // Set one spill partition to avoid the test flakiness.
                    .config(QueryConfig::kSpillPartitionBits, "0")
                    // Set the memory trigger limit to be a very small value.
                    .config(QueryConfig::kAggregationSpillMemoryThreshold, "1")
                    .assertResults(results);

    const auto opStats = task->taskStats().pipelineStats[0].operatorStats[1];
    ASSERT_EQ(opStats.outputVectors, opStats.outputPositions);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

TEST_F(AggregationTest, distinctWithSpilling) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);
  auto spillDirectory = exec::test::TempDirectoryPath::create();
  core::PlanNodeId aggrNodeId;
  auto task = AssertQueryBuilder(duckDbQueryRunner_)
                  .spillDirectory(spillDirectory->path)
                  .config(QueryConfig::kSpillEnabled, "true")
                  .config(QueryConfig::kAggregationSpillEnabled, "true")
                  .config(QueryConfig::kTestingSpillPct, "100")
                  .plan(PlanBuilder()
                            .values(vectors)
                            .singleAggregation({"c0"}, {}, {})
                            .capturePlanNodeId(aggrNodeId)
                            .planNode())
                  .assertResults("SELECT distinct c0 FROM tmp");
  // Verify that spilling is not triggered.
  ASSERT_EQ(toPlanStats(task->taskStats()).at(aggrNodeId).spilledBytes, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

TEST_F(AggregationTest, preGroupedAggregationWithSpilling) {
  std::vector<RowVectorPtr> vectors;
  int64_t val = 0;
  for (int32_t i = 0; i < 4; ++i) {
    vectors.push_back(makeRowVector(
        {// Pre-grouped key.
         makeFlatVector<int64_t>(10, [&](auto /*row*/) { return val++ / 5; }),
         // Payload.
         makeFlatVector<int64_t>(10, [](auto row) { return row; }),
         makeFlatVector<int64_t>(10, [](auto row) { return row; })}));
  }
  createDuckDbTable(vectors);
  auto spillDirectory = exec::test::TempDirectoryPath::create();
  core::PlanNodeId aggrNodeId;
  auto task =
      AssertQueryBuilder(duckDbQueryRunner_)
          .spillDirectory(spillDirectory->path)
          .config(QueryConfig::kSpillEnabled, "true")
          .config(QueryConfig::kAggregationSpillEnabled, "true")
          .config(QueryConfig::kTestingSpillPct, "100")
          .plan(PlanBuilder()
                    .values(vectors)
                    .aggregation(
                        {"c0", "c1"},
                        {"c0"},
                        {"sum(c2)"},
                        {},
                        core::AggregationNode::Step::kSingle,
                        false)
                    .capturePlanNodeId(aggrNodeId)
                    .planNode())
          .assertResults("SELECT c0, c1, sum(c2) FROM tmp GROUP BY c0, c1");
  auto stats = task->taskStats().pipelineStats;
  // Verify that spilling is not triggered.
  ASSERT_EQ(toPlanStats(task->taskStats()).at(aggrNodeId).spilledBytes, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

TEST_F(AggregationTest, adaptiveOutputBatchRows) {
  int32_t defaultOutputBatchRows = 10;
  vector_size_t size = defaultOutputBatchRows * 5;
  auto vectors = std::vector<RowVectorPtr>(
      8,
      makeRowVector(
          {"k0", "c0"},
          {makeFlatVector<int32_t>(size, [&](auto row) { return row; }),
           makeFlatVector<int8_t>(size, [&](auto row) { return row % 2; })}));

  createDuckDbTable(vectors);

  auto plan = PlanBuilder()
                  .values(vectors)
                  .singleAggregation({"k0"}, {"sum(c0)"})
                  .planNode();

  // Test setting larger output batch bytes will create batches of greater
  // number of rows.
  {
    auto outputBatchBytes = "1000";
    auto task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(QueryConfig::kPreferredOutputBatchBytes, outputBatchBytes)
            .assertResults("SELECT k0, SUM(c0) FROM tmp GROUP BY k0");

    auto aggOpStats = task->taskStats().pipelineStats[0].operatorStats[1];
    ASSERT_GT(
        aggOpStats.outputPositions / aggOpStats.outputVectors,
        defaultOutputBatchRows);
  }

  // Test setting smaller output batch bytes will create batches of fewer
  // number of rows.
  {
    auto outputBatchBytes = "1";
    auto task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(QueryConfig::kPreferredOutputBatchBytes, outputBatchBytes)
            .assertResults("SELECT k0, SUM(c0) FROM tmp GROUP BY k0");

    auto aggOpStats = task->taskStats().pipelineStats[0].operatorStats[1];
    ASSERT_LT(
        aggOpStats.outputPositions / aggOpStats.outputVectors,
        defaultOutputBatchRows);
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, reclaimDuringInputProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), VARCHAR()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  struct {
    // 0: trigger reclaim with some input processed.
    // 1: trigger reclaim after all the inputs processed.
    int triggerCondition;
    bool spillEnabled;
    bool expectedReclaimable;

    std::string debugString() const {
      return fmt::format(
          "triggerCondition {}, spillEnabled {}, expectedReclaimable {}",
          triggerCondition,
          spillEnabled,
          expectedReclaimable);
    }
  } testSettings[] = {
      {0, true, true}, {0, false, false}, {1, true, true}, {1, false, false}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<int> numInputs{0};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "Aggregation") {
            ASSERT_FALSE(testOp->canReclaim());
            return;
          }
          op = testOp;
          ++numInputs;
          if (testData.triggerCondition == 0) {
            if (numInputs != 2) {
              return;
            }
          }
          if (testData.triggerCondition == 1) {
            if (numInputs != numBatches) {
              return;
            }
          }
          ASSERT_EQ(op->canReclaim(), testData.expectedReclaimable);
          uint64_t reclaimableBytes{0};
          const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
          ASSERT_EQ(reclaimable, testData.expectedReclaimable);
          if (testData.expectedReclaimable) {
            ASSERT_GT(reclaimableBytes, 0);
          } else {
            ASSERT_EQ(reclaimableBytes, 0);
          }
          testWait.notify();
          driverWait.wait(driverWaitKey);
        })));

    std::thread taskThread([&]() {
      if (testData.spillEnabled) {
        auto task = AssertQueryBuilder(
                        PlanBuilder()
                            .values(batches)
                            .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                            .planNode())
                        .queryCtx(queryCtx)
                        .spillDirectory(tempDirectory->path)
                        .config(QueryConfig::kSpillEnabled, "true")
                        .config(QueryConfig::kAggregationSpillEnabled, "true")
                        .config(core::QueryConfig::kSpillPartitionBits, "2")
                        .maxDrivers(1)
                        .assertResults(expectedResult);
      } else {
        auto task = AssertQueryBuilder(
                        PlanBuilder()
                            .values(batches)
                            .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                            .planNode())
                        .queryCtx(queryCtx)
                        .maxDrivers(1)
                        .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    driverWait.notify();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), testData.expectedReclaimable);
    ASSERT_EQ(reclaimable, testData.expectedReclaimable);
    if (testData.expectedReclaimable) {
      ASSERT_GT(reclaimableBytes, 0);
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
    }

    if (testData.expectedReclaimable) {
      const auto usedMemory = op->pool()->currentBytes();
      op->reclaim(folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_));
      // The hash table itself in the grouping set is not cleared so it still
      // uses some memory.
      ASSERT_LT(op->pool()->currentBytes(), usedMemory);
    } else {
      VELOX_ASSERT_THROW(
          op->reclaim(
              folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_)),
          "");
    }

    Task::resume(task);

    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    if (testData.expectedReclaimable) {
      ASSERT_GT(stats[0].operatorStats[1].spilledBytes, 0);
      ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 4);
    } else {
      ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
      ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    }
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, reclaimDuringReserve) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), VARCHAR()});
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    const size_t size = i == 0 ? 100 : 40000;
    VectorFuzzer fuzzer({.vectorSize = size}, pool());
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  auto tempDirectory = exec::test::TempDirectoryPath::create();
  auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
  queryCtx->testingOverrideMemoryPool(
      memory::defaultMemoryManager().addRootPool(
          queryCtx->queryId(), kMaxBytes));
  auto expectedResult =
      AssertQueryBuilder(PlanBuilder()
                             .values(batches)
                             .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                             .planNode())
          .queryCtx(queryCtx)
          .copyResults(pool_.get());

  folly::EventCount driverWait;
  auto driverWaitKey = driverWait.prepareWait();
  folly::EventCount testWait;
  auto testWaitKey = testWait.prepareWait();

  Operator* op;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::addInput",
      std::function<void(Operator*)>(([&](Operator* testOp) {
        if (testOp->operatorType() != "Aggregation") {
          ASSERT_FALSE(testOp->canReclaim());
          return;
        }
        op = testOp;
      })));

  std::atomic<bool> injectOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::common::memory::MemoryPoolImpl::maybeReserve",
      std::function<void(memory::MemoryPoolImpl*)>(
          ([&](memory::MemoryPoolImpl* pool) {
            ASSERT_TRUE(op != nullptr);
            const std::string re(".*Aggregation");
            if (!RE2::FullMatch(pool->name(), re)) {
              return;
            }
            if (!injectOnce.exchange(false)) {
              return;
            }
            ASSERT_TRUE(op->canReclaim());
            uint64_t reclaimableBytes{0};
            const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
            ASSERT_TRUE(reclaimable);
            ASSERT_GT(reclaimableBytes, 0);
            auto* driver = op->testingOperatorCtx()->driver();
            SuspendedSection suspendedSection(driver);
            testWait.notify();
            driverWait.wait(driverWaitKey);
          })));

  std::thread taskThread([&]() {
    AssertQueryBuilder(PlanBuilder()
                           .values(batches)
                           .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                           .planNode())
        .queryCtx(queryCtx)
        .spillDirectory(tempDirectory->path)
        .config(QueryConfig::kSpillEnabled, "true")
        .config(QueryConfig::kAggregationSpillEnabled, "true")
        .config(core::QueryConfig::kSpillPartitionBits, "2")
        .maxDrivers(1)
        .assertResults(expectedResult);
  });

  testWait.wait(testWaitKey);
  ASSERT_TRUE(op != nullptr);
  auto task = op->testingOperatorCtx()->task();
  auto taskPauseWait = task->requestPause();
  taskPauseWait.wait();

  uint64_t reclaimableBytes{0};
  const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
  ASSERT_TRUE(op->canReclaim());
  ASSERT_TRUE(reclaimable);
  ASSERT_GT(reclaimableBytes, 0);

  const auto usedMemory = op->pool()->currentBytes();
  op->reclaim(folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_));
  // The hash table itself in the grouping set is not cleared so it still
  // uses some memory.
  ASSERT_LT(op->pool()->currentBytes(), usedMemory);

  driverWait.notify();
  Task::resume(task);
  taskThread.join();

  auto stats = task->taskStats().pipelineStats;
  ASSERT_GT(stats[0].operatorStats[1].spilledBytes, 0);
  ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 4);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

DEBUG_ONLY_TEST_F(AggregationTest, reclaimDuringAllocation) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), VARCHAR()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  std::vector<bool> enableSpillings = {false, true};
  for (const auto enableSpilling : enableSpillings) {
    SCOPED_TRACE(fmt::format("enableSpilling {}", enableSpilling));

    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "Aggregation") {
            ASSERT_FALSE(testOp->canReclaim());
            return;
          }
          op = testOp;
        })));

    std::atomic<bool> injectOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::common::memory::MemoryPoolImpl::allocateNonContiguous",
        std::function<void(memory::MemoryPoolImpl*)>(
            ([&](memory::MemoryPoolImpl* pool) {
              ASSERT_TRUE(op != nullptr);
              const std::string re(".*Aggregation");
              if (!RE2::FullMatch(pool->name(), re)) {
                return;
              }
              if (!injectOnce.exchange(false)) {
                return;
              }
              ASSERT_EQ(op->canReclaim(), enableSpilling);
              uint64_t reclaimableBytes{0};
              const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
              ASSERT_EQ(reclaimable, enableSpilling);
              if (enableSpilling) {
                ASSERT_GT(reclaimableBytes, 0);
              } else {
                ASSERT_EQ(reclaimableBytes, 0);
              }
              auto* driver = op->testingOperatorCtx()->driver();
              SuspendedSection suspendedSection(driver);
              testWait.notify();
              driverWait.wait(driverWaitKey);
            })));

    std::thread taskThread([&]() {
      if (enableSpilling) {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(QueryConfig::kSpillEnabled, "true")
            .config(QueryConfig::kAggregationSpillEnabled, "true")
            .config(core::QueryConfig::kSpillPartitionBits, "2")
            .maxDrivers(1)
            .assertResults(expectedResult);
      } else {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), enableSpilling);
    ASSERT_EQ(reclaimable, enableSpilling);

    if (enableSpilling) {
      ASSERT_GT(reclaimableBytes, 0);
      const auto usedMemory = op->pool()->currentBytes();
      op->reclaim(folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_));
      // No reclaim as the operator is under non-reclaimable section.
      ASSERT_EQ(usedMemory, op->pool()->currentBytes());
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
      VELOX_ASSERT_THROW(
          op->reclaim(
              folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_)),
          "");
    }

    driverWait.notify();
    Task::resume(task);

    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
    ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, reclaimDuringOutputProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  std::vector<bool> enableSpillings = {false, true};
  for (const auto enableSpilling : enableSpillings) {
    SCOPED_TRACE(fmt::format("enableSpilling {}", enableSpilling));
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<bool> injectOnce{true};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::noMoreInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "Aggregation") {
            ASSERT_FALSE(testOp->canReclaim());
            return;
          }
          op = testOp;
          if (!injectOnce.exchange(false)) {
            return;
          }
          ASSERT_EQ(op->canReclaim(), enableSpilling);
          uint64_t reclaimableBytes{0};
          const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
          ASSERT_EQ(reclaimable, enableSpilling);
          if (enableSpilling) {
            ASSERT_GT(reclaimableBytes, 0);
          } else {
            ASSERT_EQ(reclaimableBytes, 0);
          }
          testWait.notify();
          driverWait.wait(driverWaitKey);
        })));

    std::thread taskThread([&]() {
      if (enableSpilling) {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(QueryConfig::kSpillEnabled, "true")
            .config(QueryConfig::kAggregationSpillEnabled, "true")
            .config(core::QueryConfig::kSpillPartitionBits, "2")
            .maxDrivers(1)
            .assertResults(expectedResult);
      } else {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    driverWait.notify();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), enableSpilling);
    ASSERT_EQ(reclaimable, enableSpilling);
    if (enableSpilling) {
      ASSERT_GT(reclaimableBytes, 0);
      const auto usedMemory = op->pool()->currentBytes();
      op->reclaim(folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_));
      // No reclaim as the operator has started output processing.
      ASSERT_EQ(usedMemory, op->pool()->currentBytes());
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
      VELOX_ASSERT_THROW(
          op->reclaim(
              folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_)),
          "");
    }

    Task::resume(task);

    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
    ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, reclaimWithEmptyAggregationTable) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }
  std::vector<bool> enableSpillings = {false, true};
  for (const auto enableSpilling : enableSpillings) {
    SCOPED_TRACE(fmt::format("enableSpilling {}", enableSpilling));
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    core::PlanNodeId aggregationPlanNodeId;
    auto aggregationPlan =
        PlanBuilder()
            .values(batches)
            .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
            .capturePlanNodeId(aggregationPlanNodeId)
            .planNode();

    std::atomic<bool> injectOnce{true};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal",
        std::function<void(Driver*)>(([&](Driver* driver) {
          if (driver->findOperator(aggregationPlanNodeId) == nullptr) {
            return;
          }
          if (!injectOnce.exchange(false)) {
            return;
          }
          op = driver->findOperator(aggregationPlanNodeId);
          testWait.notify();
          driverWait.wait(driverWaitKey);
        })));

    std::thread taskThread([&]() {
      if (enableSpilling) {
        AssertQueryBuilder(nullptr)
            .plan(aggregationPlan)
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(QueryConfig::kSpillEnabled, "true")
            .config(QueryConfig::kAggregationSpillEnabled, "true")
            .config(core::QueryConfig::kSpillPartitionBits, "2")
            .maxDrivers(1)
            .assertResults(expectedResult);
      } else {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    driverWait.notify();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), enableSpilling);
    ASSERT_EQ(reclaimable, enableSpilling);
    if (enableSpilling) {
      ASSERT_EQ(reclaimableBytes, 0);
      const auto usedMemory = op->pool()->currentBytes();
      op->reclaim(folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_));
      // No reclaim as the operator has started output processing.
      ASSERT_EQ(usedMemory, op->pool()->currentBytes());
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
      VELOX_ASSERT_THROW(
          op->reclaim(
              folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_)),
          "");
    }

    Task::resume(task);

    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
    ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, abortDuringOutputProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  struct {
    bool abortFromRootMemoryPool;
    int numDrivers;

    std::string debugString() const {
      return fmt::format(
          "abortFromRootMemoryPool {} numDrivers {}",
          abortFromRootMemoryPool,
          numDrivers);
    }
  } testSettings[] = {{true, 1}, {false, 1}, {true, 4}, {false, 4}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<bool> injectOnce{true};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::noMoreInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "Aggregation") {
            return;
          }
          op = testOp;
          if (!injectOnce.exchange(false)) {
            return;
          }
          auto* driver = op->testingOperatorCtx()->driver();
          ASSERT_EQ(
              driver->task()->enterSuspended(driver->state()),
              StopReason::kNone);
          testWait.notify();
          driverWait.wait(driverWaitKey);
          ASSERT_EQ(
              driver->task()->leaveSuspended(driver->state()),
              StopReason::kAlreadyTerminated);
          VELOX_MEM_POOL_ABORTED(op->pool());
        })));

    std::thread taskThread([&]() {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(
              PlanBuilder()
                  .values(batches)
                  .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                  .planNode())
              .queryCtx(queryCtx)
              .maxDrivers(testData.numDrivers)
              .assertResults(expectedResult),
          "");
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    testData.abortFromRootMemoryPool ? queryCtx->pool()->abort()
                                     : op->pool()->abort();
    ASSERT_TRUE(op->pool()->aborted());
    ASSERT_TRUE(queryCtx->pool()->aborted());
    ASSERT_EQ(queryCtx->pool()->currentBytes(), 0);
    driverWait.notify();
    taskThread.join();
    task.reset();
    Task::testingWaitForAllTasksToBeDeleted();
  }
}

DEBUG_ONLY_TEST_F(AggregationTest, abortDuringInputgProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  struct {
    bool abortFromRootMemoryPool;
    int numDrivers;

    std::string debugString() const {
      return fmt::format(
          "abortFromRootMemoryPool {} numDrivers {}",
          abortFromRootMemoryPool,
          numDrivers);
    }
  } testSettings[] = {{true, 1}, {false, 1}, {true, 4}, {false, 4}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<int> numInputs{0};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "Aggregation") {
            return;
          }
          op = testOp;
          ++numInputs;
          if (numInputs != 2) {
            return;
          }
          auto* driver = op->testingOperatorCtx()->driver();
          ASSERT_EQ(
              driver->task()->enterSuspended(driver->state()),
              StopReason::kNone);
          testWait.notify();
          driverWait.wait(driverWaitKey);
          ASSERT_EQ(
              driver->task()->leaveSuspended(driver->state()),
              StopReason::kAlreadyTerminated);
          VELOX_MEM_POOL_ABORTED(op->pool());
        })));

    std::thread taskThread([&]() {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(
              PlanBuilder()
                  .values(batches)
                  .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                  .planNode())
              .queryCtx(queryCtx)
              .maxDrivers(testData.numDrivers)
              .assertResults(expectedResult),
          "");
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    testData.abortFromRootMemoryPool ? queryCtx->pool()->abort()
                                     : op->pool()->abort();
    ASSERT_TRUE(op->pool()->aborted());
    ASSERT_TRUE(queryCtx->pool()->aborted());
    ASSERT_EQ(queryCtx->pool()->currentBytes(), 0);
    driverWait.notify();
    taskThread.join();
    task.reset();
    Task::testingWaitForAllTasksToBeDeleted();
  }
}

} // namespace facebook::velox::exec::test
