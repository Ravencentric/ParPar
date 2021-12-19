#include <iostream>
#include <stdint.h>
#include <stdlib.h> // free / calloc
#include "gf16ocl.h"

// for benchmarking purposes - skip transferring anything to/from GPU
//#define SKIP_TRANSFER


std::vector<cl::Platform> GF16OCL::platforms;

// buffer for zeroing GPU memory
#define ZERO_MEM_SIZE 65536
#include "gfmat_coeff.h"

int GF16OCL::load_runtime() {
	if(load_opencl()) {
		return 1;
	}
	try {
		cl::Platform::get(&platforms);
		if(platforms.size() == 0) return 2;
	} catch(cl::Error const&) {
		return 2;
	}
	return 0;
}

GF16OCL::GF16OCL(int platformId, int deviceId) {
	_initSuccess = false;
	zeroes = NULL;
	
	cl::Platform platform;
	if(platformId == -1)
		platform = cl::Platform::getDefault();
	else {
		if(platformId >= (int)platforms.size()) return;
		platform = platforms[platformId];
	}
	
	std::vector<cl::Device> devices;
	if(deviceId == -1) {
		// first try GPU
		try {
			platform.getDevices(CL_DEVICE_TYPE_GPU, &devices);
		} catch(cl::Error const&) {}
		// if no GPU device found, try anything else
		if(devices.size() < 1) {
			try {
				platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);
			} catch(cl::Error const&) {}
		}
		if(devices.size() < 1) return; // no devices!
		// match first device that's available
		bool found = false;
		for(unsigned i=0; i<devices.size(); i++) {
			device = devices[i];
			if(device.getInfo<CL_DEVICE_AVAILABLE>() && device.getInfo<CL_DEVICE_COMPILER_AVAILABLE>() && device.getInfo<CL_DEVICE_ENDIAN_LITTLE>()) {
				found = true;
				break;
			}
		}
		if(!found) return;
	}
	else {
		platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);
		if(deviceId >= (int)devices.size()) return;
		device = devices[deviceId];
	}
	
	context = cl::Context(device);
	
	// we only support little-endian hosts and devices
#if !defined(_MSC_VER) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return;
#endif
	if(device.getInfo<CL_DEVICE_ENDIAN_LITTLE>() != CL_TRUE) return;
	
	try {
		queue = cl::CommandQueue(context, device, 0);
		_initSuccess = true;
	} catch(cl::Error const& err) {
		std::cerr << "OpenCL Error: " << err.what() << "(" << err.err() << ")" << std::endl;
	}
	
	queueEvents.reserve(2);
}

GF16OCL::~GF16OCL() {
	free(zeroes);
}


// based off getWGsizes function from clinfo
size_t GF16OCL::getWGSize(const cl::Context& context, const cl::Device& device) {
	try {
		cl::Program program(context,
			"#define GWO(type) global type* restrict\n"
			"#define GRO(type) global const type* restrict\n"
			"#define BODY int i = get_global_id(0); out[i] = in1[i] + in2[i]\n"
			"#define _KRN(T, N) kernel void sum##N(GWO(T##N) out, GRO(T##N) in1, GRO(T##N) in2) { BODY; }\n"
			"#define KRN(N) _KRN(int, N)\n"
			"KRN()\n/* KRN(2)\nKRN(4)\nKRN(8)\nKRN(16) */\n", true);
		cl::Kernel kern(program, "sum");
		return kern.getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(device);
	} catch(cl::Error const&) {
		return 0;
	}
}

// _sliceSize must be divisible by 2
bool GF16OCL::init(size_t _sliceSize, Galois16OCLMethods method, unsigned targetInputBatch, unsigned targetIters, unsigned targetGrouping) {
	if(!_initSuccess) return false;
	sliceSize = _sliceSize;
	numOutputs = 0;
	outputExponents.clear();
	
	if(method == GF16OCL_AUTO) method = default_method();
	_setupMethod = method;
	_setupTargetInputBatch = targetInputBatch;
	_setupTargetIters = targetIters;
	_setupTargetGrouping = targetGrouping;
	
	const std::string oclVersion = device.getInfo<CL_DEVICE_VERSION>(); // will return "OpenCL x.y [extra]"
	if(oclVersion[7] == '1' && oclVersion[8] == '.' && oclVersion[9] < '2') { // OpenCL < 1.2
		free(zeroes);
		zeroes = (uint8_t*)calloc(ZERO_MEM_SIZE, 1);
	}
	reset_state();
	coeffType = GF16OCL_COEFF_NORMAL;
	return true;
}

bool GF16OCL::set_outputs(unsigned _numOutputs, const uint16_t* outputExp) {
	// check if coeffs are sequential
	bool coeffIsSeq = false;
	if(coeffType != GF16OCL_COEFF_LOG) {
		coeffIsSeq = true;
		uint16_t coeffBase = outputExp[0]; // we don't support _numOutputs < 1
		for(unsigned i=1; i<_numOutputs; i++) {
			if(outputExp[i] != coeffBase+i) {
				coeffIsSeq = false;
				break;
			}
		}
	}
	
	if(_numOutputs != numOutputs || (coeffType == GF16OCL_COEFF_LOG_SEQ && !coeffIsSeq)) {
		// need to re-init as the code assumes a fixed number of outputs
		numOutputs = _numOutputs;
		outputExponents = std::vector<uint16_t>(numOutputs);
		if(!setup_kernels(_setupMethod, _setupTargetInputBatch, _setupTargetIters, _setupTargetGrouping, coeffIsSeq))
			return false;
	}
	
	memcpy(outputExponents.data(), outputExp, numOutputs*sizeof(uint16_t));
	if(coeffType == GF16OCL_COEFF_LOG) {
		cl::Event writeEvent;
		queue.enqueueWriteBuffer(buffer_outExp, CL_FALSE, 0, numOutputs*sizeof(uint16_t), outputExponents.data(), &queueEvents, &writeEvent);
		queueEvents.clear();
		queueEvents.reserve(2);
		queueEvents.push_back(writeEvent);
	} else if(coeffType == GF16OCL_COEFF_LOG_SEQ) {
		kernelMul.setArg<cl_uint>(3, outputExponents[0]);
		kernelMulAdd.setArg<cl_uint>(3, outputExponents[0]);
		kernelMulLast.setArg<cl_uint>(4, outputExponents[0]);
		kernelMulAddLast.setArg<cl_uint>(4, outputExponents[0]);
	}
	return true;
}

void GF16OCL::reset_state() {
	inputCount = 0;
	inputBufferIdx = 0;
	for(int i=0; i<OCL_BUFFER_COUNT; i++) {
		proc[i] = cl::Event(); // clear any existing event
	}
	doAdd = false;
}

std::vector<Galois16OCLMethods> GF16OCL::availableMethods(int platformId, int deviceId) {
	std::vector<Galois16OCLMethods> ret;
	if(platformId == -1 || deviceId == -1) {
		// we assume all methods are available
		ret.push_back(GF16OCL_LOOKUP);
		ret.push_back(GF16OCL_LOOKUP_HALF);
		ret.push_back(GF16OCL_LOOKUP_NOCACHE);
		ret.push_back(GF16OCL_LOOKUP_HALF_NOCACHE);
		ret.push_back(GF16OCL_LOG);
		ret.push_back(GF16OCL_LOG_SMALL);
		ret.push_back(GF16OCL_LOG_TINY);
		ret.push_back(GF16OCL_LOG_SMALL_LMEM);
		ret.push_back(GF16OCL_LOG_TINY_LMEM);
		//ret.push_back(GF16OCL_SHUFFLE);
		ret.push_back(GF16OCL_BY2);
	} else {
		if(platformId >= (int)platforms.size()) return ret;
		
		std::vector<cl::Device> devices;
		platforms[platformId].getDevices(CL_DEVICE_TYPE_ALL, &devices);
		if(deviceId >= (int)devices.size()) return ret;
		
		cl::Context context(devices[deviceId]);
		// TODO: actually check capabilities
		ret.push_back(GF16OCL_LOOKUP);
		ret.push_back(GF16OCL_LOOKUP_HALF);
		ret.push_back(GF16OCL_LOOKUP_NOCACHE);
		ret.push_back(GF16OCL_LOOKUP_HALF_NOCACHE);
		ret.push_back(GF16OCL_LOG);
		ret.push_back(GF16OCL_LOG_SMALL);
		ret.push_back(GF16OCL_LOG_TINY);
		ret.push_back(GF16OCL_LOG_SMALL_LMEM);
		ret.push_back(GF16OCL_LOG_TINY_LMEM);
		ret.push_back(GF16OCL_BY2);
	}
	
	return ret;
}
Galois16OCLMethods GF16OCL::default_method() const {
	// TODO: determine best method
	return GF16OCL_LOG_SMALL;
}

// reduce slice size
void GF16OCL::set_slice_size(size_t newSliceSize) {
	if(newSliceSize > allocatedSliceSize) return; // only reduction in size allowed
	size_t sliceGroups = (newSliceSize+bytesPerGroup -1) / bytesPerGroup;
	processRange = cl::NDRange(sliceGroups * wgSize, processRange[1]);
	sliceSize = newSliceSize;
	sliceSizeAligned = sliceGroups*bytesPerGroup;
}

void GF16OCL::run_kernel(unsigned buf, unsigned numInputs) {
	// transfer coefficient list
	cl::Event coeffEvent;
	queue.enqueueWriteBuffer(buffer_coeffs[buf], CL_FALSE, 0, inputBatchSize * (coeffType!=GF16OCL_COEFF_NORMAL ? 1 : outputExponents.size()) * sizeof(uint16_t), tmp_coeffs[buf].data(), NULL, &coeffEvent);
	queueEvents.push_back(coeffEvent);
	
	// invoke kernel
	cl::Kernel* kernel;
	if(numInputs == inputBatchSize) {
		kernel = doAdd ? &kernelMulAdd : &kernelMul;
	} else { // last round
		kernel = doAdd ? &kernelMulAddLast : &kernelMulLast;
		kernel->setArg<cl_ushort>(3, numInputs);
	}
	
	doAdd = true;
	kernel->setArg(1, buffer_input[buf]);
	kernel->setArg(2, buffer_coeffs[buf]);
	// TODO: try-catch to detect errors (e.g. out of resources)
	queue.enqueueNDRangeKernel(*kernel, cl::NullRange, processRange, workGroupRange, &queueEvents, proc+buf);
	queueEvents.clear(); // for coeffType!=GF16OCL_COEFF_NORMAL, assume that the outputs are transferred by the time we enqueue the next kernel
	
	// management
	inputCount = 0;
	inputBufferIdx = (inputBufferIdx+1) % OCL_BUFFER_COUNT;
	
	// wait for the next kernel to finish
	const cl::Event& next = proc[inputBufferIdx];
	if(next()) next.wait();
}

void GF16OCL::_add_input(const void* data, size_t size) {
#ifndef SKIP_TRANSFER
	queue.enqueueWriteBuffer(buffer_input[inputBufferIdx], CL_TRUE, inputCount * sliceSizeAligned, size, data);
	if(size < sliceSize) {
		// need to zero rest of buffer
		size_t zeroStart = inputCount * sliceSizeAligned + size;
		size_t zeroAmt = sliceSize - size;
		cl::Event zeroEvent;
		if(zeroes) { // OpenCL 1.1
			queueEvents.reserve(queueEvents.capacity() + (zeroAmt+ZERO_MEM_SIZE-1)/ZERO_MEM_SIZE);
			while(1) {
				if(zeroAmt > ZERO_MEM_SIZE) {
					queue.enqueueWriteBuffer(buffer_input[inputBufferIdx], CL_FALSE, zeroStart, ZERO_MEM_SIZE, zeroes, NULL, &zeroEvent);
					queueEvents.push_back(zeroEvent);
					zeroAmt -= ZERO_MEM_SIZE;
					zeroStart += ZERO_MEM_SIZE;
				} else {
					if(zeroAmt) {
						queue.enqueueWriteBuffer(buffer_input[inputBufferIdx], CL_FALSE, zeroStart, zeroAmt, zeroes, NULL, &zeroEvent);
						queueEvents.push_back(zeroEvent);
					}
					break;
				}
			}
		} else { // OpenCL 1.2
			queue.enqueueFillBuffer<uint8_t>(buffer_input[inputBufferIdx], 0, zeroStart, zeroAmt, NULL, &zeroEvent);
			queueEvents.push_back(zeroEvent);
		}
	}
#else
	(void)data; (void)size;
#endif
	if(inputCount == inputBatchSize-1)
		run_kernel(inputBufferIdx, inputBatchSize);
	else
		inputCount++;
}

void GF16OCL::add_input(const void* data, size_t size, unsigned inputNum) {
	// compute coeffs
	if(coeffType != GF16OCL_COEFF_NORMAL) {
		tmp_coeffs[inputBufferIdx][inputCount] = gfmat_input_log(inputNum);
	} else {
		uint16_t inputLog = gfmat_input_log(inputNum);
		for(unsigned i=0; i<numOutputs; i++)
			tmp_coeffs[inputBufferIdx][inputCount + i*inputBatchSize] = gfmat_coeff_from_log(inputLog, outputExponents[i]);
	}
	_add_input(data, size);
}
void GF16OCL::add_input(const void* data, size_t size, const uint16_t* coeffs) {
	for(unsigned i=0; i<numOutputs; i++)
		tmp_coeffs[inputBufferIdx][inputCount + i*inputBatchSize] = coeffs[i];
	_add_input(data, size);
}

void GF16OCL::flush_inputs() {
	if(inputCount)
		run_kernel(inputBufferIdx, inputCount);
}

void GF16OCL::finish() {
	//if(inputCount) // discard any unflushed inputs for now
	//	flush_inputs();
	// wait for all processing to complete
	try {
		for(unsigned i=0; i<OCL_BUFFER_COUNT; i++) {
			if(proc[i]())
				proc[i].wait();
		}
	} catch(cl::Error const& err) {
		std::cerr << "OpenCL error during finish: " << err.what() << "(" << err.err() << ")" << std::endl;
	}
	
	queue.finish(); // TODO: do we need the above waits if finish does everything?
	reset_state();
}

void GF16OCL::get_output(unsigned index, void* output) const {
#ifndef SKIP_TRANSFER
	queue.enqueueReadBuffer(buffer_output, CL_TRUE, index*sliceSizeAligned, sliceSize, output);
#else
	(void)index; (void)output;
#endif
}


std::vector<std::string> GF16OCL::getPlatforms() {
	std::vector<std::string> ret(platforms.size());
	for(unsigned i=0; i<platforms.size(); i++) {
		ret[i] = platforms[i].getInfo<CL_PLATFORM_NAME>();
	}
	return ret;
}

std::vector<GF16OCL_DeviceInfo> GF16OCL::getDevices(int platformId) {
	cl::Platform platform;
	if(platformId == -1)
		platform = cl::Platform::getDefault();
	else {
		if(platformId >= (int)platforms.size()) return std::vector<GF16OCL_DeviceInfo>();
		platform = platforms[platformId];
	}
	
	std::vector<cl::Device> devices;
	platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);

	std::vector<GF16OCL_DeviceInfo> ret(devices.size());
	for(unsigned i=0; i<devices.size(); i++) {
		GF16OCL_DeviceInfo info;
		const cl::Device& device = devices[i];
		info.name = device.getInfo<CL_DEVICE_NAME>();
		info.vendorId = device.getInfo<CL_DEVICE_VENDOR_ID>();
		info.type = device.getInfo<CL_DEVICE_TYPE>();
		info.available = device.getInfo<CL_DEVICE_AVAILABLE>() && device.getInfo<CL_DEVICE_COMPILER_AVAILABLE>();
		info.supported = info.available && device.getInfo<CL_DEVICE_ENDIAN_LITTLE>();
		info.memory = device.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>();
		info.maxWorkGroup = (unsigned)device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
		info.computeUnits = device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
		info.globalCache = device.getInfo<CL_DEVICE_GLOBAL_MEM_CACHE_SIZE>();
		info.constantMemory = device.getInfo<CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE>();
		info.localMemory = device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>();
		info.localMemoryIsGlobal = device.getInfo<CL_DEVICE_LOCAL_MEM_TYPE>() == CL_GLOBAL;
		info.maxAllocation = device.getInfo<CL_DEVICE_MAX_MEM_ALLOC_SIZE>();
		info.unifiedMemory = device.getInfo<CL_DEVICE_HOST_UNIFIED_MEMORY>();
		info.workGroupMultiple = (unsigned)getWGSize(device, device);
		ret[i] = info;
	}
	return ret;
}
