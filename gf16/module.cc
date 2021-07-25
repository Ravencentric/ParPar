#ifdef _OPENMP
#include <omp.h>
#endif
#include "../src/stdint.h"
#include <string.h>
#include <stdlib.h>
#include "gf16mul.h"

#define CACHELINE_SIZE 64

#include "gfmat_coeff.h"
void ppgf_init_constants() {
	gfmat_init();
}

static Galois16Mul* gf = NULL;
static std::vector<void*> gfScratch;

static int maxNumThreads = 1, defaultNumThreads = 1;

static void setup_gf(Galois16Methods method = GF16_AUTO, size_t size_hint = 0) {
	if(!gfScratch.empty()) {
		for(unsigned i=0; i<gfScratch.size(); i++)
			if(gfScratch[i])
				gf->mutScratch_free(gfScratch[i]);
		gfScratch.clear();
	}
	delete gf;
	gf = new Galois16Mul(method);
	
	gfScratch.reserve(maxNumThreads);
	for(int i=0; i<maxNumThreads; i++)
		gfScratch.push_back(gf->mutScratch_alloc());
}
void ppgf_maybe_setup_gf() {
	if(!gf) setup_gf();
}


#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CEIL_DIV(a, b) (((a) + (b)-1) / (b))
#define ROUND_DIV(a, b) (((a) + ((b)>>1)) / (b))

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
	#include <stdlib.h> // MSVC ARM64 seems to need this
	#define ALIGN_ALLOC(buf, len, align) *(void**)&(buf) = _aligned_malloc((len), align)
	#define ALIGN_FREE _aligned_free
#elif defined(__cplusplus) && __cplusplus >= 201100 && !(defined(_MSC_VER) && (defined(__clang__) || defined(_M_ARM64) || defined(_M_ARM))) && !defined(__APPLE__)
	// C++11 method
	// len needs to be a multiple of alignment, although it sometimes works if it isn't...
	#include <cstdlib>
	#define ALIGN_ALLOC(buf, len, align) *(void**)&(buf) = aligned_alloc(align, ((len) + (align)-1) & ~((align)-1))
	#define ALIGN_FREE free
#else
	#include <stdlib.h>
	#define ALIGN_ALLOC(buf, len, align) if(posix_memalign((void**)&(buf), align, (len))) (buf) = NULL
	#define ALIGN_FREE free
#endif

// performs multiple multiplies for a region, using threads
// note that inputs will get trashed
/* REQUIRES:
   - input and each pointer in outputs must be aligned
   - len must be a multiple of stride
   - input and length of each output is the same and == len
   - number of outputs and scales is same and == numOutputs
*/
void ppgf_multiply_mat(const void* const* inputs, uint_fast16_t* iNums, unsigned int numInputs, size_t len, void** outputs, uint_fast16_t* oNums, unsigned int numOutputs, int add) {
	// pre-calc all coefficients
	// calculation does lookups, so faster to do it first and avoid memory penalties later on
	uint16_t* factors = new uint16_t[numInputs * numOutputs];
	for(unsigned out=0; out<numOutputs; out++) {
		if(!oNums[out]) continue; // 0th powers are never used, so skip these
		for(unsigned inp=0; inp<numInputs; inp++) {
			factors[inp + out*numInputs] = gfmat_coeff(iNums[inp], oNums[out]);
		}
	}
	
	
	// break the slice into smaller chunks so that we maximise CPU cache usage
	int numChunks = ROUND_DIV(len, gf->info().idealChunkSize);
	if(numChunks < 1) numChunks = 1;
	size_t chunkSize = gf->alignToStride(CEIL_DIV(len, numChunks)); // we'll assume that input chunks are memory aligned here
	
	// fix up numChunks with actual number (since it may have changed from above)
	numChunks = CEIL_DIV(len, chunkSize);
	
	// avoid nested loop issues by combining chunk & output loop into one
	// the loop goes through outputs before chunks
	int loop = 0;
	#pragma omp parallel for num_threads(maxNumThreads)
	for(loop = 0; loop < (int)(numOutputs * numChunks); loop++) {
		size_t offset = (loop / numOutputs) * chunkSize;
		unsigned int out = loop % numOutputs;
		int procSize = MIN(len-offset, chunkSize);
#ifdef _OPENMP
		int threadNum = omp_get_thread_num();
#else
		const int threadNum = 0;
#endif

		if(!add) memset(((uint8_t*)outputs[out])+offset, 0, procSize);
		if(oNums[out])
			gf->mul_add_multi(numInputs, offset, outputs[out], inputs, procSize, factors + out*numInputs, gfScratch[threadNum]);
		else // pow(x, 0) == 1, so everything will be an add
			gf->add_multi(numInputs, offset, outputs[out], inputs, procSize);
	}
	
	delete[] factors;
}


void ppgf_prep_input(size_t destLen, size_t inputLen, char* dest, char* src) {
	ppgf_maybe_setup_gf();
	if(inputLen < destLen) {
		// need to zero out empty space at end (for final block)
		memset(dest + inputLen, 0, destLen - inputLen);
	}
	gf->prepare(dest, src, inputLen);
}
void ppgf_finish_input(unsigned int numInputs, uint16_t** inputs, size_t len) {
	ppgf_maybe_setup_gf();
	if(gf->needPrepare()) {
		// TODO: multi-thread this?
		for(int in = 0; in < (int)numInputs; in++)
			gf->finish(inputs[in], len);
	}
}

void ppgf_get_method(int* rMethod, const char** rMethLong, int* align, int* stride) {
	ppgf_maybe_setup_gf();
	const Galois16MethodInfo& info = gf->info();
	*rMethod = info.id;
	*rMethLong = info.name;
	*align = info.alignment;
	*stride = info.stride;
}

int ppgf_get_num_threads() {
#ifdef _OPENMP
	return maxNumThreads;
#else
	return 1;
#endif
}
void ppgf_set_num_threads(int threads) {
#ifdef _OPENMP
	maxNumThreads = threads;
	if(maxNumThreads < 1) maxNumThreads = defaultNumThreads;
	if(gf && (unsigned)maxNumThreads > gfScratch.size()) {
		gfScratch.reserve(maxNumThreads);
		for(unsigned i=gfScratch.size(); i<(unsigned)maxNumThreads; i++)
			gfScratch.push_back(gf->mutScratch_alloc());
	}
#endif
}
void ppgf_init_gf_module() {
#ifdef _OPENMP
	maxNumThreads = omp_get_num_procs();
	if(maxNumThreads < 1) maxNumThreads = 1;
	defaultNumThreads = maxNumThreads;
#endif
}

int ppgf_set_method(int meth, int size_hint) {
	setup_gf((Galois16Methods)meth, size_hint);
	return 0;
}
