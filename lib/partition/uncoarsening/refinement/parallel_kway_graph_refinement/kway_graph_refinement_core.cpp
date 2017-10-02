/******************************************************************************
 * kway_graph_refinement_core.cpp
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

#include <algorithm>
#include <tbb/concurrent_queue.h>

#include "data_structure/parallel/time.h"
#include "data_structure/priority_queues/bucket_pq.h"
#include "data_structure/priority_queues/maxNodeHeap.h"
#include "tools/random_functions.h"
#include "uncoarsening/refinement/kway_graph_refinement/kway_stop_rule.h"
#include "uncoarsening/refinement/parallel_kway_graph_refinement/kway_graph_refinement_core.h"

namespace parallel {
constexpr unsigned int kway_graph_refinement_core::sentinel;
constexpr int kway_graph_refinement_core::signed_sentinel;

kway_graph_refinement_core::kway_graph_refinement_core() {
}

kway_graph_refinement_core::~kway_graph_refinement_core() {

}

std::tuple<EdgeWeight, int, uint32_t>
kway_graph_refinement_core::single_kway_refinement_round(thread_data_refinement_core& td) {
        return single_kway_refinement_round_internal(td);
}

std::tuple<EdgeWeight, int, uint32_t>
kway_graph_refinement_core::single_kway_refinement_round_internal(thread_data_refinement_core& td) {
        auto& queue = td.queue;

        queue->clear();

        init_queue_with_boundary(td, queue);

        if (queue->empty()) {
                td.transpositions.push_back(sentinel);
                td.from_partitions.push_back(sentinel);
                td.to_partitions.push_back(sentinel);
                td.gains.push_back(signed_sentinel);

                return std::make_tuple(0, -1, 0);
        }

        int max_number_of_swaps = td.config.max_number_of_moves != -1 ? td.config.max_number_of_moves : td.G.number_of_nodes();

        EdgeWeight cut = std::numeric_limits<int>::max() / 2; // so we dont need to compute the edge cut
        EdgeWeight initial_cut = cut;

        //roll forwards
        EdgeWeight best_cut = cut;
        int number_of_swaps = 0;
        uint32_t movements = 0;

        std::unique_ptr<kway_stop_rule> stopping_rule;
        td.config.kway_adaptive_limits_alpha = 10;
        switch (td.config.kway_stop_rule) {
                case KWAY_SIMPLE_STOP_RULE:
                        stopping_rule = std::make_unique<kway_simple_stop_rule>(td.config);
                        break;
                case KWAY_ADAPTIVE_STOP_RULE:
                        stopping_rule = std::make_unique<kway_adaptive_stop_rule>(td.config);
                        break;
                case KWAY_CHERNOFF_ADAPTIVE_STOP_RULE:
                        stopping_rule = std::make_unique<kway_chernoff_adaptive_stop_rule>(td.config);
                        break;
        }

        int previously_moved = (int) td.transpositions.size();

        // minus 1 for sentinel
        int min_cut_index = previously_moved - 1;
        for (number_of_swaps = 0, movements = 0; movements < max_number_of_swaps; movements++, number_of_swaps++) {
                if (queue->empty()) {
                        ++td.stop_empty_queue;
                        break;
                }

                if (td.num_threads_finished.load(std::memory_order_relaxed) > 0) {
                        break;
                }

                uint32_t local_min_cut_index = (uint32_t) (min_cut_index - previously_moved >= 0 ? min_cut_index -
                                                                                                   previously_moved
                                                                                                : 0);
                if (stopping_rule->search_should_stop(local_min_cut_index, (uint32_t) number_of_swaps,
                                                      (uint32_t) td.step_limit)) {
                        ++td.stop_stopping_rule;
                        break;
                }

                Gain gain = queue->maxValue();
                NodeID node = queue->deleteMax();

                PartitionID from = td.get_local_partition(node);
#ifndef NDEBUG
                PartitionID maxgainer;
                EdgeWeight ext_degree;
                ASSERT_EQ(gain, td.compute_gain(node, from, maxgainer, ext_degree));
                ASSERT_TRUE(ext_degree > 0);
#endif

                PartitionID to;
                bool successfull = local_move_node(td, node, from, to, queue, gain);

                if (successfull) {
                        ++td.accepted_movements;
                        cut -= gain;
                        stopping_rule->push_statistics(gain);

#ifdef COMPARE_WITH_SEQUENTIAL_KAHIP
                        bool accept_equal = random_functions::nextBool();
#else
                        bool accept_equal = td.rnd.bit();
#endif
                        if (cut < best_cut || (cut == best_cut && accept_equal)) {
                                if (cut < best_cut) {
                                        stopping_rule->reset_statistics();
                                }
                                best_cut = cut;
                                min_cut_index = previously_moved + number_of_swaps;
                        }
                        td.from_partitions.push_back(from);
                        td.to_partitions.push_back(to);
                        td.transpositions.push_back(node);
                        td.gains.push_back(gain);

                        ALWAYS_ASSERT(min_cut_index < (int64_t) td.transpositions.size());
                } else {
                        number_of_swaps--; //because it wasnt swaps
                }
        }

        if (movements == max_number_of_swaps) {
                ++td.stop_max_number_of_swaps;
        }

        uint32_t unrolled_moves = unroll_moves(td, min_cut_index);
        td.accepted_movements -= unrolled_moves;
        td.nodes_partitions->clear();

        td.transpositions.push_back(sentinel);
        td.from_partitions.push_back(sentinel);
        td.to_partitions.push_back(sentinel);
        td.gains.push_back(signed_sentinel);

        return std::make_tuple(initial_cut - best_cut, min_cut_index, movements);
}

std::pair<EdgeWeight, uint32_t> kway_graph_refinement_core::local_search_from_one_node(thread_data_refinement_core& td,
                                                                                       moved_nodes_hash_map& moved_nodes,
                                                                                       NodeID start_node,
                                                                                       uint32_t max_number_of_swaps,
                                                                                       bool compute_touched_partitions,
                                                                                       std::unordered_map<PartitionID, PartitionID>& touched_blocks) const {

        // increasing number of swaps for better quality
        max_number_of_swaps = 2 * max_number_of_swaps + 100;

        kway_graph_refinement_commons* commons = kway_graph_refinement_commons::getInstance(td.config);
        std::unique_ptr<refinement_pq> queue;
        if (td.config.use_bucket_queues) {
                EdgeWeight max_degree = td.G.getMaxDegree();
                queue = std::make_unique<bucket_pq>(max_degree);
        } else {
                queue = std::make_unique<maxNodeHeap>();
        }

        PartitionID max_gainer;
        EdgeWeight ext_degree;
        Gain gain = commons->compute_gain(td.G, start_node, max_gainer, ext_degree);

        // node is not border node
        if (ext_degree == 0) {
                return {0, 0};
        }

        queue->insert(start_node, gain);

        EdgeWeight cut = std::numeric_limits<int>::max() / 2; // so we dont need to compute the edge cut
        EdgeWeight initial_cut = cut;

        //roll forwards
        EdgeWeight best_cut = cut;
        int number_of_swaps = 0;
        uint32_t movements = 0;
        int min_cut_index = -1;

        std::vector<NodeID> transpositions;
        std::vector<PartitionID> from_partitions;
        std::vector<PartitionID> to_partitions;

        transpositions.reserve(max_number_of_swaps);
        from_partitions.reserve(max_number_of_swaps);
        to_partitions.reserve(max_number_of_swaps);

        std::unique_ptr<kway_stop_rule> stopping_rule;
        switch (td.config.kway_stop_rule) {
                case KWAY_SIMPLE_STOP_RULE:
                        stopping_rule = std::make_unique<kway_simple_stop_rule>(td.config);
                        break;
                case KWAY_ADAPTIVE_STOP_RULE:
                        stopping_rule = std::make_unique<kway_adaptive_stop_rule>(td.config);
                        break;
                case KWAY_CHERNOFF_ADAPTIVE_STOP_RULE:
                        stopping_rule = std::make_unique<kway_chernoff_adaptive_stop_rule>(td.config);
                        break;
        }

        moved_hash_set moved_by_local_search(std::min<uint32_t>(get_max_size_to_fit_l1<moved_hash_set>(),
                                                                round_up_to_next_power_2(max_number_of_swaps)));

        for (number_of_swaps = 0, movements = 0; movements < max_number_of_swaps; movements++, number_of_swaps++) {
                if (queue->empty()) {
                        break;
                }

                if (stopping_rule->search_should_stop(min_cut_index >= 0 ? min_cut_index : 0, number_of_swaps,
                                                      td.step_limit)) {
                        break;
                }

                Gain gain = queue->maxValue();
                NodeID node = queue->deleteMax();

                PartitionID from = td.G.getPartitionIndex(node);
                bool successfull = move_node(td, moved_by_local_search, node, queue, commons);

                if (successfull) {
                        cut -= gain;
                        stopping_rule->push_statistics(gain);

                        bool accept_equal = td.rnd.bit();
                        if (cut < best_cut || (cut == best_cut && accept_equal)) {
                                if (cut < best_cut)
                                        stopping_rule->reset_statistics();
                                best_cut = cut;
                                min_cut_index = number_of_swaps;
                        }

                        from_partitions.push_back(from);
                        to_partitions.push_back(td.G.getPartitionIndex(node));
                        transpositions.push_back(node);
                } else {
                        number_of_swaps--; //because it wasn't swaps
                }
        }

        //roll backwards
        for (number_of_swaps--; number_of_swaps > min_cut_index; number_of_swaps--) {
                NodeID node = transpositions.back();
                transpositions.pop_back();

                PartitionID to = to_partitions.back();
                PartitionID from = from_partitions.back();
                from_partitions.pop_back();
                to_partitions.pop_back();

                relaxed_move_node_back(td, node, from, to);
        }

        ALWAYS_ASSERT(transpositions.size() == from_partitions.size());
        for (size_t i = 0; transpositions.size(); ++i) {
                // node will be considered as moved by all threads
                NodeID node = transpositions[i];
                moved_nodes[node] = std::make_pair(std::numeric_limits<uint32_t>::max(), from_partitions[i]);
        }

        //reconstruct the touched partitions
        if (compute_touched_partitions) {
                ASSERT_EQ(from_partitions.size(), to_partitions.size());
                for (size_t i = 0; i < from_partitions.size(); i++) {
                        touched_blocks[from_partitions[i]] = from_partitions[i];
                        touched_blocks[to_partitions[i]] = to_partitions[i];
                }
        }

        return std::make_pair(initial_cut - best_cut, movements);
}

std::pair<EdgeWeight, uint32_t> kway_graph_refinement_core::gain_recalculation(thread_data_refinement_core& td,
                                                                               moved_nodes_hash_map& moved_nodes,
                                                                               int start, int end,
                                                                               bool compute_touched_partitions,
                                                                               std::unordered_map<PartitionID, PartitionID>& touched_blocks) const {
        kway_graph_refinement_commons* commons = kway_graph_refinement_commons::getInstance(td.config);
        int best_gain_index = -1;
        EdgeWeight total_gain = 0;
        EdgeWeight best_total_gain = 0;

        std::vector<NodeID> transpositions;
        std::vector<PartitionID> from_partitions;
        std::vector<PartitionID> to_partitions;

        transpositions.reserve(end - start);
        from_partitions.reserve(end - start);
        to_partitions.reserve(end - start);

        int num_moves = 0;
        for (int index = start; index < end; ++index) {
                NodeID node = td.transpositions[index];

                PartitionID from = td.G.getPartitionIndex(node);
                PartitionID to;
                EdgeWeight ext_degree;
                Gain gain = commons->compute_gain(td.G, node, to, ext_degree);

                if (to == INVALID_PARTITION) {
                        continue;
                }

                bool success = relaxed_move_node(td, node, from, to);
                if (success) {
                        total_gain += gain;
                        bool accept_equal = td.rnd.bit();
                        if (total_gain > best_total_gain || (total_gain == best_total_gain && accept_equal)) {
                                best_total_gain = total_gain;
                                best_gain_index = num_moves;
                        }

                        transpositions.push_back(node);
                        from_partitions.push_back(from);
                        to_partitions.push_back(to);
                        ++num_moves;
                }
        }

        for (--num_moves; num_moves > best_gain_index; --num_moves) {
                NodeID node = transpositions.back();
                transpositions.pop_back();

                PartitionID to = to_partitions.back();
                PartitionID from = from_partitions.back();
                from_partitions.pop_back();
                to_partitions.pop_back();

                relaxed_move_node_back(td, node, from, to);
        }

        ALWAYS_ASSERT(transpositions.size() == from_partitions.size());
        for (size_t i = 0; i < transpositions.size(); ++i) {
                // node will be considered as moved by all threads
                NodeID node = transpositions[i];
                moved_nodes[node] = std::make_pair(std::numeric_limits<uint32_t>::max(), from_partitions[i]);
        }

        if (compute_touched_partitions) {
                ASSERT_EQ(from_partitions.size(), to_partitions.size());
                for (size_t i = 0; i < from_partitions.size(); i++) {
                        touched_blocks[from_partitions[i]] = from_partitions[i];
                        touched_blocks[to_partitions[i]] = to_partitions[i];
                }
        }
        return std::make_pair(best_total_gain, end - start);
}

uint32_t kway_graph_refinement_core::unroll_moves(thread_data_refinement_core& td, int min_cut_index) const {
        uint32_t unrolled_moves = 0;
        // unrolled_moves <  td.transpositions.size() - (min_cut_index + 1)
        while (unrolled_moves + min_cut_index + 1 < td.transpositions.size()) {
                size_t index = td.transpositions.size() - 1 - unrolled_moves;
                NodeID node = td.transpositions[index];
                PartitionID from = td.from_partitions[index];
                PartitionID to = td.to_partitions[index];
                local_move_back_node(td, node, from, to);

                ++unrolled_moves;
        }

        return unrolled_moves;
}

std::pair<EdgeWeight, uint32_t>
kway_graph_refinement_core::apply_moves(uint32_t num_threads,
                                        Cvector <thread_data_refinement_core>& threads_data,
                                        bool compute_touched_partitions,
                                        std::unordered_map<PartitionID, PartitionID>& touched_blocks,
                                        std::vector<NodeID>& reactivated_vertices) const {

        uint32_t overall_moved = 0;
        EdgeWeight overall_gain = 0;

        moved_nodes_hash_map moved_nodes(get_max_size_to_fit_l1<moved_nodes_hash_map>());
        for (size_t id = 0; id < num_threads; ++id) {
                overall_gain += apply_moves(threads_data[id].get(), compute_touched_partitions, touched_blocks);
        }
        overall_moved = moved_nodes.size();
        return std::make_pair(overall_gain, overall_moved);
}

std::pair<EdgeWeight, uint32_t>
kway_graph_refinement_core::apply_moves(Cvector <thread_data_refinement_core>& threads_data,
                                        bool compute_touched_partitions,
                                        std::unordered_map<PartitionID, PartitionID>& touched_blocks,
                                        std::vector<NodeID>& reactivated_vertices,
                                        tbb::concurrent_queue<uint32_t>& finished_threads,
                                        std::vector<std::future<bool>>& futures,
                                        bool& is_more_that_5percent_moved) const {

        uint32_t overall_moved = 0;
        EdgeWeight overall_gain = 0;

        moved_nodes_hash_map moved_nodes(get_max_size_to_fit_l1<moved_nodes_hash_map>());

        overall_gain += apply_moves(threads_data[0].get(), moved_nodes, compute_touched_partitions,
                                    touched_blocks, reactivated_vertices);

        if (threads_data.size() > 1) {
                uint32_t num_threads = threads_data.size() - 1;
                while (num_threads > 0) {
                        uint32_t id;
                        if (finished_threads.try_pop(id)) {
                                ALWAYS_ASSERT(id > 0);
                                if (futures[id - 1].get()) {
                                        is_more_that_5percent_moved = true;
                                }
                                overall_gain += apply_moves(threads_data[id].get(), moved_nodes,
                                                            compute_touched_partitions,
                                                            touched_blocks, reactivated_vertices);
                                --num_threads;
                        }
                }
        }
        overall_moved = moved_nodes.size();
        return std::make_pair(overall_gain, overall_moved);
}

bool kway_graph_refinement_core::is_moved(moved_nodes_hash_map& moved_nodes, NodeID node, uint32_t thread_id) const {
        return !moved_nodes.contains(node) ? false : (moved_nodes[node].first != thread_id);
}

EdgeWeight kway_graph_refinement_core::apply_moves(thread_data_refinement_core& td,
                                                   bool compute_touched_partitions,
                                                   std::unordered_map<PartitionID, PartitionID>& touched_blocks) const {
        CLOCK_START;
        ALWAYS_ASSERT(td.transpositions.size() == td.from_partitions.size());
        ALWAYS_ASSERT(td.transpositions.size() == td.to_partitions.size());
        ALWAYS_ASSERT(td.transpositions.size() == td.gains.size());
        td.transpositions_size += td.transpositions.size();

        auto min_cut_iter = td.min_cut_indices.begin();
        EdgeWeight cut_improvement = 0;
        Gain total_expected_gain = 0;
        std::vector<NodeID> transpositions;
        std::vector<PartitionID> from_partitions;
        std::vector<EdgeWeight> gains;
        uint32_t aff = 0;

        for (int index = 0; index < (int) td.transpositions.size(); ++index) {
                int min_cut_index = min_cut_iter->first;
                int next_index = min_cut_iter->second;
                ++min_cut_iter;

                if (min_cut_index == -1) {
                        index = next_index;
                        continue;
                }

                int best_total_gain = 0;
                int total_gain = 0;
                transpositions.reserve(min_cut_index - index + 1);
                from_partitions.reserve(min_cut_index - index + 1);
                gains.reserve(min_cut_index - index + 1);
                transpositions.clear();
                from_partitions.clear();
                gains.clear();

                while (index <= min_cut_index) {
                        NodeID node = td.transpositions[index];
                        PartitionID expected_from = td.from_partitions[index];
                        PartitionID expected_to = td.to_partitions[index];
                        EdgeWeight expected_gain = td.gains[index];

                        PartitionID from = td.G.getPartitionIndex(node);
                        PartitionID to;
                        EdgeWeight gain = td.compute_gain_actual(node, from, to, expected_to);

                        bool same_move = true;
                        if (expected_from != from || expected_to != to || expected_gain != gain) {
                                same_move = false;
                                ++td.affected_movements;
                                ++aff;
                        }

                        if (to == INVALID_PARTITION) {
                                ALWAYS_ASSERT(aff > 0);
                                ++index;
                                continue;
                        }

                        total_expected_gain += expected_gain;

                        bool success = relaxed_move_node(td, node, from, to);

                        if (success) {
                                transpositions.push_back(node);
                                from_partitions.push_back(from);
                                gains.push_back(gain);

                                if (compute_touched_partitions) {
                                        touched_blocks[from] = from;
                                        touched_blocks[to] = to;
                                }

                                cut_improvement += gain;
                                total_gain += gain;

                                if (total_gain > best_total_gain || (total_gain == best_total_gain && (td.rnd.bit() || same_move))) {
                                        best_total_gain = total_gain;
                                        from_partitions.clear();
                                        transpositions.clear();
                                        gains.clear();
                                }
                        }
                        ++index;
                }
                unroll_relaxed_moves(td, transpositions, from_partitions, gains, cut_improvement);
                index = next_index;
        }
        td.time_move_nodes += CLOCK_END_TIME;
        td.unperformed_gain += total_expected_gain - cut_improvement;
        td.performed_gain += cut_improvement;
        return cut_improvement;
}

EdgeWeight kway_graph_refinement_core::apply_moves(thread_data_refinement_core& td, moved_nodes_hash_map& moved_nodes,
                                                   bool compute_touched_partitions,
                                                   std::unordered_map<PartitionID, PartitionID>& touched_blocks,
                                                   std::vector<NodeID>& reactivated_vertices) const {
        //kway_graph_refinement_commons* commons = kway_graph_refinement_commons::getInstance(td.config);
        CLOCK_START;
        ALWAYS_ASSERT(td.transpositions.size() == td.from_partitions.size());
        ALWAYS_ASSERT(td.transpositions.size() == td.to_partitions.size());
        ALWAYS_ASSERT(td.transpositions.size() == td.gains.size());
        td.transpositions_size += td.transpositions.size();

        auto min_cut_iter = td.min_cut_indices.begin();
        //std::ofstream ftxt("moved_nodes_tmp.log", std::ofstream::out | std::ofstream::app);
        EdgeWeight cut_improvement = 0;
        Gain total_expected_gain = 0;

        // we save nodes which should have been moved but were not,
        // this can affect gains of other nodes
        parallel::hash_set<NodeID> not_moved(128);

        for (int index = 0; index < (int) td.transpositions.size(); ++index) {
                int min_cut_index = min_cut_iter->first;
                int next_index = min_cut_iter->second;
                ++min_cut_iter;

                if (min_cut_index == -1) {
                        index = next_index;
                        continue;
                }

                int start_index = index;
                int best_cut_index = start_index - 1;
                int best_total_gain = 0;
                int total_gain = 0;

                // Lambda which applies move strategy for conflict vertices (affected by other moves)
                auto apply_move_strategy_for_conflict = [&]() -> std::pair<EdgeWeight, uint32_t> {
#ifdef COMPARE_WITH_SEQUENTIAL_KAHIP
                        ALWAYS_ASSERT(false);
#endif
                        unroll_relaxed_moves(td, moved_nodes, best_cut_index + 1, index, cut_improvement);

                        // add all nodes that were not moved
                        for (int i = best_cut_index + 1; i <= min_cut_index; ++i) {
                                not_moved.insert(td.transpositions[i]);
                        }

                        EdgeWeight gain = 0;
                        uint32_t movements = 0;
                        if (td.config.apply_move_strategy == ApplyMoveStrategy::LOCAL_SEARCH) {
                                // start local search from node and max work amount (next_index - index) or
                                // start to move with gain recalculation
                                NodeID start_node = td.transpositions[best_cut_index + 1];
                                std::tie(gain, movements) = local_search_from_one_node(td, moved_nodes,
                                                                                       start_node,
                                                                                       next_index - best_cut_index - 1,
                                                                                       compute_touched_partitions,
                                                                                       touched_blocks);
                        } else if (td.config.apply_move_strategy == ApplyMoveStrategy::GAIN_RECALCULATION) {
                                std::tie(gain, movements) = gain_recalculation(td, moved_nodes, best_cut_index + 1,
                                                                               next_index,compute_touched_partitions,
                                                                               touched_blocks);
                        } else if (td.config.apply_move_strategy == ApplyMoveStrategy::REACTIVE_VERTICES) {
                                NodeID start_node = td.transpositions[best_cut_index + 1];
                                reactivated_vertices.push_back(start_node);

                                forall_out_edges(td.G, e, start_node){
                                        NodeID target = td.G.getEdgeTarget(e);
                                        reactivated_vertices.push_back(target);
                                } endfor
                        } else {
                               // skip strategy, do nothing
                        }
                        return std::make_pair(gain, movements);
                };

                while (index <= min_cut_index) {
                        NodeID node = td.transpositions[index];
                        PartitionID from = td.from_partitions[index];
                        PartitionID to = td.to_partitions[index];
                        EdgeWeight gain = td.gains[index];

                        // for ApplyMoveStrategy::LOCAL_SEARCH: local search for other node moved this node
                        if (is_moved(moved_nodes, node, td.id)) {

                                EdgeWeight gain;
                                uint32_t movements;
                                std::tie(gain, movements) = apply_move_strategy_for_conflict();

                                cut_improvement += gain;
                                break;
                        }

                        bool no_move = false;
                        // check if any neighbours were moved by other threads or were NOT moved by this thread
                        // if yes then stop moving
                        forall_out_edges(td.G, e, node) {
                                NodeID target = td.G.getEdgeTarget(e);
                                PartitionID target_partition = td.G.getPartitionIndex(target);
                                bool target_not_moved = not_moved.contains(target);
                                // moved by other thread or not moved by this thread
                                if (is_moved(moved_nodes, target, td.id) || target_not_moved) {
                                        PartitionID prev_target_partition = moved_nodes[target].second;
                                        if (target_not_moved || target_partition == to || target_partition == from ||
                                            prev_target_partition == to || prev_target_partition == from) {
                                                EdgeWeight gain;
                                                uint32_t movements;
                                                std::tie(gain, movements) = apply_move_strategy_for_conflict();

                                                cut_improvement += gain;
                                                no_move = true;
                                                break;
                                        }
                                }
                        } endfor

                        if (no_move) {
                                break;
                        }

                        // move node
//                        PartitionID maxgainer_;
//                        EdgeWeight ext_degree_;
//                        Gain gain_ = commons->compute_gain(td.G, node, maxgainer_, ext_degree_);
                        //ftxt << "thread " << td.id << ", move node " << node << " from " << from << " to " << to << std::endl;
                        bool success = relaxed_move_node(td, node, from, to);
//                        if (!success) {
//                                ftxt << "WAS NOT MOVED" << std::endl;
//                        }
                        if (success) {
//                                if (gain != gain_) {
//                                        std::cout << "thread " << td.id << ", move node " << node << " from " << from << " to " << to << std::endl;
//                                        std::cout << "Real partition = " << td.G.getPartitionIndex(node) << std::endl;
//                                        std::cout << "Calculated gain = " << gain << ", expected gain = " << gain_ << std::endl;
//
//                                        std::cout << "neighbours: ";
//                                        forall_out_edges(td.G, e, node) {
//                                                                NodeID target = td.G.getEdgeTarget(e);
//                                                                PartitionID target_partition = td.G.getPartitionIndex(target);
//                                                                std::cout << "node = " << target << ", part = " << target_partition << " ";
//                                                        } endfor
//
//                                        std::ofstream ftxt("gen_moved_nodes_tmp.log");
//                                        for (size_t i = 0; i <= index; ++i) {
//                                                NodeID node = td.transpositions[i];
//                                                PartitionID from = td.from_partitions[i];
//                                                PartitionID to = td.to_partitions[i];
//                                                EdgeWeight gain = td.gains[i];
//
//                                                ftxt << "thread " << td.id << ", move node " << node << " from " << from << " to " << to << std::endl;
//                                        }
//                                        ALWAYS_ASSERT(gain == gain_);
//                                }
                                //std::cout << node << ' ';
                                moved_nodes[node] = std::make_pair(td.id, from);
                                if (compute_touched_partitions) {
                                        touched_blocks[from] = from;
                                        touched_blocks[to] = to;
                                }

                                if (td.config.apply_move_strategy == ApplyMoveStrategy::REACTIVE_VERTICES
                                    && td.config.kway_all_boundary_nodes_refinement) {
                                        reactivated_vertices.push_back(node);

                                        forall_out_edges(td.G, e, node){
                                                NodeID target = td.G.getEdgeTarget(e);
                                                reactivated_vertices.push_back(target);
                                        } endfor
                                }

                                cut_improvement += gain;
                                total_gain += gain;

                                if (total_gain > best_total_gain) {
                                        best_total_gain = total_gain;
                                        best_cut_index = index;
                                } else if (total_gain == best_total_gain && td.rnd.bit()) {
                                                best_total_gain = total_gain;
                                                best_cut_index = index;
                                }
                        } else {
                                EdgeWeight gain;
                                uint32_t movements;
                                std::tie(gain, movements) = apply_move_strategy_for_conflict();

                                cut_improvement += gain;
                                break;
                        }
                        ++index;
                }
//                if (td.config.apply_move_strategy == ApplyMoveStrategy::REACTIVE_VERTICES
//                    && index == min_cut_index) {
//                        NodeID node = td.transpositions[index];
//                        PartitionID from = td.from_partitions[index];
//                        PartitionID to = td.to_partitions[index];
//
//                        reactivated_vertices.push_back(node);
//                        forall_out_edges(td.G, e, node){
//                                NodeID target = td.G.getEdgeTarget(e);
//                                reactivated_vertices.push_back(target);
//                        } endfor
//                }

                index = next_index;
        }
        td.time_move_nodes += CLOCK_END_TIME;

        td.unperformed_gain += total_expected_gain - cut_improvement;
        td.performed_gain += cut_improvement;

        return cut_improvement;
}

void kway_graph_refinement_core::init_queue_with_boundary(thread_data_refinement_core& td,
                                                          std::unique_ptr<refinement_pq>& queue) {
        if (td.config.permutation_during_refinement == PERMUTATION_QUALITY_FAST) {
                random_functions::permutate_vector_fast(td.start_nodes, false);
        } else if (td.config.permutation_during_refinement == PERMUTATION_QUALITY_GOOD) {
                random_functions::permutate_vector_good(td.start_nodes, false);
        }

        for (NodeID node : td.start_nodes) {
                bool expected = false;
                if (td.moved_idx[node].compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                        PartitionID max_gainer;
                        EdgeWeight ext_degree;
                        //compute gain
                        PartitionID from = td.get_local_partition(node);
                        Gain gain = td.compute_gain(node, from, max_gainer, ext_degree);
                        queue->insert(node, gain);
                        td.moved.push_back(node);
                }
        }
}

inline bool kway_graph_refinement_core::move_node(thread_data_refinement_core& td,
                                                  moved_hash_set& moved,
                                                  NodeID node,
                                                  std::unique_ptr<refinement_pq>& queue,
                                                  kway_graph_refinement_commons* commons) const {
        PartitionID from = td.G.getPartitionIndex(node);
        PartitionID to;
        EdgeWeight node_ext_deg;
        commons->compute_gain(td.G, node, to, node_ext_deg);
        ALWAYS_ASSERT(to != INVALID_PARTITION);

        bool success = relaxed_move_node(td, node, from, to);

        if (!success) {
                return false;
        }

        //update gain of neighbors / the boundaries have allready been updated
        forall_out_edges(td.G, e, node) {
                NodeID target = td.G.getEdgeTarget(e);
                PartitionID targets_max_gainer;
                EdgeWeight ext_degree; // the local external degree
                Gain gain = commons->compute_gain(td.G, target, targets_max_gainer, ext_degree);

                if (queue->contains(target)) {
                        assert(moved.contains(target));
                        if (ext_degree > 0) {
                                queue->changeKey(target, gain);
                        } else {
                                queue->deleteNode(target);
                        }
                } else {
                        if (ext_degree > 0) {
                                if (!moved.contains(target)) {
                                        queue->insert(target, gain);
                                        moved.insert(target);
                                }
                        }
                }
        }
        endfor

        return true;
}

inline bool kway_graph_refinement_core::relaxed_move_node(thread_data_refinement_core& td,
                                                          NodeID node,
                                                          PartitionID from,
                                                          PartitionID to) const {
        ASSERT_TRUE(td.boundary.assert_bnodes_in_boundaries());
        ASSERT_TRUE(td.boundary.assert_boundaries_are_bnodes());

        ALWAYS_ASSERT(td.G.getPartitionIndex(node) == from);

        NodeWeight this_nodes_weight = td.G.getNodeWeight(node);

        if (td.boundary.getBlockWeight(to) + this_nodes_weight >= td.config.upper_bound_partition) {
                return false;
        }

        if (td.boundary.getBlockNoNodes(from) == 1) {// assure that no block gets accidentally empty
                return false;
        }

        td.G.setPartitionIndex(node, to);

        boundary_pair pair;
        pair.k = td.config.k;
        pair.lhs = from;
        pair.rhs = to;

        td.boundary.postMovedBoundaryNodeUpdates(node, &pair, true, true);

        td.boundary.setBlockNoNodes(from, td.boundary.getBlockNoNodes(from) - 1);
        td.boundary.setBlockNoNodes(to, td.boundary.getBlockNoNodes(to) + 1);
        td.boundary.setBlockWeight(from, td.boundary.getBlockWeight(from) - this_nodes_weight);
        td.boundary.setBlockWeight(to, td.boundary.getBlockWeight(to) + this_nodes_weight);

        ASSERT_TRUE(td.boundary.assert_bnodes_in_boundaries());
        ASSERT_TRUE(td.boundary.assert_boundaries_are_bnodes());

        return true;
}

void kway_graph_refinement_core::unroll_relaxed_moves(thread_data_refinement_core& td,
                                                      moved_nodes_hash_map& moved_nodes, int start, int end,
                                                      int& cut_improvement) const {
        // iterate from end - 1 to start [start, end)
        //for (size_t i = 0; i + start < end; ++i) {
        for (int index = end - 1; index >= start; --index) {
                //size_t index = end - 1 - i;
                NodeID node = td.transpositions[index];
                PartitionID from = td.from_partitions[index];
                PartitionID to = td.to_partitions[index];
                cut_improvement -= td.gains[index];
                moved_nodes.erase(node);
                relaxed_move_node_back(td, node, from, to);
        }
}

void kway_graph_refinement_core::unroll_relaxed_moves(thread_data_refinement_core& td,
                                                      std::vector<NodeID>& transpositions,
                                                      std::vector<PartitionID>& from_partitions,
                                                      std::vector<Gain>& gains,
                                                      int& cut_improvement) const {
        size_t size = transpositions.size();
        for (size_t i = 0; i < transpositions.size(); ++i) {
                size_t index = size - i - 1;
                NodeID node = transpositions[index];
                PartitionID from = from_partitions[index];
                PartitionID to = td.G.getPartitionIndex(node);
                cut_improvement -= gains[index];
                relaxed_move_node_back(td, node, from, to);
        }
}

void kway_graph_refinement_core::relaxed_move_node_back(thread_data_refinement_core& td, NodeID node, PartitionID from,
                                                        PartitionID to) const {
        ALWAYS_ASSERT(td.G.getPartitionIndex(node) == to);
        td.G.setPartitionIndex(node, from);

        boundary_pair pair;
        pair.k = td.config.k;
        pair.lhs = from;
        pair.rhs = to;

        //update all boundaries
        td.boundary.postMovedBoundaryNodeUpdates(node, &pair, true, true);

        NodeWeight this_nodes_weight = td.G.getNodeWeight(node);
        td.boundary.setBlockNoNodes(from, td.boundary.getBlockNoNodes(from) + 1);
        td.boundary.setBlockNoNodes(to, td.boundary.getBlockNoNodes(to) - 1);
        td.boundary.setBlockWeight(from, td.boundary.getBlockWeight(from) + this_nodes_weight);
        td.boundary.setBlockWeight(to, td.boundary.getBlockWeight(to) - this_nodes_weight);
}

inline bool kway_graph_refinement_core::local_move_back_node(thread_data_refinement_core& td,
                                                             NodeID node,
                                                             PartitionID from,
                                                             PartitionID to) const {
        // td.set_local_partition(node, from);
        NodeWeight this_nodes_weight = td.G.getNodeWeight(node);

//        td.parts_weights[from].get().fetch_add(this_nodes_weight, std::memory_order_relaxed);
//        td.parts_weights[to].get().fetch_sub(this_nodes_weight, std::memory_order_relaxed);
//        td.parts_sizes[to].get().fetch_sub(1, std::memory_order_relaxed);
//        td.parts_sizes[from].get().fetch_add(1, std::memory_order_relaxed);

        td.parts_weights[from] += this_nodes_weight;
        td.parts_weights[to] -= this_nodes_weight;
        --td.parts_sizes[to];
        ++td.parts_sizes[from];


        return true;
};

inline bool kway_graph_refinement_core::local_move_node(thread_data_refinement_core& td,
                                                       NodeID node,
                                                       PartitionID from,
                                                       PartitionID& to,
                                                       std::unique_ptr<refinement_pq>& queue, Gain gain) {

        EdgeWeight node_ext_deg;

        Gain expected_gain = td.compute_gain(node, from, to, node_ext_deg);

        ALWAYS_ASSERT(expected_gain == gain);
        ALWAYS_ASSERT(to != INVALID_PARTITION);

        NodeWeight this_nodes_weight = td.G.getNodeWeight(node);

//        if (td.parts_sizes[from].get().load(std::memory_order_relaxed) == 1) {
//                return false;
//        }
//
//        NodeWeight part_weight = td.parts_weights[to].get().load(std::memory_order_relaxed);
//
//        do {
//                if (part_weight + this_nodes_weight >= td.config.upper_bound_partition) {
//                        return false;
//                }
//        } while (!td.parts_weights[to].get().compare_exchange_weak(part_weight,
//                                                                   part_weight + this_nodes_weight,
//                                                                   std::memory_order_relaxed));

        if (td.parts_sizes[from] == 1) {
                return false;
        }

        NodeWeight part_weight = td.parts_weights[to];

        if (part_weight + this_nodes_weight >= td.config.upper_bound_partition) {
                return false;
        }
        td.parts_weights[to] = part_weight + this_nodes_weight;


        td.set_local_partition(node, to);
//        td.parts_weights[from].get().fetch_sub(this_nodes_weight, std::memory_order_relaxed);
//        td.parts_sizes[to].get().fetch_add(1, std::memory_order_relaxed);
//        td.parts_sizes[from].get().fetch_sub(1, std::memory_order_relaxed);

        td.parts_weights[from] -= this_nodes_weight;
        ++td.parts_sizes[to];
        --td.parts_sizes[from];

        //update gain of neighbors / the boundaries have allready been updated
        forall_out_edges(td.G, e, node) {
                ++td.scaned_neighbours;
                NodeID target = td.G.getEdgeTarget(e);
                PartitionID targets_to;
                EdgeWeight ext_degree; // the local external degree

                if (queue->contains(target)) {
                        PartitionID target_from = td.get_local_partition(target);
                        Gain gain = td.compute_gain(target, target_from, targets_to, ext_degree);

                        assert(td.moved_idx[target].load(std::memory_order_relaxed));
                        if (ext_degree > 0) {
                                queue->changeKey(target, gain);
                        } else {
                                queue->deleteNode(target);
                        }
                } else {
                        // target was removed from priority queue
                        if (td.moved_idx[target].load(std::memory_order_relaxed)) {
                                continue;
                        }

                        PartitionID target_from = td.get_local_partition(target);
                        Gain gain = td.compute_gain(target, target_from, targets_to, ext_degree);

                        if (ext_degree > 0) {
                                bool expected = false;
                                if (td.moved_idx[target].compare_exchange_strong(expected, true,
                                                                                 std::memory_order_relaxed)) {
                                        queue->insert(target, gain);
                                        td.moved.push_back(target);
                                }
                        }
                }
        } endfor

        return true;
}
}
