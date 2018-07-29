#include <sys/file.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "misc.h"
#include "tinygraph.h"
#include "graph.h"
#include "blant.h"
#include "sets.h"
#include "matvec.h" // for VecDiff, etc.

char USAGE[] = "synthetic -k k Gtarget.el Gsynth.el blant.Gt.index blant.Gs.index\n - output is new Gsynth.el\n";

Boolean _supportNodeNames = false;

// The following is the most compact way to store the permutation between a non-canonical and its canonical representative,
// when k=8: there are 8 entries, and each entry is a integer from 0 to 7, which requires 3 bits. 8*3=24 bits total.
// For simplicity we use the same 3 bits per entry, and assume 8 entries, even for k<8.  It wastes memory for k<4, but
// makes the coding much simpler.
typedef unsigned char kperm[3]; // The 24 bits are stored in 3 unsigned chars.

static unsigned int _Bk, _k; // _k is the global variable storing k; _Bk=actual number of entries in the canon_map for given k.
static int _numCanon, _canonList[MAX_CANONICALS];
static int _numOrbits, _orbitList[MAX_CANONICALS][maxK], _numSamples;

// Here's where we're lazy on saving memory, and we could do better.  We're going to allocate a static array
// that is big enough for the 256 million permutations from non-canonicals to canonicals for k=8, even if k<8.
// So we're allocating 256MBx3=768MB even if we need much less.  I figure anything less than 1GB isn't a big deal
// these days. It needs to be aligned to a page boundary since we're going to mmap the binary file into this array.
static kperm Permutations[maxBk] __attribute__ ((aligned (8192)));
// Here's the actual mapping from non-canonical to canonical, same argument as above wasting memory, and also mmap'd.
// So here we are allocating 256MB x sizeof(short int) = 512MB.
// Grand total statically allocated memory is exactly 1.25GB.
static short int _K[maxBk] __attribute__ ((aligned (8192)));


// Assuming the global variable _k is set properly, go read in and/or mmap the big global
// arrays related to canonical mappings and permutations.
void SetGlobalCanonMaps(void)
{
    assert(3 <= _k && _k <= 8);
    _Bk = (1 <<(_k*(_k-1)/2));
    char BUF[BUFSIZ];
    int i;
    _numCanon = canonListPopulate(BUF, _canonList, _k);
    _numOrbits = orbitListPopulate(BUF, _orbitList, _k);
    mapCanonMap(BUF, _K, _k);
    sprintf(BUF, CANON_DIR "/perm_map%d.bin", _k);
    int pfd = open(BUF, 0*O_RDONLY);
    kperm *Pf = Mmap(Permutations, _Bk*sizeof(Permutations[0]), pfd);
    assert(Pf == Permutations);
}

// Given the big graph G and a set of nodes in V, return the TINY_GRAPH created from the induced subgraph of V on G.
static TINY_GRAPH *TinyGraphInducedFromGraph(TINY_GRAPH *Gv, GRAPH *G, int *Varray)
{
    unsigned i, j;
    TinyGraphEdgesAllDelete(Gv);
    for(i=0; i < Gv->n; i++) for(j=i+1; j < Gv->n; j++)
        if(GraphAreConnected(G, Varray[i], Varray[j]))
            TinyGraphConnect(Gv, i, j);
    return Gv;
}

void ReBLANT(double *D, GRAPH *G, SET **samples, int **B, int v1, int v2)
{
    int line;
    static TINY_GRAPH *g;
    static int Varray[maxK];
    if(!g) g = TinyGraphAlloc(_k);
    
    for(line = 0; line < _numSamples; line++) if(SetIn(samples[v1], line) && SetIn(samples[v2], line))
    {
	--D[B[line][0]];
	TinyGraphInducedFromGraph(g, G, B[line]+1);
	B[line][0] = _K[TinyGraph2Int(g, _k)];
	++D[B[line][0]];
    }
}

int main(int argc, char *argv[])
{
    int i, opt, j, line;

    if(argc == 1)
    {
	fprintf(stderr, "%s\n", USAGE);
	exit(1);
    }

    _k = 0;

    while((opt = getopt(argc, argv, "k:")) != -1)
    {
	switch(opt)
	{
	case 'k': _k = atoi(optarg);
	    if(!(3 <= _k && _k <= 8)) Fatal("k must be between 3 and 8\n%s", USAGE);
	    break;
	default: Fatal("unknown option %c\n%s", opt, USAGE);
	}
    }
    SetGlobalCanonMaps(); // needs _k to be set

    GRAPH *G[2]; // G[0] is the target, G[1] is the synthetic.
    for(i=0; i<2; i++)
    {
	if(!argv[optind]) Fatal("no input graph file specified\n%s", USAGE);
	FILE *fpGraph = fopen(argv[optind], "r");
	if(!fpGraph) Fatal("cannot open graph input file '%s'\n", argv[optind]);
	// Read it in using native Graph routine.
	G[i] = GraphReadEdgeList(fpGraph, true, _supportNodeNames); // sparse=true
	if(_supportNodeNames)
	    assert(G[i]->name);
	fclose(fpGraph);
	optind++;
    }

    // The distribution of graphlets (squiggly plot vectors)
    // NOTE we use DOUBLE not INT so we can use VecDiff from my libwayne/matvec
    double D[2][_numCanon], vecDiff[_numCanon];
    for(i=0; i<_numCanon; i++) D[0][i] = D[1][i] = 0;

    int nSamples[2], **BLANT[2];
    for(i=0;i<2;i++)
    {
	char cmd[BUFSIZ];
	// Get number of samples
	sprintf(cmd, "wc -l < %s", argv[optind]);
	FILE *fp = popen(cmd, "r");
	assert(fp);
	fscanf(fp, "%d", &nSamples[i]);
	assert(nSamples[i] > 0);
	pclose(fp);
	fp=fopen(argv[optind], "r");
	assert(fp);
	BLANT[i] = (int**)Malloc(nSamples[i] * sizeof(int*));
	for(line=0; line<nSamples[i]; line++)
	{
	    BLANT[i][line] = (int*)Malloc((_k+1)*sizeof(int));
	    for(j=0; j<_k+1; j++)
		assert(1 == fscanf(fp, "%d", &(BLANT[i][line][j])));
	    ++D[i][BLANT[i][line][0]]; // squiggly plot
	}
	assert(fscanf(fp, "%d", &line) < 1); // ensure there's nothing left to read.
	fclose(fp);
	optind++;
    }
    assert(nSamples[0] == nSamples[1]);
    _numSamples = nSamples[0];
    for(i=0;i<2;i++)
    {
	int testCount = 0;
	for(j=0; j<_numCanon; j++) testCount += D[i][j];
	assert(testCount == _numSamples);
    }

    SET **samples = (SET**)Malloc(G[1]->n * sizeof(SET*));
    for(i=0; i < G[1]->n; i++)
	samples[i] = SetAlloc(_numSamples);
    for(line = 0; line < _numSamples; line++)
	for(j=1; j<=_k; j++)
	    SetAdd(samples[BLANT[1][line][j]], line);

    // while(not done---either some number of iterations, or objective function says we're too far away)
    while(VecLength(_numCanon, VecDiff(_numCanon, vecDiff, D[0], D[1])) > 0.1)
    {
	putc('.', stderr); fflush(stderr);
	int edge = drand48() * G[1]->numEdges;
	int u1, u2, *pV1 = &(G[1]->edgeList[2*edge]), *pV2 = &(G[1]->edgeList[2*edge+1]);
	do {
	    u1 = drand48()*G[1]->n;
	    do u2 = drand48()*G[1]->n; while(u1==u2);
	} while(GraphAreConnected(G[1], u1, u2)); // find a non-edge
	assert(GraphAreConnected(G[1], *pV1, *pV2));
	GraphDisconnect(G[1], *pV1, *pV2); // remove edge e from Gs
	ReBLANT(D[1], G[1], samples, BLANT[1], *pV1, *pV2);
	GraphConnect(G[1], u1, u2);
	ReBLANT(D[1], G[1], samples, BLANT[1], u1, u2);

	double newDiff[_numCanon];
	// note we should do this "efficiently" using a delta rather than recomputing whole objective
	VecDiff(_numCanon, newDiff, D[0], D[1]);
	// Hill Climbing for now
	if(VecLength(_numCanon, newDiff) < VecLength(_numCanon, vecDiff))
	{
	    // *pV1 = MIN(u1,u2);
	    // *pV2 = MAX(u1,u2);
	}
	else // revert
	{
	    GraphDisconnect(G[1], u1, u2);
	    ReBLANT(D[1], G[1], samples, BLANT[1], u1, u2);
	    GraphConnect(G[1], *pV1, *pV2);
	    ReBLANT(D[1], G[1], samples, BLANT[1], *pV1, *pV2);
	}
    }
    for(i=0; i < G[1]->numEdges; i++)
	printf("%d %d\n", G[1]->edgeList[2*i], G[1]->edgeList[2*i+1]);
}
