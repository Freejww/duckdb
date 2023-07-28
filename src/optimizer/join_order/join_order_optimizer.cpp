#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/optimizer/join_order/plan_enumerator.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/list.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

unique_ptr<LogicalOperator> JoinOrderOptimizer::Optimize(unique_ptr<LogicalOperator> plan,
                                                         optional_ptr<RelationStats> stats) {

	// make sure query graph manager has not extracted a relation graph already
	LogicalOperator *op = plan.get();

	// extract the relations that go into the hyper graph.
	// We optimize the children of any non-reorderable operations we come across.
	bool reorderable = query_graph_manager.Build(op);

	// get relation_stats here since the reconstruction process will move all of the relations.
	auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();
	unique_ptr<LogicalOperator> new_logical_plan = nullptr;

	if (reorderable) {
		// query graph now has filters and relations
		auto cost_model = CostModel(query_graph_manager);

		// Initialize a plan enumerator.
		auto plan_enumerator = PlanEnumerator(query_graph_manager, cost_model, query_graph_manager.GetQueryGraph());

		// Initialize the leaf/single node plans
		// TODO: the translation of SingleJoinNodeRelations to JoinNode should not happen in the
		//  enumerator. The enumerator
		plan_enumerator.InitLeafPlans();
#ifdef DEBUG
		query_graph_manager.relation_manager.PrintRelationStats();
		cost_model.cardinality_estimator.PrintRelationToTdomInfo();
#endif

		// Ask the plan enumerator to enumerate a number of join orders
		auto final_plan = plan_enumerator.SolveJoinOrder(context.config.force_no_cross_product);
		// TODO: add in the check that if no plan exists, you have to add a cross product.

		// now reconstruct a logical plan from the query graph plan
		new_logical_plan = query_graph_manager.Reconstruct(std::move(plan), *final_plan);
	} else {
		new_logical_plan = std::move(plan);
		if (relation_stats.size() == 1) {
			new_logical_plan->estimated_cardinality = relation_stats.at(0).cardinality;
		}
	}

	// Propagate up a stats object from the top of the new_logical_plan if stats exist.
	if (stats) {
		auto cardinality = new_logical_plan->EstimateCardinality(context);
		auto bindings = new_logical_plan->GetColumnBindings();
		auto new_stats = RelationStatisticsHelper::CombineStatsOfReorderableOperator(bindings, relation_stats);
		new_stats.cardinality = MaxValue(cardinality, new_stats.cardinality);
		RelationStatisticsHelper::CopyRelationStats(stats, new_stats);
	}

	return new_logical_plan;
}

} // namespace duckdb
