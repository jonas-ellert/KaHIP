/******************************************************************************
 * kaffpa.cpp 
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

#include <argtable2.h>
#include <iostream>
#include <math.h>
#include <regex.h>
#include <sstream>
#include <stdio.h>
#include <string.h> 

#include "balance_configuration.h"
#include "data_structure/graph_access.h"
#include "data_structure/parallel/thread_pool.h"
#include "graph_io.h"
#include "macros_assertions.h"
#include "parse_parameters.h"
#include "partition/graph_partitioner.h"
#include "partition/partition_config.h"
#include "partition/uncoarsening/refinement/cycle_improvements/cycle_refinement.h"
#include "quality_metrics.h"
#include "random_functions.h"
#include "timer.h"
#include "uncoarsening/refinement/kway_graph_refinement/kway_stop_rule.h"
#include "uncoarsening/refinement/kway_graph_refinement/multitry_kway_fm.h"
#include "uncoarsening/refinement/parallel_kway_graph_refinement/multitry_kway_fm.h"
#include "uncoarsening/refinement/quotient_graph_refinement/quotient_graph_refinement.h"

int main(int argn, char **argv) {

        PartitionConfig partition_config;
        std::string graph_filename;

        bool is_graph_weighted = false;
        bool suppress_output   = false;
        bool recursive         = false;
       
        int ret_code = parse_parameters(argn, argv, 
                                        partition_config, 
                                        graph_filename, 
                                        is_graph_weighted, 
                                        suppress_output, recursive); 

        if(ret_code) {
                return 0;
        }

        std::streambuf* backup = std::cout.rdbuf();
        std::ofstream ofs;
        ofs.open("/dev/null");
        if(suppress_output) {
                std::cout.rdbuf(ofs.rdbuf()); 
        }

        partition_config.LogDump(stdout);
        graph_access G;     

        timer t;
        graph_io::readGraphWeighted(G, graph_filename);
        std::cout << "io time: " << t.elapsed()  << std::endl;
       
        G.set_partition_count(partition_config.k);
 
        balance_configuration bc;
        bc.configurate_balance( partition_config, G);

        std::vector<PartitionID> input_partition;
        if(partition_config.input_partition != "") {
                std::cout <<  "reading input partition" << std::endl;
                graph_io::readPartition(G, partition_config.input_partition);
                partition_config.graph_allready_partitioned = true;
                config.only_first_level = true;
                config.mh_no_mh = false;
                config.no_change_convergence = false;
                config.corner_refinement_enabled = false;
                config.kaffpa_perfectly_balanced_refinement = false;

                input_partition.resize(G.number_of_nodes());

                forall_nodes(G, node) {
                        input_partition[node] = G.getPartitionIndex(node);
                } endfor
        }

        srand(partition_config.seed);
        random_functions::setSeed(partition_config.seed);

        parallel::PinToCore(partition_config.main_core);
        parallel::g_thread_pool.Resize(partition_config.num_threads - 1);

        std::cout <<  "graph has " <<  G.number_of_nodes() <<  " nodes and " <<  G.number_of_edges() <<  " edges"  << std::endl;
        if (partition_config.label_propagation_refinement) {
                if (partition_config.parallel_lp) {
                        std::cout << "Algorithm\tparallel lp" << std::endl;
                } else {
                        std::cout << "Algorithm\tsequential lp" << std::endl;
                }
                std::cout << "Block size\t" << partition_config.block_size << std::endl;
        } else {
                if (partition_config.parallel_multitry_kway) {
                        std::cout << "Algorithm\tparallel multitry kway" << std::endl;
                } else {
                        std::cout << "Algorithm\tsequential multitry kway" << std::endl;
                }
        }
        std::cout << "Num threads\t" << partition_config.num_threads << std::endl;

        switch (partition_config.apply_move_strategy) {
                case ApplyMoveStrategy::LOCAL_SEARCH:
                        std::cout << "Move strategy\tlocal search" << std::endl;
                        break;
                case ApplyMoveStrategy::GAIN_RECALCULATION:
                        std::cout << "Move strategy\tgain recalculation" << std::endl;
                        break;
                case ApplyMoveStrategy::REACTIVE_VERTICES:
                        std::cout << "Move strategy\treactivate_vertices" << std::endl;
                        break;
                case ApplyMoveStrategy::SKIP:
                        std::cout << "Move strategy\tskip" << std::endl;
                        break;
        }

        switch (partition_config.kway_stop_rule) {
                case KWayStopRule::KWAY_SIMPLE_STOP_RULE:
                        std::cout << "Kway stop rule\tsimple" << std::endl;
                        break;
                case KWayStopRule::KWAY_ADAPTIVE_STOP_RULE:
                        std::cout << "Kway stop rule\tadaptive" << std::endl;
                        break;
                case KWayStopRule::KWAY_CHERNOFF_ADAPTIVE_STOP_RULE:
                        std::cout << "Kway stop rule\tchernoff_adaptive" << std::endl;
                        std::cout << "Stop probability\t" << partition_config.chernoff_stop_probability << std::endl;
                        std::cout << "Num gradient descent step\t" << partition_config.chernoff_gradient_descent_num_steps << std::endl;
                        std::cout << "Gradient descent step size\t" << partition_config.chernoff_gradient_descent_step_size << std::endl;
                        std::cout << "Min num step limit\t" << partition_config.chernoff_min_step_limit << std::endl;
                        std::cout << "Max num step limit\t" << partition_config.chernoff_max_step_limit << std::endl;
                        break;
        }

        // ***************************** perform partitioning ***************************************       
        t.restart();
        graph_partitioner partitioner;
        quality_metrics qm;

        std::cout <<  "performing partitioning!"  << std::endl;
        if(partition_config.time_limit == 0) {
                partitioner.perform_partitioning(partition_config, G);
        } else {
                PartitionID* map = new PartitionID[G.number_of_nodes()];
                EdgeWeight best_cut = std::numeric_limits<EdgeWeight>::max();
                while(t.elapsed() < partition_config.time_limit) {
                        partition_config.graph_allready_partitioned = false;
                        partitioner.perform_partitioning(partition_config, G);
                        EdgeWeight cut = qm.edge_cut(G);
                        if(cut < best_cut) {
                                best_cut = cut;
                                forall_nodes(G, node) {
                                        map[node] = G.getPartitionIndex(node);
                                } endfor
                        }
                }

                forall_nodes(G, node) {
                        G.setPartitionIndex(node, map[node]);
                } endfor
        }

        if( partition_config.kaffpa_perfectly_balance ) {
                double epsilon                         = partition_config.imbalance/100.0;
                partition_config.upper_bound_partition = (1+epsilon)*ceil(partition_config.largest_graph_weight/(double)partition_config.k);

                complete_boundary boundary(&G);
                boundary.build();

                cycle_refinement cr;
                cr.perform_refinement(partition_config, G, boundary);
        }
        // ******************************* done partitioning *****************************************       
        ofs.close();
        std::cout.rdbuf(backup);
        std::cout <<  "time spent for partitioning " << t.elapsed()  << std::endl;
       
        // output some information about the partition that we have computed 
        std::cout << "cut \t\t"         << qm.edge_cut(G)                 << std::endl;
        std::cout << "finalobjective  " << qm.edge_cut(G)                 << std::endl;
        std::cout << "bnd \t\t"         << qm.boundary_nodes(G)           << std::endl;
        std::cout << "balance \t"       << qm.balance(G)                  << std::endl;
        std::cout << "max_comm_vol \t"  << qm.max_communication_volume(G) << std::endl;

        if (!partition_config.label_propagation_refinement) {
                std::cout << "Two way refinement:" << std::endl;
                quotient_graph_refinement::print_full_statistics();
                std::cout << std::endl;

                std::cout << "Local search statistics:" << std::endl;
                if (partition_config.parallel_multitry_kway) {
                        parallel::multitry_kway_fm::print_full_statistics();
                } else {
                        multitry_kway_fm::print_full_statistics();
                }
                std::cout << std::endl;
        }


        // write the partition to the disc 
        std::stringstream filename;
        if(!partition_config.filename_output.compare("")) {
                filename << "tmppartition" << partition_config.k;
        } else {
                filename << partition_config.filename_output;
        }

        graph_io::writePartition(G, filename.str());
        
}
