#include "execution/operator/aggregate/physical_window.hpp"

#include "execution/expression_executor.hpp"
#include "parser/expression/aggregate_expression.hpp"
#include "parser/expression/constant_expression.hpp"
#include "parser/expression/columnref_expression.hpp"

#include "common/vector_operations/vector_operations.hpp"
#include "parser/expression/window_expression.hpp"

using namespace duckdb;
using namespace std;

PhysicalWindow::PhysicalWindow(LogicalOperator &op, vector<unique_ptr<Expression>> select_list,
                                     PhysicalOperatorType type)
    : PhysicalOperator(type, op.types), select_list(std::move(select_list)) {

	// TODO: check we have at least one window aggr in the select list otherwise this is pointless
}

// TODO what if we have no PARTITION BY/ORDER?
// this implements a sorted window functions variant

void PhysicalWindow::_GetChunk(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state_) {
	auto state = reinterpret_cast<PhysicalWindowOperatorState *>(state_);
	// we kind of need to materialize the intermediate here.
	ChunkCollection &big_data = state->tuples;

	if (state->position == 0) {
		// materialize intermediate
		do {
			children[0]->GetChunk(context, state->child_chunk, state->child_state.get());
			big_data.Append(state->child_chunk);
		} while (state->child_chunk.size() != 0);

		// TODO: move this to state constructor
		state->sorted_vector.resize(select_list.size());
		state->partition_ids.resize(select_list.size());
		state->serializers.resize(select_list.size());

		for (size_t expr_idx = 0; expr_idx < select_list.size(); expr_idx++) {
			if (select_list[expr_idx]->GetExpressionClass() != ExpressionClass::WINDOW) {
				continue;
			}
			auto wexpr = reinterpret_cast<WindowExpression *>(select_list[expr_idx].get());
			vector<TypeId> sort_types;
			vector<Expression *> exprs;
			OrderByDescription odesc;

			for (size_t prt_idx = 0; prt_idx < wexpr->partitions.size(); prt_idx++) {
				auto &pexpr = wexpr->partitions[prt_idx];
				sort_types.push_back(pexpr->return_type);
				exprs.push_back(pexpr.get());
				odesc.orders.push_back(OrderByNode(OrderType::ASCENDING, make_unique_base<Expression, ColumnRefExpression>(pexpr->return_type, exprs.size()-1)));

			}

			auto serializer = TupleSerializer(sort_types);

			for (size_t ord_idx = 0; ord_idx < wexpr->ordering.orders.size(); ord_idx++) {
				auto &oexpr = wexpr->ordering.orders[ord_idx].expression;
				sort_types.push_back(oexpr->return_type);
				exprs.push_back(oexpr.get());
				odesc.orders.push_back(OrderByNode(wexpr->ordering.orders[ord_idx].type, make_unique_base<Expression, ColumnRefExpression>(oexpr->return_type, exprs.size()-1)));
			}
			assert(sort_types.size() > 0);

			// TODO this is using quite a lot of memory, may not be neccessary
			uint8_t partition_data[big_data.count][serializer.TupleSize()];
			uint8_t *partition_elements[big_data.count];
			for (size_t prt_idx = 0; prt_idx < big_data.count; prt_idx++) {
				partition_elements[prt_idx] = partition_data[prt_idx];
			}

			size_t partition_offset = 0;
			ChunkCollection sort_collection;

			for (size_t i = 0; i < big_data.chunks.size(); i++) {
				DataChunk sort_chunk;
				sort_chunk.Initialize(sort_types);

				// the partition/order by in the window by might be expressions, evaluate them
				ExpressionExecutor executor(*big_data.chunks[i], context);
				executor.Execute(sort_chunk, [&](size_t i) {
					return exprs[i];
				}, exprs.size());
				sort_chunk.Verify();

				// serialize the partition columns into bytes so we can compute partition ids below efficiently
				size_t ser_offset = 0;
				for (size_t ser_col = 0; ser_col < serializer.TypeSize(); ser_col++) {
					serializer.SerializeColumn(sort_chunk, &partition_elements[partition_offset], ser_col, ser_offset);
				}

				sort_collection.Append(sort_chunk);
				partition_offset += sort_chunk.size();
			}

			assert (sort_collection.count == big_data.count);

			// TODO: combine those?
			state->sorted_vector[expr_idx] = unique_ptr<uint64_t[]>(new uint64_t[sort_collection.count]);
			state->partition_ids[expr_idx] = unique_ptr<uint64_t[]>(new uint64_t[sort_collection.count]);

			sort_collection.Sort(odesc, state->sorted_vector[expr_idx].get());

			// compare partition bytes according to sort order to figure out where a new group begins
			uint8_t *prev = partition_elements[0];
			size_t partition_id = 0;
			for (size_t t_idx = 0; t_idx < big_data.count; t_idx++) {
				if (serializer.Compare(prev, partition_elements[state->sorted_vector[expr_idx][t_idx]]) != 0) {
					partition_id++;
				}
				prev = partition_elements[state->sorted_vector[expr_idx][t_idx]];
				state->partition_ids[expr_idx][state->sorted_vector[expr_idx][t_idx]] = partition_id;
			}

			for (size_t t_idx = 0; t_idx < big_data.count; t_idx++) {
				printf("%llu %llu\n", state->sorted_vector[expr_idx][t_idx], state->partition_ids[expr_idx][t_idx]);
			}
		}
	}

	if (state->position >= big_data.count) {
		return;
	}
	// only actually compute windows now because they may never be needed after all
	auto& ret_ch = big_data.GetChunk(state->position);
	ret_ch.Copy(chunk);

	for (size_t row_idx = 0; row_idx < ret_ch.size(); row_idx++) {
		for (size_t expr_idx = 0; expr_idx < select_list.size(); expr_idx++) {
			if (select_list[expr_idx]->GetExpressionClass() != ExpressionClass::WINDOW) {
				continue;
			}
			// FIXME this only computes partition ID for now
			// from the values in this row, compute window boundaries
			// in the simplest case, they are just the partition boundaries
			chunk.data[expr_idx].SetValue(row_idx, Value::BIGINT(state->partition_ids[expr_idx][row_idx + state->position]));
		}
	}

	state->position += STANDARD_VECTOR_SIZE;
}


unique_ptr<PhysicalOperatorState> PhysicalWindow::GetOperatorState(ExpressionExecutor *parent) {
	return make_unique<PhysicalWindowOperatorState>(children[0].get(), parent);

}

