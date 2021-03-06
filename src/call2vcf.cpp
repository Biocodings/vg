// copied from https://github.com/adamnovak/glenn2vcf/blob/master/main.cpp
// as this logic really belongs in vg call

#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <unordered_set>
#include <utility>
#include <algorithm>
#include <getopt.h>

#include "vg.hpp"
#include "index.hpp"
#include "Variant.h"
#include "genotypekit.hpp"
#include "snarls.hpp"
#include "path_index.hpp"
#include "caller.hpp"
#include "stream.hpp"
#include "nested_traversal_finder.hpp"

//#define debug

namespace vg {

// How many bases may we put in an allele in VCF if we expect GATK to be able to
// parse it?
// 0 means no maximum is enforced.
const static int MAX_ALLELE_LENGTH = 0;

// Minimum log likelihood
const static double LOG_ZERO = (double)-1e100;

// convert to string using stringstream (to replace to_string when we want sci. notation)
template <typename T>
string to_string_ss(T val) {
    stringstream ss;
    ss << val;
    return ss.str();
}

/**
 * We need to suppress overlapping variants, but interval trees are hard to
 * write. This accomplishes the collision check with a massive bit vector.
 */
struct IntervalBitfield {
    // Mark every position that's used in a variant
    vector<bool> used;
    
    /**
     * Make a new IntervalBitfield covering a region of the specified length.
     */
    inline IntervalBitfield(size_t length) : used(length) {
        // Nothing to do
    }
    
    /**
     * Scan for a collision (O(n) in interval length)
     */
    inline bool collides(size_t start, size_t pastEnd) {
        for(size_t i = start; i < pastEnd; i++) {
            if(used[i]) {
                return true;
            }
        }
        return(false);
    }
    
    /**
     * Take up an interval.
     */
    inline void add(size_t start, size_t pastEnd) {
        for(size_t i = start; i < pastEnd; i++) {
            used[i] = true;
        }
    }
};

/**
 * Get the strand bias of a Support.
 */
double strand_bias(const Support& support) {
    return max(support.forward(), support.reverse()) / (support.forward() + support.reverse());
}

/**
 * Make a letter into a full string because apparently that's too fancy for the
 * standard library.
 */
string char_to_string(const char& letter) {
    string toReturn;
    toReturn.push_back(letter);
    return toReturn;
}

/**
 * Write a minimal VCF header for a file with the given samples, and the given
 * contigs with the given lengths.
 */
void write_vcf_header(ostream& stream, const vector<string>& sample_names,
    const vector<string>& contig_names, const vector<size_t>& contig_sizes,
    int min_mad_for_filter) {
    
    stream << "##fileformat=VCFv4.2" << endl;
    stream << "##ALT=<ID=NON_REF,Description=\"Represents any possible alternative allele at this location\">" << endl;
    stream << "##INFO=<ID=XREF,Number=0,Type=Flag,Description=\"Present in original graph\">" << endl;
    stream << "##INFO=<ID=XSEE,Number=.,Type=String,Description=\"Original graph node:offset cross-references\">" << endl;
    stream << "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Total Depth\">" << endl;
    stream << "##FILTER=<ID=FAIL,Description=\"Variant does not meet minimum allele read support threshold of " << min_mad_for_filter << "\">" <<endl;
    stream << "##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"Read Depth\">" << endl;
    stream << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">" << endl;
    stream << "##FORMAT=<ID=AD,Number=.,Type=Integer,Description=\"Allelic depths for the ref and alt alleles in the order listed\">" << endl;
    stream << "##FORMAT=<ID=SB,Number=4,Type=Integer,Description=\"Forward and reverse support for ref and alt alleles.\">" << endl;
    // We need this field to stratify on for VCF comparison. The info is in SB but vcfeval can't pull it out
    stream << "##FORMAT=<ID=XAAD,Number=1,Type=Integer,Description=\"Alt allele read count.\">" << endl;
    stream << "##FORMAT=<ID=AL,Number=.,Type=Float,Description=\"Allelic likelihoods for the ref and alt alleles in the order listed\">" << endl;
    
    for(size_t i = 0; i < contig_names.size(); i++) {
        // Announce the contigs as well.
        stream << "##contig=<ID=" << contig_names.at(i) << ",length=" << contig_sizes.at(i) << ">" << endl;
    }
    
    // Now the column header line
    stream << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT";
    for (auto& sample_name : sample_names) {
        // Append columns for all the samples
        stream << "\t" << sample_name;
    }
    // End the column header line
    stream << endl;
}

/**
 * Return true if a variant may be output, or false if this variant is valid but
 * the GATK might choke on it.
 *
 * Mostly used to throw out variants with very long alleles, because GATK has an
 * allele length limit. How alleles that really *are* 1 megabase deletions are
 * to be specified to GATK is left as an exercise to the reader.
 */
bool can_write_alleles(vcflib::Variant& variant) {
    for(auto& allele : variant.alleles) {
        if(MAX_ALLELE_LENGTH > 0 && allele.size() > MAX_ALLELE_LENGTH) {
            return false;
        }
    }
    return true;
}

/**
 * Return true if a mapping is a perfect match, and false if it isn't.
 */
bool mapping_is_perfect_match(const Mapping& mapping) {
    for (auto edit : mapping.edit()) {
        if (edit.from_length() != edit.to_length() || !edit.sequence().empty()) {
            // This edit isn't a perfect match
            return false;
        }
    }
    
    // If we get here, all the edits are perfect matches.
    // Note that Mappings with no edits at all are full-length perfect matches.
    return true;
}

/**
 * Given a collection of pileups by original node ID, and a set of original node
 * id:offset cross-references in both ref and alt categories, produce a VCF
 * comment line giving the pileup for each of those positions on those nodes.
 * Includes a trailing newline if nonempty.
 *
 * TODO: VCF comments aren't really a thing.
 */
string get_pileup_line(const map<int64_t, NodePileup>& node_pileups,
    const set<pair<int64_t, size_t>>& refCrossreferences,
    const set<pair<int64_t, size_t>>& altCrossreferences) {
    // We'll make a stringstream to write to.
    stringstream out;
    
    out << "#";
    
    for(const auto& xref : refCrossreferences) {
        // For every cross-reference
        if(node_pileups.count(xref.first) && node_pileups.at(xref.first).base_pileup_size() > xref.second) {
            // If we have that base pileup, grab it
            auto basePileup = node_pileups.at(xref.first).base_pileup(xref.second);
            
            out << xref.first << ":" << xref.second << " (ref) " << basePileup.bases() << "\t";
        }
        // Nodes with no pileups (either no pileups were provided or they didn't
        // appear/weren't visited by reads) will not be mentioned on this line
    }
    
    for(const auto& xref : altCrossreferences) {
        // For every cross-reference
        if(node_pileups.count(xref.first) && node_pileups.at(xref.first).base_pileup_size() > xref.second) {
            // If we have that base pileup, grab it
            auto basePileup = node_pileups.at(xref.first).base_pileup(xref.second);
            
            out << xref.first << ":" << xref.second << " (alt) " << basePileup.bases() << "\t";
        }
        // Nodes with no pileups (either no pileups were provided or they didn't
        // appear/weren't visited by reads) will not be mentioned on this line
    }
    // TODO: make these nearly-identical loops a loop or a lambda or something.
    
    if(out.str().size() > 1) {
        // We actually found something. Send it out with a trailing newline
        out << endl;
        return out.str();
    } else {
        // Give an empty string.
        return "";
    }
}

Call2Vcf::PrimaryPath::PrimaryPath(AugmentedGraph& augmented, const string& ref_path_name, size_t ref_bin_size):
    ref_bin_size(ref_bin_size), index(augmented.graph, ref_path_name, true), name(ref_path_name)  {

    // Follow the reference path and extract indexes we need: index by node ID,
    // index by node start, and the reconstructed path sequence.
    PathIndex index(augmented.graph, ref_path_name, true);

    if (index.sequence.size() == 0) {
        // No empty reference paths allowed
        throw runtime_error("Reference path cannot be empty");
    }

    // Store support binned along reference path;
    // Last bin extended to include remainder
    ref_bin_size = min(ref_bin_size, index.sequence.size());
    if (ref_bin_size <= 0) {
        // No zero-sized bins allowed
        throw runtime_error("Reference bin size must be 1 or larger");
    }
    // Start out all the bins empty.
    binned_support = vector<Support>(max(1, int(index.sequence.size() / ref_bin_size)), Support());
    
    // Crunch the numbers on the reference and its read support. How much read
    // support in total (node length * aligned reads) does the primary path get?
    total_support = Support();
    for(auto& pointerAndSupport : augmented.node_supports) {
        if(index.by_id.count(pointerAndSupport.first->id())) {
            // This is a primary path node. Add in the total read bases supporting it
            total_support += pointerAndSupport.first->sequence().size() * pointerAndSupport.second;
            
            // We also update the total for the appropriate bin
            int bin = index.by_id[pointerAndSupport.first->id()].first / ref_bin_size;
            if (bin == binned_support.size()) {
                --bin;
            }
            binned_support[bin] = binned_support[bin] + 
                pointerAndSupport.first->sequence().size() * pointerAndSupport.second;
        }
    }
    
    // Average out the support bins too (in place)
    min_bin = 0;
    max_bin = 0;
    for (int i = 0; i < binned_support.size(); ++i) {
        // Compute the average over the bin's actual size
        binned_support[i] = binned_support[i] / (
            i < binned_support.size() - 1 ? (double)ref_bin_size :
            (double)(ref_bin_size + index.sequence.size() % ref_bin_size));
            
        // See if it's a min or max
        if (binned_support[i] < binned_support[min_bin]) {
            min_bin = i;
        }
        if (binned_support[i] > binned_support[max_bin]) {
            max_bin = i;
        }
    }

}

const Support& Call2Vcf::PrimaryPath::get_support_at(size_t primary_path_offset) const {
    return get_bin(get_bin_index(primary_path_offset));
}
        
size_t Call2Vcf::PrimaryPath::get_bin_index(size_t primary_path_offset) const {
    // Find which coordinate bin the position is in
    int bin = primary_path_offset / ref_bin_size;
    if (bin == get_total_bins()) {
        --bin;
    }
    return bin;
}
    
size_t Call2Vcf::PrimaryPath::get_min_bin() const {
    return min_bin;
}
    
size_t Call2Vcf::PrimaryPath::get_max_bin() const {
    return max_bin;
}
    
const Support& Call2Vcf::PrimaryPath::get_bin(size_t bin) const {
    return binned_support[bin];
}
        
size_t Call2Vcf::PrimaryPath::get_total_bins() const {
    return binned_support.size();
}
        
Support Call2Vcf::PrimaryPath::get_average_support() const {
    return get_total_support() / get_index().sequence.size();
}

Support Call2Vcf::PrimaryPath::get_average_support(const map<string, PrimaryPath>& paths) {
    // Track the total support overall
    Support total;
    // And the total number of bases
    size_t bases;
    
    for (auto& kv : paths) {
        // Sum over all paths
        total += kv.second.get_total_support();
        bases += kv.second.get_index().sequence.size();
    }
    
    // Then divide
    return total / bases;
}
        
Support Call2Vcf::PrimaryPath::get_total_support() const {
    return total_support;
}
  
PathIndex& Call2Vcf::PrimaryPath::get_index() {
    return index;
}
    
const PathIndex& Call2Vcf::PrimaryPath::get_index() const {
    return index;
}

const string& Call2Vcf::PrimaryPath::get_name() const {
    return name;
}

map<string, Call2Vcf::PrimaryPath>::iterator Call2Vcf::find_path(const Snarl& site, map<string, PrimaryPath>& primary_paths) {
    for(auto i = primary_paths.begin(); i != primary_paths.end(); ++i) {
        // Scan the whole map with an iterator
        
        if (i->second.get_index().by_id.count(site.start().node_id()) &&
            i->second.get_index().by_id.count(site.end().node_id())) {
            // This path threads through this site
            return i;
        }
    }
    // Otherwise we hit the end and found no path that this site can be strung
    // on.
    return primary_paths.end();
}

// this was main() in glenn2vcf
void Call2Vcf::call(
    // Augmented graph
    AugmentedGraph& augmented,
    // Should we load a pileup and print out pileup info as comments after
    // variants?
    string pileup_filename) {
    
    // Set up the graph's paths properly after augmentation modified them.
    augmented.graph.paths.sort_by_mapping_rank();
    augmented.graph.paths.rebuild_mapping_aux();
    
    // Make a list of the specified or autodetected primary reference paths.
    vector<string> primary_path_names = ref_path_names;
    if (primary_path_names.empty()) {
        // Try and guess reference path names for VCF conversion or coverage measurement.
        if (verbose) {
          std:cerr << "Graph has " << augmented.graph.paths.size() << " paths to choose from."
                   << endl;
        }
        if(augmented.graph.paths.size() == 1) {
            // Autodetect the reference path name as the name of the only path
            primary_path_names.push_back((*augmented.graph.paths._paths.begin()).first);
        } else if (augmented.graph.paths.has_path("ref")) {
            // Take any "ref" path.
            primary_path_names.push_back("ref");
        }

        if (verbose && !primary_path_names.empty()) {
            cerr << "Guessed reference path name of " << primary_path_names.front() << endl;
        }
        
    }
    
    // We'll fill this in with a PrimaryPath for every primary reference path
    // that is specified or detected.
    map<string, PrimaryPath> primary_paths;
    for (auto& name : primary_path_names) {
        // Make a PrimaryPath for every primary path we have.
        // Index the primary path and compute the binned supports.   
        primary_paths.emplace(std::piecewise_construct,
            std::forward_as_tuple(name),
            std::forward_as_tuple(augmented, name, ref_bin_size));
    
        auto& primary_path = primary_paths.at(name);
    
        if (verbose) {
            cerr << "Primary path " << name << " average/off-path assumed coverage: "
                << primary_path.get_average_support() << endl;
            cerr << "Mininimum binned average coverage: " << primary_path.get_bin(primary_path.get_min_bin()) << " (bin "
                << (primary_path.get_min_bin() + 1) << " / " << primary_path.get_total_bins() << ")" << endl;
            cerr << "Maxinimum binned average coverage: " << primary_path.get_bin(primary_path.get_max_bin()) << " (bin "
                << (primary_path.get_max_bin() + 1) << " / " << primary_path.get_total_bins() << ")" << endl;
        }
    }
    
    // If applicable, load the pileup.
    // This will hold pileup records by node ID.
    map<int64_t, NodePileup> node_pileups;
    
    function<void(Pileup&)> handle_pileup = [&](Pileup& p) { 
        // Handle each pileup chunk
        for(size_t i = 0; i < p.node_pileups_size(); i++) {
            // Pull out every node pileup
            auto& pileup = p.node_pileups(i);
            // Save the pileup under its node's pointer.
            node_pileups[pileup.node_id()] = pileup;
        }
    };
    if(!pileup_filename.empty()) {
        // We have to load some pileups
        ifstream in;
        in.open(pileup_filename.c_str());
        stream::for_each(in, handle_pileup);
    }
    
    // Make a VCF because we need it in scope later, if we are outputting VCF.
    vcflib::VariantCallFile vcf;
    
    // We also might need to fillin this contig names by path name map
    map<string, string> contig_names_by_path_name;
    
    if (convert_to_vcf) {
        // Do initial setup for VCF output
        
        // Decide on names and lengths for all the primary paths.
        vector<size_t> contig_lengths;
        vector<string> contig_names;
        
        for (size_t i = 0; i < primary_path_names.size(); i++) {
            if (i < contig_name_overrides.size()) {
                // Override this name
                contig_names.push_back(contig_name_overrides.at(i));
            } else {
                // Keep the path name from the graph
                contig_names.push_back(primary_path_names.at(i));
            }
            
            // Allow looking up the assigned contig name later
            contig_names_by_path_name[primary_path_names.at(i)] = contig_names.back();
            
            if (i < length_overrides.size()) {
                // Override this length
                contig_lengths.push_back(length_overrides.at(i));
            } else {
                // Grab the length from the index
                contig_lengths.push_back(primary_paths.at(primary_path_names.at(i)).get_index().sequence.size());
            }
            
            // TODO: is this fall-through-style logic smart, or will we just
            // neglect to warn people that they forgot options by parsing what
            // they said when they provide too few overrides?
        }
    
        // Generate a vcf header. We can't make Variant records without a
        // VariantCallFile, because the variants need to know which of their
        // available info fields or whatever are defined in the file's header,
        // so they know what to output.
        stringstream header_stream;
        write_vcf_header(header_stream, {sample_name}, contig_names, contig_lengths, min_mad_for_filter);
        
        // Load the headers into a the VCF file object
        string header_string = header_stream.str();
        assert(vcf.openForOutput(header_string));
        
        // Spit out the header
        cout << header_stream.str();
    }
    
    // Then go through it from the graph's point of view: first over alt nodes
    // backending into the reference (creating things occupying ranges to which
    // we can attribute copy number) and then over reference nodes.

    // Do the new thing where we support multiple alleles

    // Find all the top-level sites
    list<const Snarl*> site_queue;
    
    CactusUltrabubbleFinder finder(augmented.graph);
    SnarlManager site_manager = finder.find_snarls();
    
    site_manager.for_each_top_level_snarl_parallel([&](const Snarl* site) {
        // Stick all the sites in this vector.
        #pragma omp critical (sites)
        site_queue.emplace_back(site);
    });
    
    // We're going to run through all the top-level sites and break up the
    // huge ones and throw out the ones not on the reference, leaving only
    // manageably-sized sites on the ref path. We won't be able to genotype
    // huge translocations or deletions, but we can save those for the
    // proper nested-site-aware genotyper.
    vector<const Snarl*> sites;
    
    while(!site_queue.empty()) {
        // Grab the first site
        const Snarl* site = move(site_queue.front());
        site_queue.pop_front();
        
        // If the site is strung on any of the primary paths, find the
        // corresponding PrimaryPath object. Otherwise, leave this null.
        PrimaryPath* primary_path = nullptr;
        {
            auto found = find_path(*site, primary_paths);
            if (found != primary_paths.end()) {
                primary_path = &found->second;
            }
        }
        
        
        if (site->type() == ULTRABUBBLE && primary_path != nullptr) {
            // This site is an ultrabubble on a primary path
        
            // Where does the variable region start and end on the reference?
            size_t ref_start = primary_path->get_index().by_id.at(site->start().node_id()).first +
                augmented.graph.get_node(site->start().node_id())->sequence().size();
            size_t ref_end = primary_path->get_index().by_id.at(site->end().node_id()).first;
            
            if(ref_start > ref_end) {
                // Make sure it's the right way around (it will get set straight
                // in the site when we do the bubbling-up).
                swap(ref_start, ref_end);
            }
            
            if(!site_manager.is_leaf(site) && ref_end > ref_start + max_ref_length) {
                // Site is too big and has children. Split it up and do the
                // children.
                for(const Snarl* child : site_manager.children_of(site)) {
                    // Dump all the children into the queue for separate
                    // processing.
                    site_queue.emplace_back(child);
                }

                if (verbose) {
                    cerr << "Broke up site from " << ref_start << " to " << ref_end << " into "
                              << site_manager.children_of(site).size() << " children" << endl;
                }
                
            } else {
                // With no children, site may still be huge, but it doesn't
                // matter because there's nothing to nest in it, so we can
                // genotype it just fine.
                
                // With children, it's practical to just include the child
                // genotypes in the site genotype.
                
                if(ref_end > ref_start + max_ref_length) {
                    // This site is big but we left it anyway.
                    if (verbose) {
                        cerr << "Left site from " << ref_start << " to " << ref_end << " with "
                                  << site_manager.children_of(site).size() << " children" << endl;
                    }
                }
                
                // Make sure start and end are front-ways relative to the ref path.
                if(primary_path->get_index().by_id.at(site->start().node_id()).first >
                    primary_path->get_index().by_id.at(site->end().node_id()).first) {
                    // The site's end happens before its start, flip it around
                    site_manager.flip(site);
                }
                
                // Throw it in the final vector of sites we're going to process.
                sites.push_back(site);
            }        
        } else if (site->type() == ULTRABUBBLE && !convert_to_vcf) {
            // This site is an ultrabubble and we can handle things off the
            // primary path.
            
            // TODO: enforce a max size or something?
            sites.push_back(site);
            
        } else {
            // The site is not on the primary path or isn't an ultrabubble, but
            // maybe one of its children will meet our requirements.
            
            size_t child_count = site_manager.children_of(site).size();
            
            for(const Snarl* child : site_manager.children_of(site)) {
                // Dump all the children into the queue for separate
                // processing.
                site_queue.emplace_back(child);
            }

            if (verbose) {
                if (child_count) {
                    cerr << "Broke up off-reference site into "
                         << child_count << " children" << endl;
                } else {
                    cerr << "Dropped off-reference site" << endl;
                }
            }
            
        }     
    }

    if (verbose) {
        cerr << "Found " << sites.size() << " sites" << endl;
    }
    
    // Now start looking for traversals of the sites.
    RepresentativeTraversalFinder traversal_finder(augmented, site_manager, max_search_depth, max_bubble_paths,
        [&] (const Snarl& site) -> PathIndex* {
        
        // When the TraversalFinder needs an index for a site, it can look it up with this function.
        auto found = find_path(site, primary_paths);
        if (found != primary_paths.end()) {
            return &found->second.get_index();
        } else {
            return nullptr;
        }
    });
    
    // We're going to remember what nodes and edges are covered by sites, so we
    // will know which nodes/edges aren't in any sites and may need generic
    // presence/absence calls.
    set<Node*> covered_nodes;
    set<Edge*> covered_edges;
    
    // When we genotype the sites into Locus objects, we will use this buffer for outputting them.
    vector<Locus> locus_buffer;
    
    // How many sites result in output?
    size_t called_loci = 0;
    
    for(const Snarl* site : sites) {
        // For every site, we're going to make a variant
        
        // Get the traversals. The ref traversal is the first one. They are all
        // in site orientation, which may oppose primary path orientation.
        vector<SnarlTraversal> traversals = traversal_finder.find_traversals(*site);
        
        // Make a Locus to represent this site, with all the Paths of the
        // different alleles.
        Locus locus;
        
        // And keep around a vector of is_reference statuses for all the alleles
        vector<bool> is_ref;
        
        for (auto& traversal : traversals) {
            // Convert each traversal to a path
            Path* path = locus.add_allele();
        
            // Start at the start of the site
            *path->add_mapping() = to_mapping(site->start(), augmented.graph);
            for (size_t i = 0; i < traversal.visits_size(); i++) {
                // Then visit each node in turn
                *path->add_mapping() = to_mapping(traversal.visits(i), augmented.graph);
            }
            // Finish up with the site's end
            *path->add_mapping() = to_mapping(site->end(), augmented.graph);
            
            // Save the traversal's reference status
            is_ref.push_back(is_reference(traversal, augmented));
        }
        
        // Collect sequences for all the paths
        vector<string> sequences;
        // And the lists of involved IDs that we use for variant IDs
        vector<string> id_lists;
        // Calculate average and min support for all the alts. These both need
        // to be the same type, so they are interchangeable in a reference and
        // we can pick one to use.
        vector<Support> min_supports;
        vector<Support> average_supports;
        // And the min likelihood along each path
        vector<double> min_likelihoods;
        for(size_t i = 0; i < locus.allele_size(); i++) {
            // Go through all the Paths in the Locus
            const Path& path = locus.allele(i);
            
            // We use this to construct the allele sequence
            stringstream sequence_stream;
            
            // And this to compose the allele's name in terms of node IDs
            stringstream id_stream;
            
            // What's the total support for this path?
            Support total_support;
            
            // And the min
            Support min_support;
            
            // Also, what's the min likelihood
            double min_likelihood = INFINITY;
                            
            for(int64_t i = 1; i + 1 < path.mapping_size(); i++) {
                // For all but the first and last nodes (which are anchors)...
                
                // Grab the mapping
                const Mapping& here = path.mapping(i);
                
                // Look up the node ID we're visiting
                id_t node_id = path.mapping(i).position().node_id();
                // And the Node* in the graph
                Node* node = augmented.graph.get_node(node_id);
                
                // Grab the node sequence in the correct orientation.
                string added_sequence = node->sequence();
                if(path.mapping(i).position().is_reverse()) {
                    // If the node is traversed backward, we need to flip its sequence.
                    added_sequence = reverse_complement(added_sequence);
                }
                
                // Stick the sequence
                sequence_stream << added_sequence;
                
                // Record ID
                id_stream << to_string(node_id);
                if(i + 2 != path.mapping_size()) {
                    // If this is not the last mapping we're going to do, add a separator.
                    id_stream << "_";
                }
                
                // Peek at the next and previous Mappings
                const Mapping& prev = path.mapping(i - 1);
                const Mapping& next = path.mapping(i + 1);
                
                // How much support do we have for visiting this node?
                Support node_support = augmented.node_supports.at(node);
                
                // Grab the support for the edge we're traversing into the node
                Edge* in_edge = augmented.graph.get_edge(make_pair(to_right_side(to_visit(prev)),
                                                                   to_left_side(to_visit(here))));
                auto& in_support = augmented.edge_supports.at(in_edge);
                
                // Ditto for the edge we're traversing out of the node
                Edge* out_edge = augmented.graph.get_edge(make_pair(to_right_side(to_visit(here)),
                                                                    to_left_side(to_visit(next))));
                auto& out_support = augmented.edge_supports.at(out_edge);
                
                // Add node's support in to the total support for the alt. Scale by node length.
                // TODO: should the edges count here?
                total_support += node->sequence().size() * node_support;
                
                // Get the min support for the node and its edges
                auto node_min_support = support_min(node_support, support_min(in_support, out_support));
                
                // Take this as the minimum support if it's the first node, and min it with the min support otherwise.
                min_support = (i == 1 ? node_min_support : support_min(min_support, node_min_support));

                // Update minimum likelihood in the alt path
                min_likelihood = min(min_likelihood, augmented.node_likelihoods.at(node));
                
                // TODO: use edge likelihood here too?
                    
            }
            
            if(path.mapping_size() == 2) {
                // We just have the anchoring nodes and the edge between them.
                // Look at that edge specially.
                Edge* edge = augmented.graph.get_edge(make_pair(to_right_side(to_visit(path.mapping(0))),
                                                                to_left_side(to_visit(path.mapping(1)))));
                
                // Only use the support on the edge
                total_support = augmented.edge_supports.at(edge);
                min_support = total_support;
                
                // And the likelihood on the edge
                min_likelihood = augmented.edge_likelihoods.at(edge);
            }
            
            // Calculate the min and average Supports
            min_supports.push_back(min_support);
            // The support needs to get divided by bases, unless we're just a
            // single edge empty allele, in which case we're special.
            average_supports.push_back(sequence_stream.str().size() > 0 ? 
                total_support / sequence_stream.str().size() : 
                total_support);
            
            // Fill in the support for this allele in the Locus
            *locus.add_support() = use_average_support ? average_supports.back() : min_supports.back();
            
            // Fill in the other vectors
            // TODO: add this info to Locus? Or calculate later when emitting VCF?
            sequences.push_back(sequence_stream.str());
            id_lists.push_back(id_stream.str());
            min_likelihoods.push_back(min_likelihood);
        }
        
        // TODO: complain if multiple copies of the same string exist???
        
        // Decide which support vector we use to actually decide
        vector<Support>& supports = use_average_support ? average_supports : min_supports;
        
        // Now look at all the paths for the site and pick the top 2.
        int best_allele = -1;
        int second_best_allele = -1;
        for(size_t i = 0; i < locus.allele_size(); i++) {
            if(best_allele == -1 || total(supports[best_allele]) <= total(supports[i])) {
                // We have a new best. Demote the old best.
                second_best_allele = best_allele;
                best_allele = i;
            } else if(second_best_allele == -1 || total(supports[second_best_allele]) <= total(supports[i])) {
                // We're not better than the best, but we can demote the second best.
                second_best_allele = i;
            }
        }
        
        // We should always have a best allele; we may sometimes have a second best.
        assert(best_allele != -1);
        
        if(best_allele == 0 && second_best_allele == -1) {
            // This site isn't variable; don't bother with it.
            continue;
        }
        
        // See if the site is on a primary path, so we can use binned support.
        map<string, PrimaryPath>::iterator found_path = find_path(*site, primary_paths);
        
        // We need to figure out how much support a site ought to have
        Support baseline_support;
        if (expected_coverage != 0.0) {
            // Use the specified coverage override
            baseline_support.set_forward(expected_coverage / 2);
            baseline_support.set_reverse(expected_coverage / 2);
        } else if (found_path != primary_paths.end()) {
            // We're on a primary path, so we can find the appropriate bin
        
            // Since the variable part of the site is after the first anchoring node, where does it start?
            size_t variation_start = found_path->second.get_index().by_id.at(site->start().node_id()).first
                + augmented.graph.get_node(site->start().node_id())->sequence().size();
            
            // Look in the bins for the primary path to get the support there.
            baseline_support = found_path->second.get_support_at(variation_start);
            
        } else {
            // Just use the primary paths' average support, which may be 0 if there are none.
            baseline_support = PrimaryPath::get_average_support(primary_paths);
        }

        // Decide if we're an indel. We're an indel if the sequence lengths
        // aren't all equal between the ref and the alleles we're going to call.
        // TODO: is this backwards?
        bool is_indel = sequences.front().size() == sequences[best_allele].size() &&
            (second_best_allele == -1 || sequences.front().size() == sequences[second_best_allele].size());

        // We need to decide what to scale the bias limits by. We scale them up if this is an indel.
        double bias_multiple = is_indel ? indel_bias_multiple : 1.0;
        
        // How much support do we have for the top two alleles?
        Support site_support = supports.at(best_allele);
        if(second_best_allele != -1) {
            site_support += supports.at(second_best_allele);
        }
        
        // Pull out the different supports. Some of them may be the same.
        Support ref_support = supports.at(0);
        Support best_support = supports.at(best_allele);
        Support second_best_support; // Defaults to 0
        if(second_best_allele != -1) {
            second_best_support = supports.at(second_best_allele);
        }
        
        // As we do the genotype, we also compute the likelihood. Holds
        // likelihood log 10. Starts out at "completely wrong".
        double gen_likelihood = -1 * INFINITY;

        // Minimum allele depth of called alleles
        double min_site_support = 0;
        
        // This is where we'll put the genotype. We only actually add it to the
        // Locus if we are confident enough to actually call.
        Genotype genotype;
        
        // We're going to make some really bad calls at low depth. We can
        // pull them out with a depth filter, but for now just elide them.
        if(total(site_support) >= total(baseline_support) * min_fraction_for_call) {
            // We have enough to emit a call here.
            
            // If best and second best are close enough to be het, we call het.
            // Otherwise, we call hom best.
            
            // We decide closeness differently depending on whether best is ref or not.
            // In practice, we use this to slightly penalize homozygous ref calls
            // (by setting max_ref_het_bias higher than max_het_bias) and rather make a less
            // supported alt call instead.  This boost max sensitivity, and because
            // everything is homozygous ref by default in VCF, any downstream filters
            // will effectively reset these calls back to homozygous ref. 
            double bias_limit = (best_allele == 0) ? max_ref_het_bias : max_het_bias;
#ifdef debug
            cerr << best_allele << ", " << best_support << " and "
                << second_best_allele << ", " << second_best_support << endl;
            
            if (total(second_best_support) > 0) {
                cerr << "Bias: (limit " << bias_limit * bias_multiple << "):"
                    << total(best_support)/total(second_best_support) << endl;
            }
            
            cerr << bias_limit * bias_multiple * total(second_best_support) << " vs "
                << total(best_support) << endl;
                
            cerr << total(second_best_support) << " vs " << min_total_support_for_call << endl;
#endif
            if(second_best_allele != -1 &&
                bias_limit * bias_multiple * total(second_best_support) >= total(best_support) &&
                total(best_support) >= min_total_support_for_call &&
                total(second_best_support) >= min_total_support_for_call) {
                // There's a second best allele, and it's not too biased to
                // call, and both alleles exceed the minimum to call them
                // present.
                
                // Say both are present
                genotype.add_allele(best_allele);
                genotype.add_allele(second_best_allele);
                
                // Compute the likelihood for a best/second best het
                // Quick quality: combine likelihood and depth, using poisson for latter
                // TODO: revize which depth (cur: avg) / likelihood (cur: min) pair to use
                gen_likelihood = ln_to_log10(poisson_prob_ln(total(best_support), 0.5 * total(baseline_support))) +
                    ln_to_log10(poisson_prob_ln(total(second_best_support), 0.5 * total(baseline_support)));
                gen_likelihood += min_likelihoods.at(best_allele) + min_likelihoods.at(second_best_allele);
                
                // Save the likelihood in the Genotype
                genotype.set_likelihood(log10_to_ln(gen_likelihood));
                
                // Get minimum support for filter (not assuming it's second_best just to be sure)
                min_site_support = min(total(second_best_support), total(best_support));
                
                // Make the call
                *locus.add_genotype() = genotype;
                
            } else if(total(best_support) >= min_total_support_for_call) {
                // The second best allele isn't present or isn't good enough,
                // but the best allele has enough coverage that we can just call
                // two of it.
                
                // Say the best is present twice
                genotype.add_allele(best_allele);
                genotype.add_allele(best_allele);
                
                // Compute the likelihood for hom best allele
                gen_likelihood = ln_to_log10(poisson_prob_ln(total(best_support), total(baseline_support)));
                gen_likelihood += min_likelihoods.at(best_allele);

                // Save the likelihood in the Genotype
                genotype.set_likelihood(log10_to_ln(gen_likelihood));

                // Get minimum support for filter
                min_site_support = total(best_support);
                
                // Make the call
                *locus.add_genotype() = genotype;

            } else {
                // We can't really call this as anything.
                
                // Don't add the genotype to the locus
            }
        } else {
            // Depth too low. Say we have no idea.
            // TODO: elide variant?
            
            // Don't add the genotype to the locus
        }
        
        // Find the total support for the Locus across all alleles
        Support locus_support;
        for (auto& s : supports) {
            // Sum up all the Supports form all alleles (even the non-best/second-best).
            locus_support += s;
        }
        // Save support
        *locus.mutable_overall_support() = locus_support;
        
        // This function emits the given variant on the given primary path, as VCF.
        auto emit_variant = [&](const Locus& locus, PrimaryPath& primary_path) {
        
            // Since the variable part of the site is after the first anchoring node, where does it start?
            // TODO: we calculate this twice...
            size_t variation_start = primary_path.get_index().by_id.at(site->start().node_id()).first
                + augmented.graph.get_node(site->start().node_id())->sequence().size();
        
            // Keep track of the alleles that actually need to go in the VCF:
            // ref, best, and second-best (if any), some of which may overlap.
            set<int> used_alleles;
            used_alleles.insert(0);
            used_alleles.insert(best_allele);
            if(second_best_allele != -1) {
                used_alleles.insert(second_best_allele);
            }
        
            // Rewrite the sequences and variation_start to just represent the
            // actually variable part, by dropping any common prefix and common
            // suffix. We just do the whole thing in place, modifying the used
            // entries in sequences.
            
            auto shared_prefix_length = [&](bool backward) {
                size_t shortest_prefix = std::numeric_limits<size_t>::max();
                
                auto here = used_alleles.begin();
                if (here != used_alleles.end()) {
                    auto next = here;
                    next++;
                    while (next != used_alleles.end()) {
                        // Consider each allele and the next one after it, as
                        // long as we have both.
                    
                        // Figure out the shorter and the longer string
                        string* shorter = &sequences.at(*here);
                        string* longer = &sequences.at(*next);
                        if (shorter->size() > longer->size()) {
                            swap(shorter, longer);
                        }
                    
                        // Calculate the match length for this pair
                        size_t match_length;
                        if (backward) {
                            // Find out how far in from the right the first mismatch is.
                            auto mismatch_places = std::mismatch(shorter->rbegin(), shorter->rend(), longer->rbegin());
                            match_length = std::distance(shorter->rbegin(), mismatch_places.first);
                        } else {
                            // Find out how far in from the left the first mismatch is.
                            auto mismatch_places = std::mismatch(shorter->begin(), shorter->end(), longer->begin());
                            match_length = std::distance(shorter->begin(), mismatch_places.first);
                        }
                        
                        // The shared prefix of these strings limits the longest
                        // prefix shared by all strings.
                        shortest_prefix = min(shortest_prefix, match_length);
                    
                        here = next;
                        ++next;
                    }
                    
                    // Return the shortest universally shared prefix
                    return shortest_prefix;
                    
                } else {
                    // Only one string. Say no prefix is in common...
                    return (size_t) 0;
                }
            };
            // Trim off the shared prefix
            size_t shared_prefix = shared_prefix_length(false);
            for (auto allele : used_alleles) {
                sequences[allele] = sequences[allele].substr(shared_prefix);
            }
            // Add it onto the start coordinate
            variation_start += shared_prefix;
            
            // Then find and trim off the shared suffix
            size_t shared_suffix = shared_prefix_length(true);
            for (auto allele : used_alleles) {
                sequences[allele] = sequences[allele].substr(0, sequences[allele].size() - shared_suffix);
            }
            
            // Make a Variant
            vcflib::Variant variant;
            variant.sequenceName = contig_names_by_path_name.at(primary_path.get_name());
            variant.setVariantCallFile(vcf);
            variant.quality = 0;
            // Position should be 1-based and offset with our offset option.
            variant.position = variation_start + 1 + variant_offset;
            
            // Set the ID based on the IDs of the involved nodes. Note that the best
            // allele may have no nodes (because it's a pure edge)
            variant.id = id_lists.at(best_allele);
            if(second_best_allele != -1 && !id_lists.at(second_best_allele).empty()) {
                // Add the second best allele's nodes in.
                variant.id += "-" + id_lists.at(second_best_allele);
            }
            
            
            if(sequences.at(0).empty() ||
                (best_allele != -1 && sequences.at(best_allele).empty()) ||
                (second_best_allele != -1 && sequences.at(second_best_allele).empty())) {
                
                // Fix up the case where we have an empty allele.
                
                // We need to grab the character before the variable part of the
                // site in the reference.
                assert(variation_start > 0);
                string extra_base = char_to_string(primary_path.get_index().sequence.at(variation_start - 1));
                
                for(auto& seq : sequences) {
                    // Stick it on the front of all the allele sequences
                    seq = extra_base + seq;
                }
                
                // Budge the variant left
                variant.position--;
            }
            
            // Add the ref allele to the variant
            create_ref_allele(variant, sequences.front());
            
            // Add the best allele
            assert(best_allele != -1);
            int best_alt = add_alt_allele(variant, sequences.at(best_allele));
            
            int second_best_alt = (second_best_allele == -1) ? -1 : add_alt_allele(variant, sequences.at(second_best_allele));
            
            
            // Say we're going to spit out the genotype for this sample.        
            variant.format.push_back("GT");
            auto& genotype_vector = variant.samples[sample_name]["GT"];

            if (locus.genotype_size() > 0) {
                // We actually made a call. Emit the first genotype, which is the call.
                
                // We need to rewrite the allele numbers to alt numbers, since
                // we aren't keeping all the alleles in the VCF, so we can't use
                // the natural conversion of Genotype to VCF genotype string.
                
                // Emit parts into this stream
                stringstream stream;
                for (size_t i = 0; i < genotype.allele_size(); i++) {
                    // For each allele called as present in the genotype
                    
                    // Convert from allele number to alt number
                    if (genotype.allele(i) == best_allele) {
                        stream << best_alt;
                    } else if (genotype.allele(i) == second_best_allele) {
                        stream << second_best_alt;
                    } else {
                        throw runtime_error("Allele " + to_string(genotype.allele(i)) +
                            " is not best or second-best and has no alt");
                    }
                    
                    if (i + 1 != genotype.allele_size()) {
                        // Write a separator after all but the last one
                        stream << (genotype.is_phased() ? '|' : '/');
                    }
                }
                
                // Save the finished genotype
                genotype_vector.push_back(stream.str());              
            } else {
                // Say there's no call here
                genotype_vector.push_back("./.");
            }
            
            // Now fill in all the other variant info/format stuff

            if((best_allele != 0 && is_ref.at(best_allele)) || 
                (second_best_allele != 0 && second_best_allele != -1 && is_ref.at(second_best_allele))) {
                // Flag the variant as reference if either of its two best alleles
                // is known but not the primary path. Don't put in a false entry if
                // it isn't known, because vcflib will spit out the flag anyway...
                variant.infoFlags["XREF"] = true;
            }
            
            // Add depth for the variant and the samples
            Support total_support = ref_support;
            if(best_allele != 0) {
                // Add in the depth from the best allele, which is not already counted
                total_support += best_support;
            }
            if(second_best_allele != -1 && second_best_allele != 0) {
                // Add in the depth from the second allele
                total_support += second_best_support;
            }
            string depth_string = to_string((int64_t)round(total(total_support)));
            variant.format.push_back("DP");
            variant.samples[sample_name]["DP"].push_back(depth_string);
            variant.info["DP"].push_back(depth_string); // We only have one sample, so variant depth = sample depth
            
            // Also allelic depths
            variant.format.push_back("AD");
            variant.samples[sample_name]["AD"].push_back(to_string((int64_t)round(total(ref_support))));
            // And strand biases
            variant.format.push_back("SB");
            variant.samples[sample_name]["SB"].push_back(to_string((int64_t)round(ref_support.forward())));
            variant.samples[sample_name]["SB"].push_back(to_string((int64_t)round(ref_support.reverse())));
            // Also allelic likelihoods (from minimum values found on their paths)
            variant.format.push_back("AL");
            variant.samples[sample_name]["AL"].push_back(to_string_ss(min_likelihoods.at(0)));
            if(best_allele != 0) {
                // If our best allele isn't ref, it comes next.
                variant.samples[sample_name]["AD"].push_back(to_string((int64_t)round(total(best_support))));
                variant.samples[sample_name]["SB"].push_back(to_string((int64_t)round(best_support.forward())));
                variant.samples[sample_name]["SB"].push_back(to_string((int64_t)round(best_support.reverse())));
                variant.samples[sample_name]["AL"].push_back(to_string_ss(min_likelihoods.at(best_allele)));
            }
            if(second_best_allele != -1 && second_best_allele != 0) {
                // If our second best allele is real and not ref, it comes next.
                variant.samples[sample_name]["AD"].push_back(to_string((int64_t)round(total(second_best_support))));
                variant.samples[sample_name]["SB"].push_back(to_string((int64_t)round(second_best_support.forward())));
                variant.samples[sample_name]["SB"].push_back(to_string((int64_t)round(second_best_support.reverse())));
                variant.samples[sample_name]["AL"].push_back(to_string_ss(min_likelihoods.at(second_best_allele)));
            }
            
            // And total alt allele depth for the alt alleles
            Support alt_support; // Defaults to 0
            if(best_allele != 0) {
                alt_support += best_support;
            }
            if(second_best_allele != -1 && second_best_allele != 0) {
                alt_support += second_best_support;
            }
            variant.format.push_back("XAAD");
            variant.samples[sample_name]["XAAD"].push_back(to_string((int64_t)round(total(alt_support))));

            // Copy over the quality value
            variant.quality = -10. * log10(1. - pow(10, gen_likelihood));

            // Apply Min Allele Depth cutoff and store result in Filter column
            variant.filter = min_site_support >= min_mad_for_filter ? "PASS" : "FAIL";
            
            if(can_write_alleles(variant)) {
                // No need to check for collisions because we assume sites are correctly found.
                
                // Output the created VCF variant.
                cout << variant << endl;
            } else {
                if (verbose) {
                    cerr << "Variant is too large" << endl;
                }
                // TODO: track bases lost again
            }
            
        };
        
        if (convert_to_vcf) {
            // We want to emit VCF
            if(found_path != primary_paths.end()) {
                // And this site is on a primary path
                
                // Emit the variant for this Locus
                emit_variant(locus, found_path->second);
            }
            // Otherwise discard it as off-path
            // TODO: update bases lost
        } else {
            // Emit the locus itself
            locus_buffer.push_back(locus);
            stream::write_buffered(cout, locus_buffer, locus_buffer_size);
        }
        
        // We called a site
        called_loci++;
        
        // Mark all the nodes and edges in the site as covered
        auto contents = site_manager.deep_contents(site, augmented.graph, true);
        for (auto* node : contents.first) {
            covered_nodes.insert(node);
        }
        for (auto* edge : contents.second) {
            covered_edges.insert(edge);
        }
    }
    
    if (verbose) {
        cerr << "Called " << called_loci << " loci" << endl;
    }
    
    // OK now we have handled all the real sites. But there are still nodes and
    // edges that we might want to call as present or absent.
    
    if (!convert_to_vcf) {
        
        size_t extra_loci = 0;
        
        augmented.graph.for_each_edge([&](Edge* e) {
            // We want to make calls on all the edges that aren't covered yet
            if (covered_edges.count(e)) {
                // Skip this edge
                return;
            }
            
            // Make a couple of fake Visits
            Visit from_visit;
            from_visit.set_node_id(e->from());
            from_visit.set_backward(e->from_start());
            Visit to_visit;
            to_visit.set_node_id(e->to());
            to_visit.set_backward(e->to_end());
            
            // Make a Locus for the edge
            Locus locus;
            
            // Give it an allele
            Path* path = locus.add_allele();
            
            // Fill in 
            *path->add_mapping() = to_mapping(from_visit, augmented.graph);
            *path->add_mapping() = to_mapping(to_visit, augmented.graph);
            
            // Set the support
            *locus.add_support() = augmented.edge_supports[e];
            *locus.mutable_overall_support() = augmented.edge_supports[e];
            
            // Decide on the genotype
            Genotype gt;
            
            // TODO: use the coverage bins
            if (total(locus.support(0)) > total(PrimaryPath::get_average_support(primary_paths)) * 0.25) {
                // We're closer to 1 copy than 0 copies
                gt.add_allele(0);
                
                if (total(locus.support(0)) > total(PrimaryPath::get_average_support(primary_paths)) * 0.75) {
                    // We're closer to 2 copies than 1 copy
                    gt.add_allele(0);
                }
            }
            // Save the genotype with 0, 1, or 2 copies.
            *locus.add_genotype() = gt;
            
            // Send out the locus
            locus_buffer.push_back(locus);
            stream::write_buffered(cout, locus_buffer, locus_buffer_size);
            
            extra_loci++;
            
        });
        
        // TODO: look at average node coverages and do node loci (in case any nodes have no edges?)
    
        // Flush the buffer of Locus objects we have to write
        stream::write_buffered(cout, locus_buffer, 0);
        
        if (verbose) {
            cerr << "Called " << extra_loci << " extra loci with copy number estimates" << endl;
        }
        
    }
    
}

bool Call2Vcf::is_reference(const SnarlTraversal& trav, AugmentedGraph& augmented) {
    
    // Keep track of the previous NodeSide
    NodeSide previous;
    
    // We'll call this function with each visit in turn.
    // If it ever returns false, the whole thing is nonreference.
    auto experience_visit = [&](const Visit& visit) {
        // TODO: handle nested sites
        assert(visit.node_id());
        
        if (previous.node != 0) {
            // Consider the edge from the previous visit
            Edge* edge = augmented.graph.get_edge(previous, to_left_side(visit));
            
            if (augmented.edge_calls.at(edge) != CALL_REFERENCE) {
                // Found a novel edge!
                return false;
            }
        }
        
        if (augmented.node_calls.at(augmented.graph.get_node(visit.node_id())) != CALL_REFERENCE) {
            // This node itself is novel
            return false;
        }
        
        // Remember we want an edge from this visit when we look at the next
        // one.
        previous = to_right_side(visit);
        
        // This visit is known.
        return true;
    };
    
    // Make sure we visit a ref start node
    if (!experience_visit(trav.snarl().start())) {
        return false;
    }
    
    // Then all the internal nodes
    for (size_t i = 0; i < trav.visits_size(); i++) {
        if (!experience_visit(trav.visits(i))) {
            return false;
        }
    }
    
    // And finally the end node
    if (!experience_visit(trav.snarl().end())) {
        return false;
    }
    
    // And if we make it through it's a reference traversal.
    return true;
        
}

}
