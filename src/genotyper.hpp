#ifndef VG_GENOTYPER_H
#define VG_GENOTYPER_H
// genotyper.hpp: defines the Genotyper, which is used to genotype from a graph
// and a collection of indexed alignments.

#include <iostream>
#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <list>
#include "vg.pb.h"
#include "vg.hpp"
#include "hash_map.hpp"
#include "utility.hpp"
#include "types.hpp"

namespace vg {

using namespace std;

/**
 * Class to hold on to genotyping parameters and genotyping functions.
 */
class Genotyper {
public:

    // How many nodes max should we walk when checking if a path runs through a superbubble/site
    size_t max_path_search_steps = 100;
    
    // How long should we unfold graphs to?
    int unfold_max_length = 200;
    
    // How many steps of dagification should we do?
    int dagify_steps = 1;
    
    // What portion of the total affinity can be on one allele before we refuse
    // to call het and call homozygous instead.
    double max_het_bias = 4;
    
    /**
     * Unfold and dagify a graph, find the superbubbles, and then convert them
     * back to the space of the original graph.
     *
     * Returns a map from a pair of start, end node traversals for a superbubble
     * to the list of node IDs involved.
     */
    map<pair<NodeTraversal, NodeTraversal>, vector<id_t>> find_sites(VG& graph);
    
    /**
     * For the superbubble/site between start and end in the given orientations,
     * emit all unique subpaths that run from start to end, out of the paths in
     * the graph.
     */
    vector<Path> get_paths_through_site(VG& graph, NodeTraversal start, NodeTraversal end);
    
    /**
     * Get the affinity of all the reads relevant to the superbubble to all the
     * paths through the superbubble. We need to know all the nodes involved in
     * the superbubble so that we can clip them and their edges out and replace
     * them with the paths in turn.
     *
     * Affinity is a double out of 1.0. Higher is better.
     */ 
    map<Alignment*, vector<double>> get_affinities(VG& graph, const map<string, Alignment*>& reads_by_name,
        vector<id_t>& superbubble_contents, vector<Path>& superbubble_paths);
        
    /**
     * Compute annotated genotype from affinities and superbubble paths.
     */
    Genotype get_genotype(const vector<Path>& superbubble_paths, const map<Alignment*, vector<double>>& affinities);
        
    
    

};

}


#endif