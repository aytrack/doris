// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
// This file is copied from
// https://github.com/ClickHouse/ClickHouse/blob/master/src/Functions/Nullif.cpp
// and modified by Doris

#include <glog/logging.h>
#include <stddef.h>

#include <algorithm>
#include <boost/iterator/iterator_facade.hpp>
#include <memory>
#include <vector>

#include "common/status.h"
#include "runtime/runtime_state.h"
#include "vec/aggregate_functions/aggregate_function.h"
#include "vec/columns/column.h"
#include "vec/columns/column_const.h"
#include "vec/common/assert_cast.h"
#include "vec/core/block.h"
#include "vec/core/column_numbers.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/columns_with_type_and_name.h"
#include "vec/core/types.h"
#include "vec/data_types/data_type.h"
#include "vec/data_types/data_type_nothing.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_type_number.h"
#include "vec/functions/function.h"
#include "vec/functions/simple_function_factory.h"

namespace doris {
class FunctionContext;
} // namespace doris

namespace doris::vectorized {
class FunctionNullIf : public IFunction {
public:
    static constexpr auto name = "nullif";

    static FunctionPtr create() { return std::make_shared<FunctionNullIf>(); }

    String get_name() const override { return name; }

    size_t get_number_of_arguments() const override { return 2; }

    bool use_default_implementation_for_nulls() const override { return false; }

    DataTypePtr get_return_type_impl(const DataTypes& arguments) const override {
        return make_nullable(arguments[0]);
    }

    static DataTypePtr get_return_type_for_equal(const ColumnsWithTypeAndName& arguments) {
        ColumnsWithTypeAndName args_without_low_cardinality(arguments);

        for (ColumnWithTypeAndName& arg : args_without_low_cardinality) {
            bool is_const = arg.column && is_column_const(*arg.column);
            if (is_const) {
                arg.column = assert_cast<const ColumnConst&>(*arg.column).remove_low_cardinality();
            }
        }

        if (!arguments.empty()) {
            if (have_null_column(arguments)) {
                return make_nullable(std::make_shared<doris::vectorized::DataTypeUInt8>());
            }
        }

        return std::make_shared<doris::vectorized::DataTypeUInt8>();
    }

    // nullIf(col1, col2) == if(col1 = col2, NULL, col1)
    Status execute_impl(FunctionContext* context, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t input_rows_count) const override {
        const ColumnsWithTypeAndName eq_columns {
                block.get_by_position(arguments[0]),
                block.get_by_position(arguments[1]),
        };
        auto result_type = get_return_type_for_equal(eq_columns);
        Block eq_temporary_block({block.get_by_position(arguments[0]),
                                  block.get_by_position(arguments[1]),
                                  {nullptr, result_type, ""}});

        auto equals_func = SimpleFunctionFactory::instance().get_function(
                "eq", eq_columns, result_type,
                {.enable_decimal256 = context->state()->enable_decimal256()});
        DCHECK(equals_func);
        RETURN_IF_ERROR(
                equals_func->execute(context, eq_temporary_block, {0, 1}, 2, input_rows_count));

        const ColumnWithTypeAndName new_result_column {
                block.get_by_position(result),
        };

        const ColumnWithTypeAndName if_column {
                eq_temporary_block.get_by_position(2),
        };
        const ColumnsWithTypeAndName if_columns {
                if_column,
                {block.get_by_position(result).type->create_column_const_with_default_value(
                         input_rows_count),
                 block.get_by_position(result).type, "NULL"},
                block.get_by_position(arguments[0]),
        };

        Block temporary_block(
                {if_column,
                 {block.get_by_position(result).type->create_column_const_with_default_value(
                          input_rows_count),
                  block.get_by_position(result).type, "NULL"},
                 block.get_by_position(arguments[0]),
                 new_result_column});

        auto func_if = SimpleFunctionFactory::instance().get_function(
                "if", if_columns, new_result_column.type,
                {.enable_decimal256 = context->state()->enable_decimal256()});
        DCHECK(func_if);
        RETURN_IF_ERROR(func_if->execute(context, temporary_block, {0, 1, 2}, 3, input_rows_count));
        block.get_by_position(result).column = temporary_block.get_by_position(3).column;

        return Status::OK();
    }
};

void register_function_nullif(SimpleFunctionFactory& factory) {
    factory.register_function<FunctionNullIf>();
}
} // namespace doris::vectorized