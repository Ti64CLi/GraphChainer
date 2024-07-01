#ifndef Aligner_h
#define Aligner_h

#include <string>
#include <vector>
#include "AlignmentGraph.h"
#include "vg.pb.h"
#include "AlignmentSelection.h"

struct AlignerParams
{
	std::string graphFile;
	std::vector<std::string> fastqFiles;
	size_t numThreads;
	size_t initialBandwidth;
	size_t rampBandwidth;
	bool dynamicRowStart;
	size_t maxCellsPerSlice;
	std::vector<std::string> seedFiles;
	std::string outputGAMFile;
	std::string outputJSONFile;
	std::string outputGAFFile;
	std::string outputCorrectedFile;
	std::string outputCorrectedClippedFile;
	std::string IndexMpcFile;
	bool verboseMode;
	bool shortVerboseMode;
	bool tryAllSeeds;
	bool highMemory;
	size_t mxmLength;
	size_t mumCount;
	size_t memCount;
	std::string seederCachePrefix;
	AlignmentSelection::SelectionMethod alignmentSelectionMethod;
	double selectionECutoff;
	bool forceGlobal;
	bool compressCorrected;
	bool compressClipped;
	bool preciseClipping;
	size_t minimizerLength;
	size_t minimizerWindowSize;
	double minimizerSeedDensity;
	size_t seedClusterMinSize;
	double minimizerDiscardMostNumerousFraction;
	double seedExtendDensity;
	bool nondeterministicOptimizations;
	bool optimalDijkstra;
	double preciseClippingIdentityCutoff;
	int Xdropcutoff;
	size_t DPRestartStride;
	bool cigarMatchMismatchMerge;

	bool colinearChaining;
	bool symmetricColinearChaining;
	bool generatePath;
	bool graphStatistics;
	long long generatePathSeed;
	long long colinearGap;
	long long colinearSplitLen;
	long long colinearSplitGap;
	double samplingStep;
	bool fastMode;
	
};

void alignReads(AlignerParams params);
void replaceDigraphNodeIdsWithOriginalNodeIds(vg::Alignment& alignment, const AlignmentGraph& graph);

#endif
