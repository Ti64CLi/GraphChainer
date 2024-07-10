#include <omp.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <limits>
#include <csignal>
#include "Aligner.h"
#include "stream.hpp"
#include "ThreadReadAssertion.h"
#include "EValue.h"

int main(int argc, char** argv)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	std::cout << "GraphChainer " << VERSION << std::endl;
	std::cerr << "GraphChainer " << VERSION << std::endl;

#ifndef NOBUILTINPOPCOUNT
	if (__builtin_cpu_supports("popcnt") == 0)
	{
		std::cerr << "CPU does not support builtin popcount operation" << std::endl;
		std::cerr << "recompile with -DNOBUILTINPOPCOUNT" << std::endl;
		std::abort();
	}
#endif

	struct sigaction act;
	act.sa_handler = ThreadReadAssertion::signal;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGSEGV, &act, 0);

	boost::program_options::options_description mandatory("Mandatory parameters");
	mandatory.add_options()
		("graph,g", boost::program_options::value<std::string>(), "input graph (.gfa / .vg)")
		("reads,f", boost::program_options::value<std::vector<std::string>>()->multitoken(), "input reads (fasta or fastq, uncompressed or gzipped)")
		("alignments-out,a", boost::program_options::value<std::vector<std::string>>(), "output alignment file (.gaf/.gam/.json)")
	;
	boost::program_options::options_description clcparams("Colinear chaining parameters");
	clcparams.add_options()
		("sampling-step", boost::program_options::value<long long>(), "Sampling step factor (default 1). Use >1 (<1, >0) for faster (slower), but less (more) accurate alignments. It increases (decreases) the sampling sparsity of fragments.")
		("colinear-split-len", boost::program_options::value<long long>(), "The length of the fragments in which the long read is split to create anchors. [default 35]")
		("colinear-split-gap", boost::program_options::value<long long>(), "The distance between consecutive fragments [default 35]. If --sampling-step is set, then always --colinear-split-gap = ceil(--sampling-step * --colinear-split-len).")
		("colinear-gap", boost::program_options::value<long long>(), "When converting an optimal chain of anchors into an alignment path, split the path if the distance between consecutive anchors is greater than this value [default 10000].")
		// ("mpc-index,i", boost::program_options::value<std::string>(), "minimium path cover index filename")
		("fast-mode", "(Development purposes) Skip edit distance computation after chaining (output the path instead of alignment)")
	;

	boost::program_options::options_description general("General parameters");
	general.add_options()
		("help,h", "help message")
		("version", "print version")
		("threads,t", boost::program_options::value<size_t>(), "number of threads (int) (default 1)")
		("verbose", "print progress messages")
		("short-verbose", "print shorter progress messages")
	;

	boost::program_options::options_description generalGA("General parameters from GraphAligner");
	generalGA.add_options()
		("E-cutoff", boost::program_options::value<double>(), "discard alignments with E-value > arg")
		("all-alignments", "return all alignments instead of the best non-overlapping alignments")
		("extra-heuristic", "use heuristics to discard more seed hits")
		("try-all-seeds", "don't use heuristics to discard seed hits")
		("global-alignment", "force the read to be aligned end-to-end even if the alignment score is poor")
		("optimal-alignment", "calculate the optimal alignment (VERY SLOW)")
		("X-drop", boost::program_options::value<int>(), "use X-drop heuristic to end alignment with score cutoff arg (int)")
		("precise-clipping", boost::program_options::value<double>(), "clip the alignment ends more precisely with arg as the identity cutoff between correct / wrong alignments (float)")
		("cigar-match-mismatch", "use M for matches and mismatches in the cigar string instead of = and X")
		("generate-path", "generate a path and write to file instead of aligning reads from file")
		("generate-path-seed", boost::program_options::value<long long>(), "seed for generating path")
		("graph-statistics", "show statistics (size, width) of the input graph then exit")
	;

	boost::program_options::options_description seeding("Seeding parameters from GraphAligner");
	seeding.add_options()
		("seeds-clustersize", boost::program_options::value<size_t>(), "discard seed clusters with fewer than arg seeds (int)")
		("seeds-extend-density", boost::program_options::value<double>(), "extend up to approximately the best (arg * sequence length) seeds (double) (-1 for all)")
		("seeds-minimizer-length", boost::program_options::value<size_t>(), "k-mer length for minimizer seeding (int)")
		("seeds-minimizer-windowsize", boost::program_options::value<size_t>(), "window size for minimizer seeding (int)")
		("seeds-minimizer-density", boost::program_options::value<double>(), "keep approximately (arg * sequence length) least common minimizers (double) (-1 for all)")
		("seeds-minimizer-ignore-frequent", boost::program_options::value<double>(), "ignore arg most frequent fraction of minimizers (double)")
		("seeds-mum-count", boost::program_options::value<size_t>(), "arg longest maximal unique matches fully contained in a node (int) (-1 for all)")
		("seeds-mem-count", boost::program_options::value<size_t>(), "arg longest maximal exact matches fully contained in a node (int) (-1 for all)")
		("seeds-mxm-length", boost::program_options::value<size_t>(), "minimum length for maximal unique / exact matches (int)")
		("seeds-mxm-cache-prefix", boost::program_options::value<std::string>(), "store the mum/mem seeding index to the disk for reuse, or reuse it if it exists (filename prefix)")
		("seeds-file,s", boost::program_options::value<std::vector<std::string>>()->multitoken(), "external seeds (.gam)")
		("seedless-DP", "no seeding, instead use DP alignment algorithm for the entire first row. VERY SLOW except on tiny graphs")
		("DP-restart-stride", boost::program_options::value<size_t>(), "if --seedless-DP doesn't span the entire read, restart after arg base pairs (int)")
	;
	boost::program_options::options_description alignment("Extension parameters from GraphAligner");
	alignment.add_options()
		("bandwidth,b", boost::program_options::value<size_t>(), "alignment bandwidth (int)")
		("ramp-bandwidth,B", boost::program_options::value<size_t>(), "ramp bandwidth (int)")
		("tangle-effort,C", boost::program_options::value<size_t>(), "tangle effort limit, higher results in slower but more accurate alignments (int) (-1 for unlimited)")
		("high-memory", "use slightly less CPU but a lot more memory")
	;
	boost::program_options::options_description hidden("hidden");
	hidden.add_options()
		("schedule-inverse-E-sum", "optimally select a non-overlapping set based on the sum of inverse E-values")
		("schedule-inverse-E-product", "optimally select a non-overlapping set based on the product of inverse E-values")
		("schedule-score", "optimally select a non-overlapping set based on the alignment score")
		("schedule-length", "optimally select a non-overlapping set based on the alignment length")
		("greedy-length", "greedily select a non-overlapping alignment set based on alignment length")
		("greedy-E", "greedily select a non-overlapping alignment set based on E-value")
		("greedy-score", "greedily select a non-overlapping alignment set based on alignment score")
		("no-colinear-chaining", "do not run colinear chaining and align as in GraphAligner, default parameters")
		("symmetric-colinear-chaining", "use the symmetric verion of colinear chainin (default is asymmetric)")
		("use-mem", "use MEM seeds for colinear chaining (default is ussing all seed types)")
	;

	boost::program_options::options_description cmdline_options;
	cmdline_options.add(mandatory).add(clcparams).add(general).add(generalGA).add(seeding).add(alignment).add(hidden);

	boost::program_options::variables_map vm;
	try
	{
		boost::program_options::store(boost::program_options::parse_command_line(argc, argv, cmdline_options), vm);
	}
	catch (std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		std::cerr << "run with option -h for help" << std::endl;
		std::exit(1);
	}
	boost::program_options::notify(vm);

	if (vm.count("help"))
	{
		std::cerr << mandatory << std::endl << clcparams << std::endl << general << std::endl << generalGA << std::endl << seeding << std::endl << alignment << std::endl;
		std::exit(0);
	}
	if (vm.count("version"))
	{
		std::cout << "Version " << VERSION << std::endl;
		std::exit(0);
	}

	AlignerParams params;
	params.graphFile = "";
	params.outputGAMFile = "";
	params.outputJSONFile = "";
	params.outputGAFFile = "";
	params.outputCorrectedFile = "";
	params.outputCorrectedClippedFile = "";
	params.numThreads = 1;
	params.initialBandwidth = 0;
	params.rampBandwidth = 0;
	params.dynamicRowStart = false;
	params.maxCellsPerSlice = std::numeric_limits<decltype(params.maxCellsPerSlice)>::max();
	params.verboseMode = false;
	params.shortVerboseMode = false;
	params.tryAllSeeds = false;
	params.highMemory = false;
	params.mxmLength = 20;
	params.mumCount = 0;
	params.memCount = 0;
	params.seederCachePrefix = "";
	params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::GreedyLength; //todo pick better default
	params.selectionECutoff = -1;
	params.forceGlobal = false;
	params.compressCorrected = false;
	params.compressClipped = false;
	params.preciseClipping = false;
	params.minimizerSeedDensity = 0;
	params.minimizerLength = 19;
	params.minimizerWindowSize = 30;
	params.seedClusterMinSize = 1;
	params.minimizerDiscardMostNumerousFraction = 0.0002;
	params.seedExtendDensity = 0.002;
	params.nondeterministicOptimizations = false;
	params.optimalDijkstra = false;
	params.preciseClippingIdentityCutoff = 0.5;
	params.Xdropcutoff = 0;
	params.DPRestartStride = 0;
	params.cigarMatchMismatchMerge = false;
	params.colinearChaining = true;
	params.symmetricColinearChaining = false;
	params.generatePath = false;
	params.generatePathSeed = 0;
	params.IndexMpcFile = "";
	params.fastMode = false;
	params.graphStatistics = false;

	std::vector<std::string> outputAlns;
	bool paramError = false;

	// Using GraphAligner's vg presets:
	params.minimizerSeedDensity = 10;
	params.minimizerLength = 15;
	params.minimizerWindowSize = 20;
	params.seedExtendDensity = -1;
	params.minimizerDiscardMostNumerousFraction = 0.001;
	params.nondeterministicOptimizations = false;
	params.initialBandwidth = 10;

	if (vm.count("fast-mode"))
		params.fastMode = true;
	
	if (vm.count("no-colinear-chaining"))
		params.colinearChaining = false;

	if (params.colinearChaining)
	{
		if (vm.count("symmetric-colinear-chaining")) {
			params.symmetricColinearChaining = true;
		}
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::All;
		if (vm.count("use-mem")) {
			std::cout << "Using MEM seeds" << std::endl;
			params.mumCount = 0;
			params.memCount = std::numeric_limits<size_t>::max();
			params.mxmLength = 2;
			params.minimizerSeedDensity = 0;
			params.seedFiles.clear();
			params.seederCachePrefix = "current_mem";
		} else {
			params.tryAllSeeds = true;
		}

		params.colinearGap = 10000;
		params.colinearSplitLen = 35;
		params.colinearSplitGap = 35;
        params.samplingStep = 1;
	}

	if (vm.count("graph")) params.graphFile = vm["graph"].as<std::string>();
	if (vm.count("reads")) params.fastqFiles = vm["reads"].as<std::vector<std::string>>();
	if (vm.count("alignments-out")) outputAlns = vm["alignments-out"].as<std::vector<std::string>>();
	if (vm.count("threads")) params.numThreads = vm["threads"].as<size_t>();
	if (vm.count("bandwidth")) params.initialBandwidth = vm["bandwidth"].as<size_t>();

	if (vm.count("seeds-extend-density")) params.seedExtendDensity = vm["seeds-extend-density"].as<double>();
	if (vm.count("seeds-minimizer-ignore-frequent")) params.minimizerDiscardMostNumerousFraction = vm["seeds-minimizer-ignore-frequent"].as<double>();
	if (vm.count("seeds-clustersize")) params.seedClusterMinSize = vm["seeds-clustersize"].as<size_t>();
	if (vm.count("seeds-minimizer-density")) params.minimizerSeedDensity = vm["seeds-minimizer-density"].as<double>();
	if (vm.count("seeds-minimizer-length")) params.minimizerLength = vm["seeds-minimizer-length"].as<size_t>();
	if (vm.count("seeds-minimizer-windowsize")) params.minimizerWindowSize = vm["seeds-minimizer-windowsize"].as<size_t>();
	if (vm.count("seeds-file")) params.seedFiles = vm["seeds-file"].as<std::vector<std::string>>();
	if (vm.count("seeds-mxm-length")) params.mxmLength = vm["seeds-mxm-length"].as<size_t>();
	if (vm.count("seeds-mem-count")) params.memCount = vm["seeds-mem-count"].as<size_t>();
	if (vm.count("seeds-mum-count")) params.mumCount = vm["seeds-mum-count"].as<size_t>();
	if (vm.count("seeds-mxm-cache-prefix")) params.seederCachePrefix = vm["seeds-mxm-cache-prefix"].as<std::string>();
	if (vm.count("seedless-DP")) params.dynamicRowStart = true;
	
	if (vm.count("colinear-gap")) params.colinearGap = vm["colinear-gap"].as<long long>();
	if (vm.count("colinear-split-len")) params.colinearSplitLen = vm["colinear-split-len"].as<long long>();
	if (vm.count("colinear-split-gap")) params.colinearSplitGap = vm["colinear-split-gap"].as<long long>();
	if (vm.count("sampling-step")) params.samplingStep = vm["sampling-step"].as<long long>();
	if (vm.count("mpc-index")) params.IndexMpcFile = vm["mpc-index"].as<std::string>();
	
	if (params.samplingStep != 1)
	{
		if (vm.count("colinear-split-gap"))
		{
			std::cerr << "WARNING: --sampling-step and --colinear-split-gap are both set! --colinear-split-gap will be ignored, and set to (--sampling-step * --colinear-split-len)" << std::endl;
		}
		params.colinearSplitGap = ceil(params.samplingStep * params.colinearSplitLen);
	}
	
	if (vm.count("DP-restart-stride")) params.DPRestartStride = vm["DP-restart-stride"].as<size_t>();

	if (vm.count("extra-heuristic")) params.nondeterministicOptimizations = true;
	if (vm.count("ramp-bandwidth")) params.rampBandwidth = vm["ramp-bandwidth"].as<size_t>();
	if (vm.count("tangle-effort")) params.maxCellsPerSlice = vm["tangle-effort"].as<size_t>();
	if (vm.count("verbose")) params.verboseMode = true;
	if (vm.count("short-verbose")) params.shortVerboseMode = true;
	if (vm.count("try-all-seeds")) params.tryAllSeeds = true;
	if (vm.count("high-memory")) params.highMemory = true;
	if (vm.count("cigar-match-mismatch")) params.cigarMatchMismatchMerge = true;
	if (vm.count("generate-path")) params.generatePath = true;
	if (vm.count("generate-path-seed")) params.generatePathSeed = vm["generate-path-seed"].as<long long>();
	if (vm.count("graph-statistics")) params.graphStatistics = true;

	int resultSelectionMethods = 0;
	if (vm.count("all-alignments"))
	{
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::All;
		params.tryAllSeeds = true;
		resultSelectionMethods += 1;
	}
	if (vm.count("greedy-length"))
	{
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::GreedyLength;
		resultSelectionMethods += 1;
	}
	if (vm.count("E-cutoff"))
	{
		params.selectionECutoff = vm["E-cutoff"].as<double>();
	}
	if (vm.count("schedule-inverse-E-sum"))
	{
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::ScheduleInverseESum;
		resultSelectionMethods += 1;
	}
	if (vm.count("schedule-inverse-E-product"))
	{
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::ScheduleInverseEProduct;
		resultSelectionMethods += 1;
	}
	if (vm.count("schedule-score"))
	{
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::ScheduleScore;
		resultSelectionMethods += 1;
	}
	if (vm.count("schedule-length"))
	{
		params.alignmentSelectionMethod = AlignmentSelection::SelectionMethod::ScheduleLength;
		resultSelectionMethods += 1;
	}
	if (vm.count("verbose")) params.verboseMode = true;
	if (vm.count("short-verbose")) params.shortVerboseMode = true;
	if (vm.count("try-all-seeds")) params.tryAllSeeds = true;
	if (vm.count("high-memory")) params.highMemory = true;
	if (vm.count("global-alignment")) params.forceGlobal = true;
	if (vm.count("precise-clipping"))
	{
		params.preciseClipping = true;
		params.preciseClippingIdentityCutoff = vm["precise-clipping"].as<double>();
		if (params.preciseClippingIdentityCutoff < 0.001 || params.preciseClippingIdentityCutoff > 0.999)
		{
			std::cerr << "precise clipping identity cutoff must be between 0.001 and 0.999" << std::endl;
			paramError = true;
		}
		if (params.preciseClippingIdentityCutoff >= 0.001 && params.preciseClippingIdentityCutoff < 0.501)
		{
			std::cerr << "Warning: precise clipping identity cutoff set below 0.501. Output will almost certainly contain spurious alignments." << std::endl;
		}
	}
	if (vm.count("X-drop"))
	{
		params.Xdropcutoff = vm["X-drop"].as<int>();
		if (params.Xdropcutoff < 1)
		{
			std::cerr << "X-drop score cutoff must be > 1" << std::endl;
			paramError = true;
		}
	}
	if (vm.count("optimal-alignment")) params.optimalDijkstra = true;

	if (params.graphFile == "")
	{
		std::cerr << "graph file must be given" << std::endl;
		paramError = true;
	}
	if (params.fastqFiles.size() == 0)
	{
		std::cerr << "read file must be given" << std::endl;
		paramError = true;
	}
	if (outputAlns.size() == 0 && params.outputCorrectedFile == "" && params.outputCorrectedClippedFile == "")
	{
		std::cerr << "alignments-out must be given" << std::endl;
		paramError = true;
	}
	for (std::string file : outputAlns)
	{
		if (file.size() >= 4 && file.substr(file.size()-4) == ".gam")
		{
			params.outputGAMFile = file;
		}
		else if (file.size() >= 5 && file.substr(file.size()-5) == ".json")
		{
			params.outputJSONFile = file;
		}
		else if (file.size() >= 4 && file.substr(file.size()-4) == ".gaf")
		{
			params.outputGAFFile = file;
		}
		else
		{
			std::cerr << "unknown output alignment format (" << file << "), must be either .gaf, .gam or .json" << std::endl;
			paramError = true;
		}
	}
	if (params.outputCorrectedFile != "" && params.outputCorrectedFile.substr(params.outputCorrectedFile.size()-3) != ".fa" && params.outputCorrectedFile.substr(params.outputCorrectedFile.size()-6) != ".fasta" && params.outputCorrectedFile.substr(params.outputCorrectedFile.size()-6) != ".fa.gz" && params.outputCorrectedFile.substr(params.outputCorrectedFile.size()-9) != ".fasta.gz")
	{
		std::cerr << "unknown output corrected read format, must be .fa or .fa.gz" << std::endl;
		paramError = true;
	}
	if (params.outputCorrectedClippedFile != "" && params.outputCorrectedClippedFile.substr(params.outputCorrectedClippedFile.size()-3) != ".fa" && params.outputCorrectedClippedFile.substr(params.outputCorrectedClippedFile.size()-6) != ".fasta" && params.outputCorrectedClippedFile.substr(params.outputCorrectedClippedFile.size()-6) != ".fa.gz" && params.outputCorrectedClippedFile.substr(params.outputCorrectedClippedFile.size()-9) != ".fasta.gz")
	{
		std::cerr << "unknown output corrected read format, must be .fa or .fa.gz" << std::endl;
		paramError = true;
	}
	if (params.numThreads < 1)
	{
		std::cerr << "number of threads must be >= 1" << std::endl;
		paramError = true;
	}
	if (params.initialBandwidth < 1 && !params.optimalDijkstra)
	{
		std::cerr << "default bandwidth must be >= 1" << std::endl;
		paramError = true;
	}
	if (params.rampBandwidth != 0 && params.rampBandwidth <= params.initialBandwidth)
	{
		std::cerr << "ramp bandwidth must be higher than default bandwidth" << std::endl;
		paramError = true;
	}
	if (params.mxmLength < 2)
	{
		std::cerr << "mum/mem minimum length must be >= 2" << std::endl;
		paramError = true;
	}
	if (params.minimizerLength >= sizeof(size_t)*8/2)
	{
		std::cerr << "Maximum minimizer length is " << (sizeof(size_t)*8/2)-1 << std::endl;
		paramError = true;
	}
	if (params.minimizerDiscardMostNumerousFraction < 0 || params.minimizerDiscardMostNumerousFraction >= 1)
	{
		std::cerr << "Minimizer discard fraction must be 0 <= x < 1" << std::endl;
		paramError = true;
	}
	if (params.minimizerSeedDensity < 0 && params.minimizerSeedDensity != -1)
	{
		std::cerr << "Minimizer density can't be negative" << std::endl;
		paramError = true;
	}
	if (params.seedExtendDensity <= 0 && params.seedExtendDensity != -1)
	{
		std::cerr << "Seed extension density can't be negative" << std::endl;
		paramError = true;
	}
	int pickedSeedingMethods = ((params.dynamicRowStart) ? 1 : 0) + ((params.seedFiles.size() > 0) ? 1 : 0) + ((params.mumCount != 0) ? 1 : 0) + ((params.memCount != 0) ? 1 : 0) + ((params.minimizerSeedDensity != 0) ? 1 : 0);
	if (params.optimalDijkstra && (params.initialBandwidth > 0))
	{
		std::cerr << "--optimal-alignment set, ignoring parameter --bandwidth" << std::endl;
	}
	if (params.optimalDijkstra && (params.rampBandwidth > 0))
	{
		std::cerr << "--optimal-alignment set, ignoring parameter --ramp-bandwidth" << std::endl;
	}
	if (params.optimalDijkstra && (params.maxCellsPerSlice != std::numeric_limits<decltype(params.maxCellsPerSlice)>::max()))
	{
		std::cerr << "--optimal-alignment set, ignoring parameter --tangle-effort" << std::endl;
	}
	if (params.optimalDijkstra && pickedSeedingMethods > 0)
	{
		if (params.dynamicRowStart) std::cerr << "--optimal-alignment cannot be combined with --first-rows-DP" << std::endl;
		if (params.seedFiles.size() > 0) std::cerr << "--optimal-alignment cannot be combined with --seeds-file" << std::endl;
		if (params.mumCount > 0) std::cerr << "--optimal-alignment cannot be combined with --seeds-mum-count" << std::endl;
		if (params.memCount > 0) std::cerr << "--optimal-alignment cannot be combined with --seeds-mem-count" << std::endl;
		if (params.minimizerSeedDensity > 0) std::cerr << "--optimal-alignment cannot be combined with --seeds-minimizer-density" << std::endl;
		std::cerr << "pick only one seeding method" << std::endl;
		paramError = true;
	}
	if (pickedSeedingMethods == 0 && !params.optimalDijkstra)
	{
		std::cerr << "pick a seeding method" << std::endl;
		paramError = true;
	}
	if (pickedSeedingMethods > 1)
	{
		std::cerr << "pick only one seeding method" << std::endl;
		paramError = true;
	}
	if (params.Xdropcutoff > 0 && !params.preciseClipping)
	{
		std::cerr << "--X-drop is set but --precise-clipping is not, using default value of --precise-clipping .66" << std::endl;
		params.preciseClipping = true;
		params.preciseClippingIdentityCutoff = 0.66;
	}
	if (params.tryAllSeeds && vm.count("seeds-extend-density") && vm["seeds-extend-density"].as<double>() != -1)
	{
		std::cerr << "WARNING: --try-all-seeds and --seeds-extend-density are both set! --seeds-extend-density will be ignored" << std::endl;
		params.seedExtendDensity = -1;
	}
	if (resultSelectionMethods > 1)
	{
		std::cerr << "pick only one method for selecting outputted alignments" << std::endl;
		paramError = true;
	}

	if (paramError)
	{
		std::cerr << "run with option -h for help" << std::endl;
		std::exit(1);
	}

	if (params.outputCorrectedFile.size() >= 3 && params.outputCorrectedFile.substr(params.outputCorrectedFile.size()-3) == ".gz")
	{
		params.compressCorrected = true;
	}

	if (params.outputCorrectedClippedFile.size() >= 3 && params.outputCorrectedClippedFile.substr(params.outputCorrectedClippedFile.size()-3) == ".gz")
	{
		params.compressClipped = true;
	}

	omp_set_num_threads(params.numThreads);

	alignReads(params);

	return 0;
}
