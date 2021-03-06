/******************************************************************************
 * quotient_graph_refinement.cpp
 *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 *
 ******************************************************************************
 * Copyright (C) 2013-2015 Christian Schulz <christian.schulz@kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <unordered_map>

#include "2way_fm_refinement/two_way_fm.h"
#include "complete_boundary.h"
#include "data_structure/parallel/time.h"
#include "flow_refinement/two_way_flow_refinement.h"
#include "quality_metrics.h"
#include "quotient_graph_refinement.h"
#include "quotient_graph_scheduling/active_block_quotient_graph_scheduler.h"
#include "quotient_graph_scheduling/simple_quotient_graph_scheduler.h"
#include "uncoarsening/refinement/kway_graph_refinement/kway_graph_refinement.h"
#include "uncoarsening/refinement/kway_graph_refinement/multitry_kway_fm.h"
#include "uncoarsening/refinement/parallel_kway_graph_refinement/multitry_kway_fm.h"

#include "quality_metrics.h"

//double quotient_graph_refinement::total_time_two_way(0.0);

quotient_graph_refinement::quotient_graph_refinement() {

}

quotient_graph_refinement::~quotient_graph_refinement() {

}

void quotient_graph_refinement::setup_start_nodes(graph_access & G,
                PartitionID partition,
                boundary_pair & bp,
                complete_boundary & boundary,
                boundary_starting_nodes & start_nodes) {

        start_nodes.resize(boundary.size(partition, &bp));
        NodeID cur_idx = 0;

        PartitionID lhs = bp.lhs;
        PartitionID rhs = bp.rhs;
        PartialBoundary & lhs_b = boundary.getDirectedBoundary(partition, lhs, rhs);

        forall_boundary_nodes(lhs_b, cur_bnd_node) {
                ASSERT_EQ(G.getPartitionIndex(cur_bnd_node), partition);
                start_nodes[cur_idx++] = cur_bnd_node;
        } endfor
}

EdgeWeight quotient_graph_refinement::perform_refinement_all(PartitionConfig& config, graph_access& G, complete_boundary& boundary) {
        CLOCK_START;
        EdgeWeight overall_improvement = 0;
        if (config.refinement_scheduling_algorithm == REFINEMENT_SCHEDULING_ACTIVE_BLOCKS_REF_KWAY) {
                std::cout << "START KWAY" << std::endl;
                auto kway_ref = get_multitry_kway_fm_instance(config, G, boundary);
                overall_improvement = kway_ref->perform_refinement(config, G, boundary, config.global_multitry_rounds,
                                                                   true, config.kway_adaptive_limits_alpha);

                std::cout << "Cut improvement time\t" << CLOCK_END_TIME << std::endl;
                std::cout << "Cut improvement\t" << overall_improvement << std::endl;
        }
        return overall_improvement;
}

EdgeWeight quotient_graph_refinement::perform_refinement(PartitionConfig & config, graph_access & G, complete_boundary & boundary) {
        EdgeWeight overall_improvement = 0;
        {
                ASSERT_TRUE(boundary.assert_bnodes_in_boundaries());
                ASSERT_TRUE(boundary.assert_boundaries_are_bnodes());

                QuotientGraphEdges qgraph_edges;
                boundary.getQuotientGraphEdges(qgraph_edges);
                quotient_graph_scheduling* scheduler = NULL;

                int factor = ceil(config.bank_account_factor * qgraph_edges.size());
                switch (config.refinement_scheduling_algorithm) {
                        case REFINEMENT_SCHEDULING_FAST:
                                scheduler = new simple_quotient_graph_scheduler(config, qgraph_edges, factor);
                                break;
                        case REFINEMENT_SCHEDULING_ACTIVE_BLOCKS:
                                scheduler = new active_block_quotient_graph_scheduler(config, qgraph_edges, factor);
                                break;
                        case REFINEMENT_SCHEDULING_ACTIVE_BLOCKS_REF_KWAY:
                                scheduler = new active_block_quotient_graph_scheduler(config, qgraph_edges, factor);
                                break;
                }

                //EdgeWeight overall_improvement = 0;
                unsigned int no_of_pairwise_improvement_steps = 0;
                quality_metrics qm;
                EdgeWeight cut_improvement = 0;

                CLOCK_START;
                double time = 0.0;
                auto kway_ref = get_multitry_kway_fm_instance(config, G, boundary);
                time += CLOCK_END_TIME;

                double time_two_way = 0.0;

                do {
                        no_of_pairwise_improvement_steps++;
                        // ********** preconditions ********************
                        ASSERT_TRUE(boundary.assert_bnodes_in_boundaries());
                        ASSERT_TRUE(boundary.assert_boundaries_are_bnodes());
                        // *************** end *************************

                        if (scheduler->hasFinished()) break; //fetch the case where we have no qgraph edges

                        boundary_pair& bp = scheduler->getNext();
                        PartitionID lhs = bp.lhs;
                        PartitionID rhs = bp.rhs;

                        NodeWeight lhs_part_weight = boundary.getBlockWeight(lhs);
                        NodeWeight rhs_part_weight = boundary.getBlockWeight(rhs);

                        EdgeWeight initial_cut_value = boundary.getEdgeCut(&bp);
                        if (initial_cut_value < 0)
                                continue; // quick fix, for bug 02 (very rare cross combine bug / coarsest level) !

                        bool something_changed = false;

#ifndef NDEBUG
                        EdgeWeight oldcut = initial_cut_value;
#endif

                        PartitionConfig cfg = config;
//                        COMMENT OF TWO REFINEMENT BEGIN;
                        CLOCK_START;
                        EdgeWeight improvement = 0;
                        if (cfg.quotient_graph_two_way_refinement) {
                                improvement = perform_a_two_way_refinement(cfg, G, boundary, bp,
                                                                           lhs, rhs,
                                                                           lhs_part_weight, rhs_part_weight,
                                                                           initial_cut_value, something_changed);
                        }

                        time_two_way += CLOCK_END_TIME;

                        overall_improvement += improvement;
//                        COMMENT OF TWO REFINEMENT END;

                        EdgeWeight multitry_improvement = 0;
                        if (config.refinement_scheduling_algorithm == REFINEMENT_SCHEDULING_ACTIVE_BLOCKS_REF_KWAY) {
                                std::unordered_map<PartitionID, PartitionID> touched_blocks;

                                //int old_cut = qm.edge_cut(G);
                                CLOCK_START;
                                multitry_improvement = kway_ref->perform_refinement_around_parts(cfg, G,
                                                                                                 boundary, true,
                                                                                                 config.local_multitry_fm_alpha,
                                                                                                 lhs, rhs,
                                                                                                 touched_blocks);
                                time += CLOCK_END_TIME;
                                cut_improvement += multitry_improvement;
                                //int cut_diff = old_cut - qm.edge_cut(G);
                                //std::cout << "Improved multitry:\t" << multitry_improvement << ", expected:\t" << cut_diff << std::endl;
                                //ALWAYS_ASSERT(cut_diff == multitry_improvement);

                                if (multitry_improvement > 0) {
                                        ((active_block_quotient_graph_scheduler*) scheduler)->activate_blocks(
                                                touched_blocks);
                                }

                        }

                        qgraph_edge_statistics stat(improvement, &bp, something_changed);
                        scheduler->pushStatistics(stat);

                        //**************** assertions / postconditions **************************
                        ASSERT_TRUE(oldcut - improvement == qm.edge_cut(G, lhs, rhs)
                                    || config.refinement_scheduling_algorithm ==
                                       REFINEMENT_SCHEDULING_ACTIVE_BLOCKS_REF_KWAY);
                        ASSERT_TRUE(boundary.assert_bnodes_in_boundaries());
                        ASSERT_TRUE(boundary.assert_boundaries_are_bnodes());
                        ASSERT_TRUE(boundary.getBlockNoNodes(lhs) > 0);
                        ASSERT_TRUE(boundary.getBlockNoNodes(rhs) > 0);
                        //*************************** end ****************************************
                } while (!scheduler->hasFinished());
                //std::cout << "Cut improvement time\t" << time << std::endl;
                //std::cout << "Two way time\t" << time_two_way << std::endl;
                //total_time_two_way += time_two_way;
                //std::cout << "Cut improvement\t" << cut_improvement << std::endl;

                delete scheduler;
        }

        return overall_improvement;
}

EdgeWeight quotient_graph_refinement::perform_a_two_way_refinement(PartitionConfig & config,
                                                                   graph_access & G,
                                                                   complete_boundary & boundary,
                                                                   boundary_pair & bp,
                                                                   PartitionID & lhs,
                                                                   PartitionID & rhs,
                                                                   NodeWeight & lhs_part_weight,
                                                                   NodeWeight & rhs_part_weight,
                                                                   EdgeWeight & initial_cut_value,
                                                                   bool & something_changed) {

        two_way_fm pair_wise_refinement;
        two_way_flow_refinement pair_wise_flow;

        std::vector<NodeID> lhs_bnd_nodes;
        setup_start_nodes(G, lhs, bp, boundary, lhs_bnd_nodes);

        std::vector<NodeID> rhs_bnd_nodes;
        setup_start_nodes(G, rhs, bp, boundary, rhs_bnd_nodes);

        something_changed      = false;
        EdgeWeight improvement = 0;

        quality_metrics qm;
        if(config.refinement_type == REFINEMENT_TYPE_FM_FLOW || config.refinement_type == REFINEMENT_TYPE_FM) {
                improvement = pair_wise_refinement.perform_refinement(config,
                                                                      G,
                                                                      boundary,
                                                                      lhs_bnd_nodes,
                                                                      rhs_bnd_nodes,
                                                                      &bp,
                                                                      lhs_part_weight,
                                                                      rhs_part_weight,
                                                                      initial_cut_value,
                                                                      something_changed);
                ASSERT_TRUE(improvement >= 0 || config.rebalance);
        }

        if(config.refinement_type == REFINEMENT_TYPE_FM_FLOW || config.refinement_type == REFINEMENT_TYPE_FLOW){
                lhs_bnd_nodes.clear();
                setup_start_nodes(G, lhs, bp, boundary, lhs_bnd_nodes);

                rhs_bnd_nodes.clear();
                setup_start_nodes(G, rhs, bp, boundary, rhs_bnd_nodes);

                EdgeWeight _improvement = pair_wise_flow.perform_refinement(config,
                                                                            G,
                                                                            boundary,
                                                                            lhs_bnd_nodes,
                                                                            rhs_bnd_nodes,
                                                                            &bp,
                                                                            lhs_part_weight,
                                                                            rhs_part_weight,
                                                                            initial_cut_value,
                                                                            something_changed);

                ASSERT_TRUE(_improvement >= 0 || config.rebalance);
                improvement += _improvement;
        }

        bool only_one_block_is_overloaded = boundary.getBlockWeight(lhs) > config.upper_bound_partition;
        only_one_block_is_overloaded = only_one_block_is_overloaded
                                     || boundary.getBlockWeight(rhs) > config.upper_bound_partition;
        only_one_block_is_overloaded = only_one_block_is_overloaded &&
                (boundary.getBlockWeight(lhs) <= config.upper_bound_partition ||
                 boundary.getBlockWeight(rhs) <= config.upper_bound_partition);

        if(only_one_block_is_overloaded) {

                PartitionConfig cfg = config;
                cfg.softrebalance   = true;
                cfg.rebalance       = false;

                lhs_bnd_nodes.clear();
                setup_start_nodes(G, lhs, bp, boundary, lhs_bnd_nodes);

                rhs_bnd_nodes.clear();
                setup_start_nodes(G, rhs, bp, boundary, rhs_bnd_nodes);

                improvement += pair_wise_refinement.perform_refinement(cfg,
                                                                       G,
                                                                       boundary,
                                                                       lhs_bnd_nodes,
                                                                       rhs_bnd_nodes,
                                                                       &bp,
                                                                       lhs_part_weight,
                                                                       rhs_part_weight,
                                                                       initial_cut_value,
                                                                       something_changed);

                ASSERT_TRUE(improvement >= 0 || config.rebalance);

                if(!config.disable_hard_rebalance && !config.kaffpa_perfectly_balanced_refinement && !config.initial_bipartitioning) {
                                only_one_block_is_overloaded = boundary.getBlockWeight(lhs) > config.upper_bound_partition;
                                only_one_block_is_overloaded = only_one_block_is_overloaded
                                        || boundary.getBlockWeight(rhs) > config.upper_bound_partition;
                                only_one_block_is_overloaded = only_one_block_is_overloaded &&
                                        (boundary.getBlockWeight(lhs) <= config.upper_bound_partition ||
                                         boundary.getBlockWeight(rhs) <= config.upper_bound_partition);

                        if(only_one_block_is_overloaded) {
                                cfg.softrebalance = true;
                                cfg.rebalance     = true;

                                lhs_bnd_nodes.clear();
                                setup_start_nodes(G, lhs, bp, boundary, lhs_bnd_nodes);

                                rhs_bnd_nodes.clear();
                                setup_start_nodes(G, rhs, bp, boundary, rhs_bnd_nodes);

                                improvement += pair_wise_refinement.perform_refinement(cfg,
                                                G,
                                                boundary,
                                                lhs_bnd_nodes,
                                                rhs_bnd_nodes,
                                                &bp,
                                                lhs_part_weight,
                                                rhs_part_weight,
                                                initial_cut_value,
                                                something_changed);

                        }
                }
        }

        return improvement;
}

