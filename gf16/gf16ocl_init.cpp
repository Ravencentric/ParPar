#include <iostream>
#include <stdio.h> // snprintf
#include <stdint.h>
#include <string.h> // strstr
#include <sstream> // std::stringstream
#include "gf16ocl.h"
#include "gf16_global.h" // GF16_POLYNOMIAL

// for viewing compiled code, uncomment
//#define DUMP_ASM

// uncomment to prefer using the workgroup multiple instead of the max workgroup size
// it seems like using the max width on GCN is usually ideal, but on Fermi and older, width is too wide that it hurts perf
//#define OCL_PREFER_WORKGROUP_MULTIPLE 8


#ifdef DUMP_ASM
# include <fstream> // std::ofstream
#endif

#define STRINGIFY(...) #__VA_ARGS__
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
const static char _ocl_defines[] =
"#define GF16_POLYNOMIAL " STR(GF16_POLYNOMIAL) "\n"
"#define SHIFT_TOP_BIT(x) ((x) >> (sizeof(x)*8-1))\n" // shift down top bit to bottom
// can use 24-bit multiply if the slice size fits within 24 bits (rownum guaranteed to be within 16 bits)
// TODO: may be possible to do this is if stride is less than 16M *elements*, but we don't have byte addressing, so may not be possible
"#if MAX_SLICE_SIZE < 16777216\n"
" #define RBUF_REF(row, stride, col) mad24((uint)(row), (uint)(stride), (uint)(col))\n"
"#else\n"
" #define RBUF_REF(row, stride, col) ((row) * (stride) + (col))\n"
"#endif\n"
"#if MAX_SLICE_SIZE < 16777216 && MAX_SLICE_SIZE*NUM_OUTPUTS <= 0xffffffffUL\n"
" #define WBUF_REF(row, stride, col) mad24((uint)(row), (uint)(stride), (uint)(col))\n"
"#else\n"
" #define WBUF_REF(row, stride, col) ((row) * (stride) + (col))\n"
"#endif\n"
"#define COBUF_REF(row, col) ((row) * SUBMIT_INPUTS + (col))\n"
// if we're dealing with < 4GB at a time, there's no need for 64-bit memory references
"#if MAX_SLICE_SIZE*SUBMIT_INPUTS > 0xffffffffUL || MAX_SLICE_SIZE*NUM_OUTPUTS > 0xffffffffUL\n"
" typedef size_t memsize_t;\n"
"#else\n"
// TODO: check - size_t seems to faster than uint?
//" typedef uint memsize_t;\n"
" typedef size_t memsize_t;\n"
"#endif\n"
"#if VECT_WIDTH == 1\n"
" #define VECT_ONE 0x0001u\n"
" #define nat_ucharX uchar2\n"
" #define nat_ushort4 ushort4\n"
" #define nat_uint ushort\n"
" #define nat_int short\n"
" #define NAT_BITS 16\n"
"#elif VECT_WIDTH == 2\n"
" #define VECT_ONE 0x00010001ul\n"
" #define nat_ucharX uchar4\n"
" #define nat_ushort4 uint2\n"
" #define nat_uint uint\n"
" #define nat_int int\n"
" #define NAT_BITS 32\n"
"#elif VECT_WIDTH == 4\n"
" #define VECT_ONE 0x0001000100010001ull\n"
" #define nat_ucharX uchar8\n"
" #define nat_ushort4 ulong\n"
" #define nat_uint ulong\n"
" #define nat_int long\n"
" #define NAT_BITS 64\n"
"#else\n"
" #error Unsupported vector width\n"
"#endif\n"

"#ifndef EX_TABLE_ARGS\n"
" #define EX_TABLE_ARGS\n"
" #define EX_TABLE_ARGS_DECL\n"
"#endif\n"
"#ifndef EX_TABLE_KARGS_DECL\n"
" #define EX_TABLE_KARGS_DECL EX_TABLE_ARGS_DECL\n"
"#endif\n"
// experimental flag which probably isn't worth it - only usable in LOG methods, which doesn't have many coeffs to look up
//"#define COPY_COEFF\n" // only used for `nolut` kernel: copies coefficients from global memory to local before using
"#ifdef COPY_COEFF\n"
" #define COEFF_TABLE_TYPE __local\n"
"#else\n"
" #define COEFF_TABLE_TYPE __global\n"
"#endif\n"

"#define val_t nat_uint\n"
// TODO: explore using uint4 for reads to maximise bandwidth
"#define READ_SRC(src, idx) src[idx]\n";

const static char _ocl_method_by2[] =
"#define GF_MULTIPLY gf16_multiply_by2\n" STRINGIFY(
	
	// `a` is a packed ushort, `b` is just a ushort
	nat_uint gf16_multiply_by2(nat_uint a, nat_uint b) {
		nat_int btmp = b << (NAT_BITS - 16);
		nat_uint res = SHIFT_TOP_BIT(btmp) & a;
		) "\n#pragma unroll\n" STRINGIFY(
		for(int i=0; i<15; i++) {
			nat_uint poly = ((res >> 15) & VECT_ONE) * (GF16_POLYNOMIAL & 0xffff);
			res = ((res + res) & ~VECT_ONE) ^ poly;
			btmp <<= 1;
			res ^= SHIFT_TOP_BIT(btmp) & a;
		}
		return res;
	}
);
const static char _ocl_method_lh[] =
"#define LUT_REF(name, idx) ((__local ushort*)(name) + (idx)*512)\n"
"#define LUT_DECLARATION(name, size) __local ushort name[512 * (size)] __attribute__ ((aligned (VECT_WIDTH*2)))\n"
"#define LUT_GENERATE compute_lhtable\n"
"#define LUT_GENERATE_HAS_BARRIER\n"
"#define LUT_MULTIPLY lhtable_multiply\n"
STRINGIFY(
	ushort gf16_multiply_256(ushort v) {
		return (v << 8) ^ mul256poly[v >> 8];
	}
	
	// always writes 4 entries to table
	// requires val < 256 (used for lhtable calculation)
	void gf16_multiply_write_lh(__local nat_uint* table, nat_uint val, nat_uint coeff, nat_ushort4 coeff256) {
		// align a and b to the top so that the top-bit can be replicated with a single arithmetic right-shift
		nat_int a = val << (NAT_BITS - 8);
		nat_int b = coeff << (NAT_BITS - 16);
		nat_int prod = SHIFT_TOP_BIT(a) & b;
		) "\n#pragma unroll\n" STRINGIFY(
		for(int i=0; i<7; i++) {
			nat_uint poly = SHIFT_TOP_BIT(prod) & ((nat_uint)(GF16_POLYNOMIAL & 0xffff) << (NAT_BITS-16));
			prod = (prod << 1) ^ poly;
			a <<= 1;
			prod ^= SHIFT_TOP_BIT(a) & b;
		}
		
		// multiply b by 2 (aligned to top)
		nat_uint b2 = (b << 1) ^ (SHIFT_TOP_BIT(b) & ((nat_uint)(GF16_POLYNOMIAL & 0xffff) << (NAT_BITS-16)));
		// multiply prod by 256
		ushort prod256 = gf16_multiply_256((nat_uint)prod >> (NAT_BITS - 16));
		
		// write out to subsequent products
		) "\n#if VECT_WIDTH == 1\n" STRINGIFY(
		table[val/VECT_WIDTH] = prod;
		table[val/VECT_WIDTH + 1] = prod ^ b;
		table[val/VECT_WIDTH + 2] = prod ^ b2;
		table[val/VECT_WIDTH + 3] = prod ^ b2 ^ b;
		
		table[val/VECT_WIDTH + 256/VECT_WIDTH] = prod256;
		table[val/VECT_WIDTH + 256/VECT_WIDTH + 1] = prod256 ^ coeff256.s1;
		table[val/VECT_WIDTH + 256/VECT_WIDTH + 2] = prod256 ^ coeff256.s2;
		table[val/VECT_WIDTH + 256/VECT_WIDTH + 3] = prod256 ^ coeff256.s3;
		) "\n#elif VECT_WIDTH == 2\n" STRINGIFY(
		// add `b` to second element
		table[val/VECT_WIDTH] = (prod ^ b) | ((nat_uint)prod >> 16);
		table[val/VECT_WIDTH + 1] = (prod ^ b ^ b2) | (((nat_uint)prod ^ b2) >> 16);
		
		table[val/VECT_WIDTH + 256/VECT_WIDTH] = upsample(prod256, prod256) ^ coeff256.s0;
		table[val/VECT_WIDTH + 256/VECT_WIDTH + 1] = upsample(prod256, prod256) ^ coeff256.s1;
		) "\n#elif VECT_WIDTH == 4\n" STRINGIFY(
		nat_uint res = (prod ^ b2) | ((nat_uint)prod >> 32);
		table[val/VECT_WIDTH] = (res ^ b ^ ((nat_uint)b >> 32)) | (res >> 16);
		
		table[val/VECT_WIDTH + 256/VECT_WIDTH] = (prod256 * VECT_ONE) ^ coeff256;
		) "\n#endif\n" STRINGIFY(
	}
	
	nat_ushort4 lh_compute_coeff256(nat_uint coeff) {
		nat_ushort4 result;
		// compute coeff*0, coeff*256, coeff*512, coeff*768
		ushort coeff256 = gf16_multiply_256(coeff);
		ushort coeff512 = ((nat_uint)coeff256 << 1) ^ (-((nat_uint)coeff256 >> 15) & GF16_POLYNOMIAL);
		) "\n#if VECT_WIDTH == 1\n" STRINGIFY(
		//result[0] = 0; // unused
		result.s1 = coeff256;
		result.s2 = coeff512;
		result.s3 = coeff256 ^ coeff512;
		) "\n#elif VECT_WIDTH == 2\n" STRINGIFY(
		result.s0 = (uint)coeff256 << 16;
		result.s1 = upsample((ushort)(coeff256 ^ coeff512), coeff512);
		) "\n#elif VECT_WIDTH == 4\n" STRINGIFY(
		result = upsample(upsample((ushort)(coeff256 ^ coeff512), coeff512), (uint)coeff256 << 16);
		) "\n#endif\n" STRINGIFY(
		return result;
	}
	
	void compute_lhtable(__local ushort* table, __global const ushort* restrict coeffs, const ushort numCoeff) {
		barrier(CLK_LOCAL_MEM_FENCE);
		
		// assume workgroup size is a power of 2
		) "\n#if COL_GROUP_SIZE*4 > 256\n" STRINGIFY(
		// process multiple coefficients at a time
		for(nat_uint coeffBase=0; coeffBase<numCoeff; coeffBase+=(COL_GROUP_SIZE*4/256)) {
			const nat_uint coeffI = coeffBase + (get_local_id(0)*4)/256;
			if(coeffI < numCoeff) {
				const nat_uint coeff = coeffs[coeffI];
				gf16_multiply_write_lh(
					(__local nat_uint*)(table + coeffI*512),
					(get_local_id(0)*4) & 0xff,
					coeff,
					lh_compute_coeff256(coeff)
				);
			}
		}
		
		) "\n#else\n" STRINGIFY(
		// process one coefficient at a time
		
		for(nat_uint coeffI=0; coeffI<numCoeff; coeffI++) {
			const nat_uint coeff = coeffs[coeffI];
			const nat_ushort4 coeff256 = lh_compute_coeff256(coeff);
			__local nat_uint* nat_table = (__local nat_uint*)(table + coeffI*512);
			) "\n#pragma unroll\n" STRINGIFY(
			for(nat_uint valI=0; valI<256; valI+=COL_GROUP_SIZE*4) {
				gf16_multiply_write_lh(nat_table, valI + get_local_id(0)*4, coeff, coeff256);
			}
		}
		
		) "\n#endif\n" STRINGIFY(
		
		barrier(CLK_LOCAL_MEM_FENCE);
	}
	
	inline nat_uint lhtable_multiply(__local const ushort* table, nat_uint val) {
		) "\n#if VECT_WIDTH==2\n" STRINGIFY(
		return upsample(
			(ushort)(table[(val>>16) & 0xff] ^ table[((val>>24) & 0xff) + 256]),
			(ushort)(table[val & 0xff] ^ table[((val>>8) & 0xff) + 256])
		);
		) "\n#else\n" STRINGIFY(
		nat_uint result = table[val & 0xff] ^ table[((val>>8) & 0xff) + 256];
		) "\n#pragma unroll\n" STRINGIFY(
		for(nat_uint v=1; v<VECT_WIDTH; v++) {
			val >>= 16;
			result |= (nat_uint)(table[val & 0xff] ^ table[((val>>8) & 0xff) + 256]) << (v*16);
		}
		return result;
		) "\n#endif\n" STRINGIFY(
	}
);
const static char _ocl_method_ll[] =
"#define LUT_REF(name, idx) ((__local ushort*)(name) + (idx)*256)\n"
"#define LUT_DECLARATION(name, size) __local ushort name[256 * (size)] __attribute__ ((aligned (VECT_WIDTH*2)))\n"
"#define LUT_GENERATE compute_lltable\n"
"#define LUT_GENERATE_HAS_BARRIER\n"
"#define LUT_MULTIPLY lltable_multiply\n"
STRINGIFY(
	ushort gf16_multiply_256(ushort v) {
		return (v << 8) ^ mul256poly[v >> 8];
	}

	// as gf16_multiply_write_lh, but doesn't write 256x entries
	void gf16_multiply_write_ll(__local nat_uint* table, nat_uint val, nat_uint coeff) {
		// align a and b to the top so that the top-bit can be replicated with a single arithmetic right-shift
		nat_int a = val << (NAT_BITS - 8);
		nat_int b = coeff << (NAT_BITS - 16);
		nat_int prod = SHIFT_TOP_BIT(a) & b;
		) "\n#pragma unroll\n" STRINGIFY(
		for(int i=0; i<7; i++) {
			nat_uint poly = SHIFT_TOP_BIT(prod) & ((nat_uint)(GF16_POLYNOMIAL & 0xffff) << (NAT_BITS-16));
			prod = (prod << 1) ^ poly;
			a <<= 1;
			prod ^= SHIFT_TOP_BIT(a) & b;
		}
		
		// multiply by 2 (aligned to top)
		nat_uint b2 = (b << 1) ^ (SHIFT_TOP_BIT(b) & ((nat_uint)(GF16_POLYNOMIAL & 0xffff) << (NAT_BITS-16)));
		
		// write out to subsequent products
		) "\n#if VECT_WIDTH == 1\n" STRINGIFY(
		table[val/VECT_WIDTH] = prod;
		table[val/VECT_WIDTH + 1] = prod ^ b;
		table[val/VECT_WIDTH + 2] = prod ^ b2;
		table[val/VECT_WIDTH + 3] = prod ^ b2 ^ b;
		) "\n#elif VECT_WIDTH == 2\n" STRINGIFY(
		// add `b` to second element
		table[val/VECT_WIDTH] = (prod ^ b) | ((nat_uint)prod >> 16);
		table[val/VECT_WIDTH + 1] = (prod ^ b ^ b2) | (((nat_uint)prod ^ b2) >> 16);
		) "\n#elif VECT_WIDTH == 4\n" STRINGIFY(
		nat_uint res = (prod ^ b2) | ((nat_uint)prod >> 32);
		table[val/VECT_WIDTH] = (res ^ b ^ ((nat_uint)b >> 32)) | (res >> 16);
		) "\n#endif\n" STRINGIFY(
	}
	
	
	void compute_lltable(__local ushort* table, __global const ushort* restrict coeffs, const ushort numCoeff) {
		barrier(CLK_LOCAL_MEM_FENCE);
		
		// assume workgroup size is a power of 2
		) "\n#if COL_GROUP_SIZE*4 > 256\n" STRINGIFY(
		// process multiple coefficients at a time
		for(nat_uint coeffBase=0; coeffBase<numCoeff; coeffBase+=(COL_GROUP_SIZE*4/256)) {
			const nat_uint coeffI = coeffBase + (get_local_id(0)*4)/256;
			if(coeffI < numCoeff) {
				const nat_uint coeff = coeffs[coeffI];
				gf16_multiply_write_ll(
					(__local nat_uint*)(table + coeffI*256),
					(get_local_id(0)*4) & 0xff,
					coeff
				);
			}
		}
		
		) "\n#else\n" STRINGIFY(
		// process one coefficient at a time
		
		for(nat_uint coeffI=0; coeffI<numCoeff; coeffI++) {
			const nat_uint coeff = coeffs[coeffI];
			__local nat_uint* nat_table = (__local nat_uint*)(table + coeffI*256);
			) "\n#pragma unroll\n" STRINGIFY(
			for(nat_uint valI=0; valI<256; valI+=COL_GROUP_SIZE*4) {
				gf16_multiply_write_ll(nat_table, valI + get_local_id(0)*4, coeff);
			}
		}
		
		) "\n#endif\n" STRINGIFY(
		
		barrier(CLK_LOCAL_MEM_FENCE);
	}
	
	inline nat_uint lltable_multiply(__local const ushort* table, nat_uint val) {
		) "\n#if VECT_WIDTH==2\n" STRINGIFY(
		return upsample(
			(ushort)(table[(val>>16) & 0xff] ^ gf16_multiply_256(table[(val>>24) & 0xff])),
			(ushort)(table[val & 0xff] ^ gf16_multiply_256(table[(val>>8) & 0xff]))
		);
		) "\n#else\n" STRINGIFY(
		nat_uint result = table[val & 0xff] ^ gf16_multiply_256(table[(val>>8) & 0xff]);
		) "\n#pragma unroll\n" STRINGIFY(
		for(nat_uint v=1; v<VECT_WIDTH; v++) {
			val >>= 16;
			result |= (nat_uint)(table[val & 0xff] ^ gf16_multiply_256(table[(val>>8) & 0xff])) << (v*16);
		}
		return result;
		) "\n#endif\n" STRINGIFY(
	}
);
const static char _ocl_method_shuffle[] =
"#undef READ_SRC\n"
"#define READ_SRC gf16_shuffle_read\n"
"#define WRITE_DST_OVERRIDE(dst, idx, val) gf16_shuffle_write(dst, idx, val)\n"
"#define WRITE_DST_ADD_OVERRIDE(dst, idx, val) gf16_shuffle_add(dst, idx, val)\n"
"#undef val_t\n"
"#if VECT_WIDTH==1\n"
" #define val_t uchar4\n"
"#elif VECT_WIDTH==2\n"
" #define val_t uchar8\n"
"#else\n"
" #define val_t uchar16\n"
"#endif\n"
"#define LUT_REF(name, idx) ((__private uchar4*)(name) + (idx)*16)\n"
"#define LUT_DECLARATION(name, size) __private uchar4 name[16 * (size)]\n"
"#define LUT_GENERATE compute_shuffletable\n"
"#define LUT_MULTIPLY shuffle_multiply\n"
STRINGIFY(
	ushort gf16_mult2(const ushort v) {
		return (v << 1) ^ (-(v >= 0x8000) & (GF16_POLYNOMIAL & 0xffff));
	}
	void compute_shuffletable(__private uchar4* table, __global const ushort* restrict coeffs, const ushort numCoeff) {
		//for(uint i=get_local_id(0); i<numCoeff; i+=COL_GROUP_SIZE) { // for local memory
		for(uint i=0; i<numCoeff; i++) {
			ushort val = coeffs[i];
			ushort val2 = gf16_mult2(val);
			ushort val3 = val2 ^ val;
			__private uchar4* tbl = table + i*16;
			
			tbl[0] = (uchar4)(0, val&0xff, val2&0xff, val3&0xff);
			tbl[1] = (uchar4)(0, val>>8, val2>>8, val3>>8);
			
			uchar4 ri;
			) "\n#if GF16_POLYNOMIAL != 0x1100b\n#error Unsupported polynomial\n#endif\n"
			"#define MUL4(p, c) "
				"ri = tbl[p+1] >> (uchar)6;"
				"tbl[c+1] = ((tbl[p+1] << (uchar)2) | (tbl[p] >> (uchar)6)) ^ (ri << (uchar)4);"
				"tbl[c] = (tbl[p] << (uchar)2) ^ shuffle((uchar4)((uchar)0, (uchar)0xb, (uchar)(0xb<<1), (uchar)((0xb<<1)^0xb)), ri)\n" STRINGIFY(
			MUL4(0, 2);
			MUL4(2, 4);
			MUL4(4, 6);
			MUL4(6, 8);
			MUL4(8, 10);
			MUL4(10, 12);
			MUL4(12, 14);
			) "\n#undef MUL4\n" STRINGIFY(
		}
	}
	
	val_t gf16_shuffle_read(__global const val_t* restrict src, const size_t idx) {
		const val_t data = src[idx];
		return (val_t)(data.even, data.odd);
	}
	void gf16_shuffle_write(__global val_t* dst, const memsize_t offs, const val_t val) {
		*(__global ushort4*)(dst + offs) = upsample(val.hi, val.lo);
	}
	void gf16_shuffle_add(__global val_t* dst, const memsize_t offs, const val_t val) {
		*(__global ushort4*)(dst + offs) ^= upsample(val.hi, val.lo);
	}
	
	inline val_t shuffle_multiply(__private const uchar4* table, val_t val) {
		nat_ucharX resLo = shuffle(table[0], val.lo) ^ shuffle(table[8], val.hi);
		nat_ucharX resHi = shuffle(table[1], val.lo) ^ shuffle(table[9], val.hi);
		resLo ^= shuffle(table[2], val.lo >>(uchar)2) ^ shuffle(table[10], val.hi >>(uchar)2);
		resHi ^= shuffle(table[3], val.lo >>(uchar)2) ^ shuffle(table[11], val.hi >>(uchar)2);
		resLo ^= shuffle(table[4], val.lo >>(uchar)4) ^ shuffle(table[12], val.hi >>(uchar)4);
		resHi ^= shuffle(table[5], val.lo >>(uchar)4) ^ shuffle(table[13], val.hi >>(uchar)4);
		resLo ^= shuffle(table[6], val.lo >>(uchar)6) ^ shuffle(table[14], val.hi >>(uchar)6);
		resHi ^= shuffle(table[7], val.lo >>(uchar)6) ^ shuffle(table[15], val.hi >>(uchar)6);
		return (val_t)(resLo, resHi);
	}
);
const static char _ocl_method_log[] =
"#define GF_MULTIPLY(a, b) gf16_multiply_log(a, b  EX_TABLE_ARGS)\n"
"#undef READ_SRC\n"
"#define READ_SRC(src, idx) gf16_log_src(src[idx]  EX_TABLE_ARGS)\n"
STRINGIFY(
	val_t gf16_log_src(val_t val  EX_TABLE_ARGS_DECL) {
		) "\n#if VECT_WIDTH==2\n" STRINGIFY(
		return upsample(gf16_log[val >> 16], gf16_log[val & 0xffff]);
		) "\n#else\n" STRINGIFY(
		val_t res = gf16_log[val & 0xffff];
		) "\n#pragma unroll\n" STRINGIFY(
		for(int v=1; v<VECT_WIDTH; v++) {
			int shift = v*16;
			res |= (val_t)gf16_log[(val>>shift) & 0xffff] << shift;
		}
		return res;
		) "\n#endif\n" STRINGIFY(
	}
	
	nat_uint gfmat_calc_coeff_log(nat_uint recBlock, ushort inCoeff) {
		uint result = inCoeff * (ushort)(RECOVERY_EXP(recBlock));
		// calc 'result %= 65535'
		result = (result >> 16) + (result & 65535);
		result += result >> 16;
		return result & 65535;
	}
	
	nat_uint gf16_multiply_log(nat_uint a, nat_uint b  EX_TABLE_ARGS_DECL) {
		nat_uint result = 0;
		) "\n#pragma unroll\n" STRINGIFY(
		for(int v=0; v<VECT_WIDTH; v++) {
			nat_uint va = a & 65535;
			// TODO: try defering this conditional so compiler optimizes stuff inside - this seems to slightly benefit on some platforms, makes others much worse
			if(va != 65535) { // exp(a) != 0
				// compute (va+b) % 65535
				) "\n#if VECT_WIDTH == 1\n" STRINGIFY(
				va += add_sat(va, b) == (ushort)65535;
				va += b;
				) "\n#else\n" STRINGIFY(
				va += b;
				va = (va >> 16) + (va & 0xffff);
				) "\n#endif\n" STRINGIFY(
				
				) "\n#ifdef OCL_METHOD_LOG_SMALL\n" STRINGIFY(
				uint vatmp = gf16_antilog[va >> 3];
				vatmp <<= va & 7;
				vatmp ^= gf16_antilog[8192 + (vatmp >> 16)];
				va = vatmp & 0xffff;
				) "\n#elif defined(OCL_METHOD_LOG_TINY)\n" STRINGIFY(
				uint vatmp = gf16_antilog[va >> 4];
				vatmp <<= va & 15;
				vatmp ^= gf16_antilog[4096 + (vatmp >> 24)] << 8;
				vatmp &= 0xffffff;
				vatmp ^= gf16_antilog[4096 + (vatmp >> 16)];
				va = vatmp & 0xffff;
				) "\n#else\n" STRINGIFY(
				va = gf16_antilog[va];
				) "\n#endif\n" STRINGIFY(
				
				result |= va << (v*16);
			}
			a >>= 16;
		}
		return result;
	}
);
/* // probably not that useful of an idea
const static char _ocl_method_splitmul[] =
"#define GF_MULTIPLY gf16_multiply_splitmul\n"
STRINGIFY(
	nat_uint gf16_multiply_splitmul(nat_uint a, nat_uint b) {
		nat_uint result = 0;
		ushort ba = (b>>12)*16;
		ushort bb = ((b>>8) & 0xf)*16;
		ushort bc = ((b>>4) & 0xf)*16;
		ushort bd = (b & 0xf)*16;
		) "\n#pragma unroll\n" STRINGIFY(
		for(int v=0; v<VECT_WIDTH; v++) {
			ushort ax = (a >> 8) & 0xff;
			ushort ay = a & 0xff;
			ushort val = 
				  mul_split1[bd + ay]
				^ gf16_mul16(mul_split1[bc + ay])
				^ mul_split256[bb + ay] ^ mul_split256[bd + ax]
				^ gf16_mul16(mul_split256[ba + ay] ^ mul_split256[bc + ax])
				^ mul_split4107[bb + ax]
				^ gf16_mul16(mul_split4107[ba + ax])
			
			result |= val << (v*16);
			a >>= 16;
		}
		return result;
	}
);
*/

// kernel sources
// defined during compile: MUL_ONLY, numInputs

// strategy which caches several inputs, dot-products to multiple outputs, and doesn't require a LUT per coefficient
#define KERNEL_NOLUT_MULGROUP \
"(__global val_t* restrict dst, __global const val_t* restrict src, COEFF_TABLE_TYPE const ushort* restrict coeff, const nat_uint outBlk, const nat_uint numOutputs, const nat_uint _numInputs, __local val_t* cache  EX_TABLE_ARGS_DECL) { \n"\
"	const memsize_t len = get_global_size(0)*COL_GROUP_ITERS;                                    \n"\
"	const memsize_t globalCol = get_global_id(0)*COL_GROUP_ITERS;                                \n"\
"	const uint col = get_local_id(0)*COL_GROUP_ITERS;                                            \n"\
"	__global const val_t* srcBase = src + globalCol;                                             \n"\
"	                                                                                             \n"\
"	val_t result[OUTPUT_GROUPING][COL_GROUP_ITERS];                                              \n"\
"	val_t val[COL_GROUP_ITERS];                                                                  \n"\
"	#pragma unroll                                                                               \n"\
"	for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++) {                                         \n"\
"		#ifdef DO_CACHE                                                                          \n"\
"			val[iter] = READ_SRC(srcBase, iter);                                                 \n"\
"			if(DO_CACHE == 1) cache[col+iter] = val[iter];                                       \n"\
"		#else                                                                                    \n"\
"			val[iter] = cache[col+iter];                                                         \n"\
"		#endif                                                                                   \n"\
"	}                                                                                            \n"\
"	                                                                                             \n"\
"	nat_uint curCoeff;                                                                           \n"\
"	#ifdef GFMAT_COEFF_SHORTCUT                                                                  \n"\
"		curCoeff = gfmat_calc_coeff_log(outBlk, coeff[0]);                                       \n"\
"		#pragma unroll                                                                           \n"\
"		for (nat_uint o = 0; o < numOutputs; o++) {                                              \n"\
"			#pragma unroll                                                                       \n"\
"			for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++)                                   \n"\
"				result[o][iter] = GF_MULTIPLY(val[iter], curCoeff);                              \n"\
"			curCoeff += coeff[0];                                                                \n"\
"			curCoeff += curCoeff >> 16;                                                          \n"\
"			curCoeff &= 0xffff;                                                                  \n"\
"		}                                                                                        \n"\
"	#else                                                                                        \n"\
"		#pragma unroll                                                                           \n"\
"		for (nat_uint o = 0; o < numOutputs; o++) {                                              \n"\
"			curCoeff = GET_COEFF(outBlk+o, 0u);                                                  \n"\
"			#pragma unroll                                                                       \n"\
"			for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++)                                   \n"\
"				result[o][iter] = GF_MULTIPLY(val[iter], curCoeff);                              \n"\
"		}                                                                                        \n"\
"	#endif                                                                                       \n"\
"	                                                                                             \n"\
"	for (nat_uint i = 1; i < _numInputs; i++) {                                                  \n"\
"		srcBase += len;                                                                          \n"\
"		#pragma unroll                                                                           \n"\
"		for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++) {                                     \n"\
"			#ifdef DO_CACHE                                                                      \n"\
"				val[iter] = READ_SRC(srcBase, iter);                                             \n"\
"				if(DO_CACHE == 1) cache[i*COL_GROUP_SIZE*COL_GROUP_ITERS + col+iter] = val[iter];\n"\
"			#else                                                                                \n"\
"				val[iter] = cache[i*COL_GROUP_SIZE*COL_GROUP_ITERS + col+iter];                  \n"\
"			#endif                                                                               \n"\
"		}                                                                                        \n"\
"		                                                                                         \n"\
"		#ifdef GFMAT_COEFF_SHORTCUT                                                              \n"\
"			curCoeff = gfmat_calc_coeff_log(outBlk, coeff[i]);                                   \n"\
"			#pragma unroll                                                                       \n"\
"			for (nat_uint o = 0; o < numOutputs; o++) {                                          \n"\
"				#pragma unroll                                                                   \n"\
"				for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++)                               \n"\
"					result[o][iter] ^= GF_MULTIPLY(val[iter], curCoeff);                         \n"\
"				curCoeff += coeff[i];                                                            \n"\
"				curCoeff += curCoeff >> 16;                                                      \n"\
"				curCoeff &= 0xffff;                                                              \n"\
"			}                                                                                    \n"\
"		#else                                                                                    \n"\
"			#pragma unroll                                                                       \n"\
"			for (nat_uint o = 0; o < numOutputs; o++) {                                          \n"\
"				curCoeff = GET_COEFF(outBlk+o, i);                                               \n"\
"				#pragma unroll                                                                   \n"\
"				for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++)                               \n"\
"					result[o][iter] ^= GF_MULTIPLY(val[iter], curCoeff);                         \n"\
"			}                                                                                    \n"\
"		#endif                                                                                   \n"\
"	}                                                                                            \n"\
"	__global val_t* dstBase = dst + WBUF_REF(outBlk, len, globalCol);                            \n"\
"	#pragma unroll                                                                               \n"\
"	for (nat_uint o = 0; o < numOutputs; o++) {                                                  \n"\
"		#pragma unroll                                                                           \n"\
"		for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++) {                                     \n"\
"			WRITE_DST(dstBase, iter, result[o][iter]);                                           \n"\
"		}                                                                                        \n"\
"		dstBase += len;                                                                          \n"\
"	}                                                                                            \n"\
"}                                                                                               \n"

const static char _ocl_kernel_nolut_funcs[] =
"#ifdef WRITE_DST_OVERRIDE\n"
" #ifdef MUL_ONLY\n"
"  #define WRITE_DST WRITE_DST_OVERRIDE\n"
" #else\n"
"  #define WRITE_DST WRITE_DST_ADD_OVERRIDE\n"
" #endif\n"
"#elif defined(MUL_ONLY)\n"
" #define WRITE_DST(dst, idx, val) dst[idx] = val\n"
"#else\n"
" #define WRITE_DST(dst, idx, val) dst[idx] ^= val\n"
"#endif\n"
"#ifdef OCL_METHOD_LOG\n"
" #define GET_COEFF(o, i) gfmat_calc_coeff_log(o, coeff[i])\n"
"#else\n"
" #define GET_COEFF(o, i) coeff[COBUF_REF(o, i)]\n"
"#endif\n"
"#define DO_CACHE 1\n"
"inline void KERNFN(mulgroup_cache)" KERNEL_NOLUT_MULGROUP "\n"
"#undef DO_CACHE\n"
"#define DO_CACHE 0\n"
"inline void KERNFN(mulgroup_nocache)" KERNEL_NOLUT_MULGROUP "\n"
"#undef DO_CACHE\n"
"inline void KERNFN(mulgroup_read)" KERNEL_NOLUT_MULGROUP "\n"
"#undef GET_COEFF\n";

const static char _ocl_kernel_nolut[] = STRINGIFY({
	) "\n#ifdef LMEM_CACHE_SIZE\n" STRINGIFY(
	// copy the antilog table to local memory for maybe faster access
	__local ushort gf16_antilog[LMEM_CACHE_SIZE] __attribute__ ((aligned (VECT_WIDTH*2)));
	__local nat_uint* lmemDst = (__local nat_uint*)gf16_antilog;
	LMEM_SRC_TYPE nat_uint* lmemSrc = (LMEM_SRC_TYPE nat_uint*)gf16_antilog_src;
	for(ushort i=get_local_id(0); i<LMEM_CACHE_SIZE/VECT_WIDTH; i+=COL_GROUP_SIZE) {
		lmemDst[i] = lmemSrc[i];
	}
	) "\n#endif\n" STRINGIFY(
	) "\n#ifdef COPY_COEFF\n" STRINGIFY(
	// TODO: these could be optimized more with wider copies
	) "\n #ifdef METHOD_LOG\n" STRINGIFY(
	__local ushort lcoeff[SUBMIT_INPUTS] __attribute__ ((aligned (VECT_WIDTH*2)));
	for(uint i=get_local_id(0); i<SUBMIT_INPUTS; i+=COL_GROUP_SIZE)
		lcoeff[i] = coeff[i];
	) "\n #else\n" STRINGIFY(
	__local ushort lcoeff[SUBMIT_INPUTS*NUM_OUTPUTS] __attribute__ ((aligned (VECT_WIDTH*2)));
	for(uint i=get_local_id(0); i<SUBMIT_INPUTS*NUM_OUTPUTS; i+=COL_GROUP_SIZE)
		lcoeff[i] = coeff[i];
	) "\n #endif\n" STRINGIFY(
	) "\n#else\n" STRINGIFY(
	__global const ushort* lcoeff = coeff;
	) "\n#endif\n" STRINGIFY(
	) "\n#if defined(COPY_COEFF) || defined(LMEM_CACHE_SIZE)\n" STRINGIFY(
	barrier(CLK_LOCAL_MEM_FENCE);
	) "\n#endif\n" STRINGIFY(
	
	
	
	// compute output
	) "\n#if NUM_OUTPUTS <= OUTPUTS_PER_THREAD\n" STRINGIFY(
		nat_uint outBlk = 0;
		const bool isPartialOutputsThread = false;
	) "\n#else\n" STRINGIFY(
		nat_uint outBlk = get_global_id(1) * OUTPUTS_PER_THREAD;
		) "\n#if NUM_OUTPUTS % OUTPUTS_PER_THREAD == 0\n" STRINGIFY(
		const bool isPartialOutputsThread = false;
		) "\n#else\n" STRINGIFY(
		const bool isPartialOutputsThread = get_global_id(1) == get_global_size(1);
		) "\n#endif\n" STRINGIFY(
	) "\n#endif\n" STRINGIFY(
	
	if(!isPartialOutputsThread) {
		) "\n#if OUTPUTS_PER_THREAD > OUTPUT_GROUPING\n" STRINGIFY(
		__local val_t cache[SUBMIT_INPUTS*COL_GROUP_SIZE*COL_GROUP_ITERS];
		
		// first iteration -> copy loaded data to cache
		//barrier(CLK_LOCAL_MEM_FENCE);  // probably not needed? thread only accesses the same value that it writes
		KERNFN(mulgroup_cache)(dst, src, lcoeff, outBlk, OUTPUT_GROUPING, numInputs, cache  EX_TABLE_ARGS);
		outBlk += OUTPUT_GROUPING;
		//barrier(CLK_LOCAL_MEM_FENCE);
		
		for (; outBlk < OUTPUTS_PER_THREAD - OUTPUT_GROUPING + 1; outBlk += OUTPUT_GROUPING) {
			KERNFN(mulgroup_read)(dst, src, lcoeff, outBlk, OUTPUT_GROUPING, numInputs, cache  EX_TABLE_ARGS);
		}
		) "\n#endif\n"
		"#if OUTPUTS_PER_THREAD == OUTPUT_GROUPING || OUTPUTS_PER_THREAD % OUTPUT_GROUPING > 0\n" STRINGIFY(
		) "\n#if OUTPUTS_PER_THREAD > OUTPUT_GROUPING\n" STRINGIFY(
			KERNFN(mulgroup_read)(dst, src, lcoeff, outBlk, (OUTPUTS_PER_THREAD == OUTPUT_GROUPING ? OUTPUTS_PER_THREAD : OUTPUTS_PER_THREAD % OUTPUT_GROUPING), numInputs, cache  EX_TABLE_ARGS);
		) "\n#else\n" STRINGIFY(
			KERNFN(mulgroup_nocache)(dst, src, lcoeff, outBlk, (OUTPUTS_PER_THREAD == OUTPUT_GROUPING ? OUTPUTS_PER_THREAD : OUTPUTS_PER_THREAD % OUTPUT_GROUPING), numInputs, (__local val_t*)0  EX_TABLE_ARGS);
		) "\n#endif\n" STRINGIFY(
		) "\n#endif\n" STRINGIFY(
	} else {
		) "\n#define OUTPUTS_THIS_THREAD (NUM_OUTPUTS % OUTPUTS_PER_THREAD)\n" STRINGIFY(
		
		) "\n#if OUTPUTS_THIS_THREAD > OUTPUT_GROUPING\n" STRINGIFY(
		__local val_t cache[SUBMIT_INPUTS*COL_GROUP_SIZE*COL_GROUP_ITERS];
		// first iteration -> copy loaded data to cache
		//barrier(CLK_LOCAL_MEM_FENCE);
		KERNFN(mulgroup_cache)(dst, src, lcoeff, outBlk, OUTPUT_GROUPING, numInputs, cache  EX_TABLE_ARGS);
		outBlk += OUTPUT_GROUPING;
		//barrier(CLK_LOCAL_MEM_FENCE);
		
		for (; outBlk < OUTPUTS_THIS_THREAD - OUTPUT_GROUPING + 1; outBlk += OUTPUT_GROUPING) {
			KERNFN(mulgroup_read)(dst, src, lcoeff, outBlk, OUTPUT_GROUPING, numInputs, cache  EX_TABLE_ARGS);
		}
		) "\n#endif\n#if OUTPUTS_THIS_THREAD == OUTPUT_GROUPING || OUTPUTS_THIS_THREAD % OUTPUT_GROUPING > 0\n" STRINGIFY(
		) "\n#if OUTPUTS_THIS_THREAD > OUTPUT_GROUPING\n" STRINGIFY(
			KERNFN(mulgroup_read)(dst, src, lcoeff, outBlk, (OUTPUTS_THIS_THREAD == OUTPUT_GROUPING ? OUTPUTS_THIS_THREAD : OUTPUTS_THIS_THREAD % OUTPUT_GROUPING), numInputs, cache  EX_TABLE_ARGS);
		) "\n#else\n" STRINGIFY(
			KERNFN(mulgroup_nocache)(dst, src, lcoeff, outBlk, (OUTPUTS_THIS_THREAD == OUTPUT_GROUPING ? OUTPUTS_THIS_THREAD : OUTPUTS_THIS_THREAD % OUTPUT_GROUPING), numInputs, (__local val_t*)0  EX_TABLE_ARGS);
		) "\n#endif\n" STRINGIFY(
		) "\n#endif\n" STRINGIFY(
		
	}
}) "\n#undef WRITE_DST\n";
// strategy which caches several inputs, uses dot-product operations to a single output, and a LUT per coefficient
const static char _ocl_kernel_cachelut[] =
"#ifdef WRITE_DST_OVERRIDE\n"
" #ifdef MUL_ONLY\n"
"  #define WRITE_DST WRITE_DST_OVERRIDE\n"
" #else\n"
"  #define WRITE_DST WRITE_DST_ADD_OVERRIDE\n"
" #endif\n"
"#elif defined(MUL_ONLY)\n"
" #define WRITE_DST(dst, idx, val) dst[idx] = val\n"
"#else\n"
" #define WRITE_DST(dst, idx, val) dst[idx] ^= val\n"
"#endif\n"
STRINGIFY({
	const uint colLocal = get_local_id(0);
	const memsize_t colGlobal = colLocal + get_group_id(0)*COL_GROUP_SIZE*COL_GROUP_ITERS;
	const memsize_t len = get_global_size(0) * COL_GROUP_ITERS;
	
	// compute output
	) "\n#if NUM_OUTPUTS <= OUTPUTS_PER_THREAD\n" STRINGIFY(
		const nat_uint outputsThisThread = NUM_OUTPUTS;
		const nat_uint outBase = 0;
		__global val_t* dstBase = dst + colGlobal;
	) "\n#else\n" STRINGIFY(
		const nat_uint outBase = get_global_id(1) * OUTPUTS_PER_THREAD;
		) "\n#if NUM_OUTPUTS % OUTPUTS_PER_THREAD == 0\n" STRINGIFY(
		const nat_uint outputsThisThread = OUTPUTS_PER_THREAD;
		) "\n#else\n" STRINGIFY(
		const nat_uint outputsThisThread = (get_global_id(1)+1 == get_global_size(1) ? NUM_OUTPUTS % OUTPUTS_PER_THREAD : OUTPUTS_PER_THREAD);
		) "\n#endif\n" STRINGIFY(
		__global val_t* dstBase = dst + WBUF_REF(outBase, len, colGlobal);
	) "\n#endif\n" STRINGIFY(
	__global const val_t* srcBase = src + colGlobal;
	
	__local val_t cache[SUBMIT_INPUTS][COL_GROUP_SIZE*COL_GROUP_ITERS];
	LUT_DECLARATION(lut_table, SUBMIT_INPUTS);
	LUT_GENERATE(lut_table, coeff + COBUF_REF(outBase, 0u), numInputs);
	
	
	) "\n#ifndef LUT_GENERATE_HAS_BARRIER\n" STRINGIFY(
	//barrier(CLK_LOCAL_MEM_FENCE);
	) "\n#endif\n" STRINGIFY(
	
	for(uint iter=0; iter<COL_GROUP_ITERS; iter++) { // first iteration -> copy loaded data to cache
		uint iterBase = iter*COL_GROUP_SIZE;
		
		val_t val = READ_SRC(srcBase, 0u);
		cache[0][colLocal+iterBase] = val;
		val_t result = LUT_MULTIPLY(LUT_REF(lut_table, 0u), val);
		
		__global const val_t* srcIterBase = srcBase;
		) "\n#ifdef numInputs\n#pragma unroll\n#endif\n" STRINGIFY(
		for (nat_uint input = 1; input < numInputs; input++) {
			srcIterBase += len;
			val = READ_SRC(srcIterBase, 0u);
			// copy global input to local cache to be used in subsequent iterations
			// TODO: investigate using async_work_group_copy
			cache[input][colLocal + iterBase] = val;
			result ^= LUT_MULTIPLY(LUT_REF(lut_table, input), val);
		}
		WRITE_DST(dstBase, iterBase, result);
		srcBase += COL_GROUP_SIZE;
	}
	//barrier(CLK_LOCAL_MEM_FENCE);
	
	// subsequent iterations: use cache
	for(nat_uint output=1; output<outputsThisThread; output++) {
		dstBase += len;
		LUT_GENERATE(lut_table, coeff + COBUF_REF(outBase+output, 0u), numInputs);
		for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++) {
			uint iterBase = iter*COL_GROUP_SIZE;
			val_t result = LUT_MULTIPLY(LUT_REF(lut_table, 0u), cache[0][colLocal + iterBase]);
			
			) "\n#ifdef numInputs\n#pragma unroll\n#endif\n" STRINGIFY(
			for (ushort input = 1; input < numInputs; input++) {
				result ^= LUT_MULTIPLY(LUT_REF(lut_table, input), cache[input][colLocal + iterBase]);
			}
			WRITE_DST(dstBase, iterBase, result);
		}
	}
}) "\n#undef WRITE_DST\n";

// strategy where inputs are pulled from global memory (not cached), multiplies to multiple outputs, and requires a LUT per coefficient
// seems to generally be less efficient that CACHELUT
const static char _ocl_kernel_lut_funcs[] =
"#ifdef WRITE_DST_OVERRIDE\n"
" #ifdef MUL_ONLY\n"
"  #define WRITE_DST WRITE_DST_OVERRIDE\n"
" #else\n"
"  #define WRITE_DST WRITE_DST_ADD_OVERRIDE\n"
" #endif\n"
"#elif defined(MUL_ONLY)\n"
" #define WRITE_DST(dst, idx, val) dst[idx] = val\n"
"#else\n"
" #define WRITE_DST(dst, idx, val) dst[idx] ^= val\n"
"#endif\n"
STRINGIFY(
	inline void KERNFN(mulgroup)(__global val_t* restrict dst, __global const val_t* restrict src, __global const ushort* restrict coeff, __local void* lut_table, const nat_uint outBlk, const nat_uint numOutputs, const nat_uint _numInputs) {
		const memsize_t len = get_global_size(0) * COL_GROUP_ITERS;
		const memsize_t globalCol = get_local_id(0) + get_group_id(0)*COL_GROUP_SIZE*COL_GROUP_ITERS;
		
		// TODO: defer looping to lut_generate
		) "\n#pragma unroll\n" STRINGIFY(
		for (ushort o = 0; o < numOutputs; o++) {
			LUT_GENERATE(LUT_REF(lut_table, o*SUBMIT_INPUTS), coeff + COBUF_REF(outBlk+o, 0u), _numInputs);
		}
		
		__global val_t* dstBase = dst + WBUF_REF(outBlk, len, globalCol);
		__global const val_t* srcBase = src + globalCol;
		for(nat_uint iter=0; iter<COL_GROUP_ITERS; iter++) {
			val_t result[OUTPUT_GROUPING];
			val_t val = READ_SRC(srcBase, 0u);
			) "\n#pragma unroll\n" STRINGIFY(
			for (ushort o = 0; o < numOutputs; o++) {
				result[o] = LUT_MULTIPLY(LUT_REF(lut_table, o*SUBMIT_INPUTS), val);
			}
			__global const val_t* srcIterBase = srcBase;
			for (ushort i = 1; i < _numInputs; i++) {
				srcIterBase += len;
				val = READ_SRC(srcIterBase, 0u);
				) "\n#pragma unroll\n" STRINGIFY(
				for (ushort o = 0; o < numOutputs; o++) {
					result[o] ^= LUT_MULTIPLY(LUT_REF(lut_table, o*SUBMIT_INPUTS+i), val);
				}
			}
			__global val_t* dstIterBase = dstBase;
			) "\n#pragma unroll\n" STRINGIFY(
			for (ushort o = 0; o < numOutputs; o++) {
				WRITE_DST(dstIterBase, 0u, result[o]);
				dstIterBase += len;
			}
			
			srcBase += COL_GROUP_SIZE;
			dstBase += COL_GROUP_SIZE;
		}
	}
);
const static char _ocl_kernel_lut[] = STRINGIFY({
	) "\n#if NUM_OUTPUTS <= OUTPUTS_PER_THREAD\n" STRINGIFY(
		nat_uint outBlk = 0;
		const bool isPartialOutputsThread = false;
	) "\n#else\n" STRINGIFY(
		nat_uint outBlk = get_global_id(1) * OUTPUTS_PER_THREAD;
		) "\n#if NUM_OUTPUTS % OUTPUTS_PER_THREAD == 0\n" STRINGIFY(
		const bool isPartialOutputsThread = false;
		) "\n#else\n" STRINGIFY(
		const bool isPartialOutputsThread = get_global_id(1)+1 == get_global_size(1);
		) "\n#endif\n" STRINGIFY(
	) "\n#endif\n" STRINGIFY(
	
	LUT_DECLARATION(lut_table, SUBMIT_INPUTS*OUTPUT_GROUPING);
	if(!isPartialOutputsThread) {
		) "\n#if OUTPUTS_PER_THREAD > OUTPUT_GROUPING\n" STRINGIFY(
		for (; outBlk < OUTPUTS_PER_THREAD - OUTPUT_GROUPING + 1; outBlk += OUTPUT_GROUPING) {
			KERNFN(mulgroup)(dst, src, coeff, lut_table, outBlk, OUTPUT_GROUPING, numInputs);
		}
		) "\n#endif\n#if OUTPUTS_PER_THREAD == OUTPUT_GROUPING || OUTPUTS_PER_THREAD % OUTPUT_GROUPING > 0\n" STRINGIFY({
			) "\n#define NUM_OUTPUTS_LEFT (OUTPUTS_PER_THREAD == OUTPUT_GROUPING ? OUTPUTS_PER_THREAD : OUTPUTS_PER_THREAD % OUTPUT_GROUPING)\n" STRINGIFY(
			
			KERNFN(mulgroup)(dst, src, coeff, lut_table, outBlk, NUM_OUTPUTS_LEFT, numInputs);
			
			) "\n#undef NUM_OUTPUTS_LEFT\n" STRINGIFY(
		}) "\n#endif\n" STRINGIFY(
	} else {
		) "\n#define OUTPUTS_THIS_THREAD (NUM_OUTPUTS % OUTPUTS_PER_THREAD)\n" STRINGIFY(
		
		) "\n#if OUTPUTS_THIS_THREAD > OUTPUT_GROUPING\n" STRINGIFY(
		for (; outBlk < OUTPUTS_THIS_THREAD - OUTPUT_GROUPING + 1; outBlk += OUTPUT_GROUPING) {
			KERNFN(mulgroup)(dst, src, coeff, lut_table, outBlk, OUTPUT_GROUPING, numInputs);
		}
		) "\n#endif\n#if OUTPUTS_THIS_THREAD == OUTPUT_GROUPING || OUTPUTS_THIS_THREAD % OUTPUT_GROUPING > 0\n" STRINGIFY({
			) "\n#define NUM_OUTPUTS_LEFT (OUTPUTS_THIS_THREAD == OUTPUT_GROUPING ? OUTPUTS_THIS_THREAD : OUTPUTS_THIS_THREAD % OUTPUT_GROUPING)\n" STRINGIFY(
			
			KERNFN(mulgroup)(dst, src, coeff, lut_table, outBlk, NUM_OUTPUTS_LEFT, numInputs);
			
			) "\n#undef NUM_OUTPUTS_LEFT\n" STRINGIFY(
		}) "\n#endif\n" STRINGIFY(
	}
}) "\n#undef WRITE_DST\n";

#undef STRINGIFY
#undef STR_HELPER
#undef STR



#define CEIL_DIV(a, b) (((a) + (b)-1) / (b))

// ensure value is a power of 2, rounding down if needed
static inline size_t round_down_pow2(size_t v) {
	if((v & (v-1)) == 0) return v; // is a power of 2 (shortcut exists because this is the common case)
	
	// find target number via a float conversion
	// (usage of float over double does mean that this can be wrong for very large numbers, but we're not expecting these)
	union { float f; uint32_t u; } tmp;
	tmp.f = (float)v;     // convert to float
	tmp.u &= 0xff800000;  // discard mantissa
	return (size_t)tmp.f; // convert back to int
	
	
	// alternative approach, avoiding flot<>int conversion, but using bit-scans
	/*
#ifdef _MSC_VER
	unsigned long idx;
	_BitScanReverse(&idx, v);
	return v & (1 << idx);
#else
	int idx = __builtin_clzl(v);
	return v & (1 << ((sizeof(unsigned long)-1) ^ idx));
#endif
	*/
}

// _sliceSize must be divisible by 2
bool GF16OCL::setup_kernels(Galois16OCLMethods method, unsigned targetInputBatch, unsigned targetIters, unsigned targetGrouping) {
	unsigned numOutputs = (unsigned)outputExponents.size();
	
	// TODO: get device info
	// CL_DEVICE_HOST_UNIFIED_MEMORY (deprecated?)
	// CL_DEVICE_ADDRESS_BITS (max referencable memory)
	
	
	unsigned infoShortVecSize = device.getInfo<CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT>(); // seems to usually be the same as CL_DEVICE_NATIVE_VECTOR_WIDTH_SHORT
	if(infoShortVecSize < 2) infoShortVecSize = 2; // assume all GPUs do 32-bit math efficiently (various nVidia platforms return 1, but 2 runs faster)
	if(infoShortVecSize > 2) infoShortVecSize = 4; // we currently only support vect-width=1,2,4
	wgSize = device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
#ifdef OCL_PREFER_WORKGROUP_MULTIPLE
	size_t wgSizeMultiple = getWGSize(context, device) * OCL_PREFER_WORKGROUP_MULTIPLE;
	if(wgSizeMultiple && wgSizeMultiple < wgSize)
		wgSize = wgSizeMultiple;
#endif
	if(wgSize > 8192) wgSize = 8192; // sanity check
	
	size_t deviceAvailConstSize = device.getInfo<CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE>();
	size_t deviceLocalSize = device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>() - 128; // subtract a little to allow some spare space if the device needs it
	//size_t deviceGlobalSize = device.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>(); // TODO: check sliceSize*numOutputs ?
	
	unsigned outputsPerThread = numOutputs; // currently, process all outputs on every kernel invocation
	std::stringstream sourceStream;
	
	unsigned groupIterations = 1;
	bool usesOutGrouping = false;
	unsigned sizePerWorkGroup = infoShortVecSize*2 * wgSize;
	unsigned outputGrouping = 8; // for nolut kernel; have seen some cards prefer '4' (on older cards?)
	const char* kernelCode;
	const char* kernelFuncs = NULL;
	const char* methodCode;
	const char* oclVerArg = "";
	// method selector
	coeffAsLog = false;
	switch(method) {
	case GF16OCL_BY2:
		inputBatchSize = 16;
		while(1) {
			if(inputBatchSize*sizePerWorkGroup > deviceLocalSize)
				inputBatchSize = (unsigned)(deviceLocalSize / sizePerWorkGroup);
			
			if(inputBatchSize < 1 && deviceLocalSize >= 2048) {
				// try reducing workgroup if too large
				if(wgSize > 256) {
					wgSize = 256;
					sizePerWorkGroup = infoShortVecSize*2 * wgSize;
					inputBatchSize = 16;
					continue;
				}
			}
			break;
		}
		
		if(targetInputBatch) inputBatchSize = targetInputBatch;
		
		kernelCode = _ocl_kernel_nolut;
		kernelFuncs = _ocl_kernel_nolut_funcs;
		methodCode = _ocl_method_by2;
		usesOutGrouping = true;
	break;
	case GF16OCL_SHUFFLE:
	//case GF16OCL_SHUFFLE2:
		oclVerArg = "-cl-std=CL1.1";
		inputBatchSize = 4;
		sizePerWorkGroup *= 2; // process two words, so double workgroup size
		if(targetInputBatch) inputBatchSize = targetInputBatch;
		// TODO: compute ideal iteration count
		groupIterations = (unsigned)round_down_pow2(
			deviceLocalSize / (sizePerWorkGroup * inputBatchSize)
		);
		
		kernelCode = _ocl_kernel_cachelut;
		methodCode = _ocl_method_shuffle;
	break;
	case GF16OCL_LOG:
	case GF16OCL_LOG_SMALL:
	case GF16OCL_LOG_TINY:
	case GF16OCL_LOG_SMALL_LMEM:
	case GF16OCL_LOG_TINY_LMEM:
		coeffAsLog = true;
		inputBatchSize = 8;
		
		{
			size_t reqLocalMem = 0;
			if(method == GF16OCL_LOG_SMALL_LMEM) reqLocalMem = 16384+256;
			if(method == GF16OCL_LOG_TINY_LMEM)  reqLocalMem =  8192+512;
			if(deviceLocalSize < reqLocalMem) return false;
			deviceLocalSize -= reqLocalMem;
		}
		
		while(1) {
			if(inputBatchSize*sizePerWorkGroup > deviceLocalSize)
				inputBatchSize = (unsigned)(deviceLocalSize / sizePerWorkGroup);
			
			if(inputBatchSize < 1 && deviceLocalSize >= 2048) {
				// try reducing workgroup if too large
				if(wgSize > 256) {
					wgSize = 256;
					sizePerWorkGroup = infoShortVecSize*2 * wgSize;
					inputBatchSize = 8;
					continue;
				}
			}
			break;
		}
		
		if(targetInputBatch) inputBatchSize = targetInputBatch;
		
		kernelCode = _ocl_kernel_nolut;
		kernelFuncs = _ocl_kernel_nolut_funcs;
		sourceStream << "#define OCL_METHOD_LOG\n";
		if(method == GF16OCL_LOG_SMALL || method == GF16OCL_LOG_SMALL_LMEM)
			sourceStream << "#define OCL_METHOD_LOG_SMALL\n";
		if(method == GF16OCL_LOG_TINY || method == GF16OCL_LOG_TINY_LMEM)
			sourceStream << "#define OCL_METHOD_LOG_TINY\n";
		methodCode = _ocl_method_log;
		usesOutGrouping = true;
	break;
	case GF16OCL_LOOKUP:
	case GF16OCL_LOOKUP_HALF:
	case GF16OCL_LOOKUP_NOCACHE:
	case GF16OCL_LOOKUP_HALF_NOCACHE:
	default:
		if(method == GF16OCL_LOOKUP_NOCACHE || method == GF16OCL_LOOKUP_HALF_NOCACHE) {
			unsigned tables = (unsigned)(deviceLocalSize / (method == GF16OCL_LOOKUP_HALF_NOCACHE ? 512 : 1024));
			if(tables >= 96)
				inputBatchSize = 8;
			else if(tables >= 60)
				inputBatchSize = 6;
			else if(tables >= 32)
				inputBatchSize = 4;
			else
				inputBatchSize = 2;
			outputGrouping = tables / inputBatchSize;
			if(outputGrouping > 16) outputGrouping = 16;
			
			groupIterations = 8; // generally little reason not to do multiple iters
			
			if(targetInputBatch) inputBatchSize = targetInputBatch;
			kernelCode = _ocl_kernel_lut;
			kernelFuncs = _ocl_kernel_lut_funcs;
			usesOutGrouping = true;
		} else {
			inputBatchSize = 4;
			if(targetInputBatch) inputBatchSize = targetInputBatch;
			// TODO: compute ideal iteration count
			while(1) {
				groupIterations = (unsigned)round_down_pow2(
					(deviceLocalSize - inputBatchSize*(method == GF16OCL_LOOKUP_HALF ? 512 : 1024))
					/ (sizePerWorkGroup * inputBatchSize)
				);
				if(groupIterations < 1 && deviceLocalSize >= 8192) {
					// maybe the workgroup is too big
					if(wgSize > 256) {
						wgSize = 256;
						sizePerWorkGroup = infoShortVecSize*2 * wgSize;
						continue;
					}
					// try reducing input batch size
					if(inputBatchSize > 2) {
						inputBatchSize = 2;
						continue;
					}
				}
				break;
			}
			kernelCode = _ocl_kernel_cachelut;
		}
		
		methodCode = (method == GF16OCL_LOOKUP_HALF || method == GF16OCL_LOOKUP_HALF_NOCACHE) ? _ocl_method_ll : _ocl_method_lh;
		
		// write multiply-by-256 reduction table
		sourceStream << "__constant ushort mul256poly[256] = {0";
		for(int i=1; i<256; i++) {
			int n = i<<8;
			for(int b=0; b<8; b++)
				n = (n << 1) ^ (-(n>>15) & GF16_POLYNOMIAL);
			sourceStream << ',' << (n & 0xffff);
		}
		sourceStream << "};\n";
	break;
	}
	
	if(inputBatchSize < 1) return false;
	
	if(targetIters) groupIterations = targetIters;
	if(targetGrouping) outputGrouping = targetGrouping;
	
	if(groupIterations < 1 || outputGrouping < 1) return false;
	
	// for very small slices, scale down iterations/wgSize
	if(sizePerWorkGroup * groupIterations > sliceSize) {
		// scale down iterations first
		if(groupIterations > 1) {
			groupIterations = (unsigned)CEIL_DIV(sliceSize, sizePerWorkGroup);
			if(groupIterations < 1) groupIterations = 1;
		}
		// if iterations cannot be scaled down, scale down workgroup size next
		if(groupIterations == 1 && sizePerWorkGroup > sliceSize) {
			wgSize = CEIL_DIV(sliceSize, infoShortVecSize*2);
			if(wgSize < 1) wgSize = 1;
			sizePerWorkGroup = infoShortVecSize*2 * wgSize;
		}
	}
	
	// code generation for log methods
	uint16_t* tblLog = NULL;
	uint16_t* tblAntiLog = NULL;
	unsigned tblAntiLogSize = 0;
	if(coeffAsLog) {
		// write output exponents to remove the need to transfer it later
		std::stringstream recoveryExpCode;
		uint16_t recOffset = outputExponents[0];
		bool recOffsetIsSequential = true;
		recoveryExpCode << "__constant ushort recoveryIdx[" << numOutputs << "] = {" << outputExponents[0];
		for(unsigned i=1; i<numOutputs; i++) {
			recoveryExpCode << ',' << outputExponents[i];
			if(outputExponents[i] != recOffset+i)
				recOffsetIsSequential = false;
		}
		recoveryExpCode << "};\n";
		if(recOffsetIsSequential) {
			// if all recovery slice numbers are sequential (will be true most of the time), save some memory by cutting out the table
			sourceStream << "#define RECOVERY_EXP(n) (ushort)(n+" << recOffset << ")\n";
			sourceStream << "#define GFMAT_COEFF_SHORTCUT\n";
		} else {
			sourceStream << "#define RECOVERY_EXP(n) recoveryIdx[n]\n" << recoveryExpCode.str();
			if(deviceAvailConstSize < numOutputs*2) return false; // TODO: may need to have option to pass it as global memory
			deviceAvailConstSize -= numOutputs*2;
		}
		
		// construct log/exp table and embed into OpenCL source
		tblLog = new uint16_t[65536];
		tblLog[0] = 65535; // special value to represent 0
		int n = 1;
		if(method == GF16OCL_LOG) {
			tblAntiLogSize = 65536;
			tblAntiLog = new uint16_t[tblAntiLogSize];
			for (int exp = 0; exp < 65535; exp++) {
				tblAntiLog[exp] = n;
				tblLog[n] = exp;
				n = (n << 1) ^ (-(n>>15) & GF16_POLYNOMIAL);
			}
			tblAntiLog[65535] = tblAntiLog[0]; // saves having to wrap around 65535 to 0
		}
		else {
			int bitsCut = (method == GF16OCL_LOG_SMALL || method == GF16OCL_LOG_SMALL_LMEM ? 3 : 4);
			if(bitsCut == 3)
				tblAntiLogSize = 8192+128;
			else
				tblAntiLogSize = 4096+256;
			tblAntiLog = new uint16_t[tblAntiLogSize];
			for (int exp = 0; exp < 65535; exp++) {
				if((exp & ((1<<bitsCut)-1)) == 0)
					tblAntiLog[exp >> bitsCut] = n;
				tblLog[n] = exp;
				n = (n << 1) ^ (-(n>>15) & GF16_POLYNOMIAL);
			}
			// add reduction table
			uint16_t* tblALogReduction = tblAntiLog + (1u << (16-bitsCut));
			tblALogReduction[0] = 0;
			for (int i = 1; i < (1<<(bitsCut+4)); i++) {
				n = i << (12-bitsCut);
				for (int j = 0; j < (bitsCut+4); j++)
					n = (n << 1) ^ (-(n>>15) & GF16_POLYNOMIAL);
				tblALogReduction[i] = n;
			}
		}
		
		// try to fit antiLog table first, then log table
		if(deviceAvailConstSize >= tblAntiLogSize*2) {
			deviceAvailConstSize -= tblAntiLogSize*2;
			sourceStream << "__constant ushort gf16_antilog";
			if(method == GF16OCL_LOG_SMALL_LMEM || method == GF16OCL_LOG_TINY_LMEM)
				sourceStream << "_src";
			sourceStream << "[" << tblAntiLogSize << "] __attribute__ ((aligned (VECT_WIDTH*2))) = {" << tblAntiLog[0];
			for (unsigned i = 1; i < tblAntiLogSize; i++)
				sourceStream << ',' << tblAntiLog[i];
			sourceStream << "};\n";
			delete[] tblAntiLog;
			tblAntiLog = NULL;
		}
		if(deviceAvailConstSize >= 131072) {
			sourceStream << "__constant ushort gf16_log[65536] = {" << tblLog[0];
			for(int i=1; i<65536; i++)
				sourceStream << ',' << tblLog[i];
			sourceStream << "};\n";
			delete[] tblLog;
			tblLog = NULL;
		}
		
		if(method == GF16OCL_LOG_SMALL_LMEM || method == GF16OCL_LOG_TINY_LMEM) {
			sourceStream << "#define LMEM_CACHE_SIZE " << tblAntiLogSize << "\n";
			if(tblAntiLog)
				sourceStream << "#define LMEM_SRC_TYPE __global const\n";
			else
				sourceStream << "#define LMEM_SRC_TYPE __constant\n";
			if(tblLog) {
				if(tblAntiLog)
					sourceStream << "#define EX_TABLE_KARGS_DECL , __global const ushort* restrict gf16_log, __global const ushort* restrict gf16_antilog_src\n";
				else
					sourceStream << "#define EX_TABLE_KARGS_DECL , __global const ushort* restrict gf16_log\n";
				
				sourceStream <<
					"#define EX_TABLE_ARGS_DECL , __global const ushort* restrict gf16_log, __local const ushort* restrict gf16_antilog\n"
					"#define EX_TABLE_ARGS , gf16_log, gf16_antilog\n";
			} else {
				if(tblAntiLog)
					sourceStream << "#define EX_TABLE_KARGS_DECL , __global const ushort* restrict gf16_antilog_src\n";
				else
					sourceStream << "#define EX_TABLE_KARGS_DECL\n";
				
				sourceStream <<
					"#define EX_TABLE_ARGS_DECL , __local const ushort* restrict gf16_antilog\n"
					"#define EX_TABLE_ARGS , gf16_antilog\n";
			}
		} else {
			if(tblLog) {
				if(tblAntiLog) {
					sourceStream <<
						"#define EX_TABLE_ARGS_DECL , __global const ushort* restrict gf16_log, __global const ushort* restrict gf16_antilog\n"
						"#define EX_TABLE_ARGS , gf16_log, gf16_antilog\n";
				} else {
					sourceStream <<
						"#define EX_TABLE_ARGS_DECL , __global const ushort* restrict gf16_log\n"
						"#define EX_TABLE_ARGS , gf16_log\n";
				}
			}
		}
	}
	sourceStream << _ocl_defines << "\n" << methodCode << "\n";
	
	
	
	// avoid uneven workgroups by aligning to workgroup size
	bytesPerGroup = sizePerWorkGroup * groupIterations;
	size_t sliceGroups = CEIL_DIV(sliceSize, bytesPerGroup);
	processRange = cl::NDRange(sliceGroups * wgSize, CEIL_DIV(numOutputs, outputsPerThread));
	sliceSizeAligned = sliceGroups*bytesPerGroup;
	allocatedSliceSize = sliceSizeAligned;
	
	
	char params[300];
	snprintf(params, sizeof(params), "%s -DMAX_SLICE_SIZE=%zu -DNUM_OUTPUTS=%u -DOUTPUTS_PER_THREAD=%u -DOUTPUT_THREADS=%u -DVECT_WIDTH=%u -DCOL_GROUP_SIZE=%zu -DCOL_GROUP_ITERS=%u -DOUTPUT_GROUPING=%u -DSUBMIT_INPUTS=%u", oclVerArg, sliceSizeAligned, numOutputs, outputsPerThread, CEIL_DIV(numOutputs, outputsPerThread), infoShortVecSize, wgSize, groupIterations, outputGrouping, inputBatchSize);
	
	
#ifdef DUMP_ASM
	static bool dumped = false;
	if(!dumped) {
		dumped = true;
		char paramsDump[300];
		// nvidia CUDA doesn't seem to like -save-temps, so remove if on there
		snprintf(paramsDump, sizeof(paramsDump), "-save-temps=ocl_dump/ %s -DSLICE_SIZE=1048576 -DNUM_OUTPUTS=96 -DOUTPUTS_PER_THREAD=96 -DOUTPUT_THREADS=1 -DVECT_WIDTH=%u -DCOL_GROUP_SIZE=%zu -DCOL_GROUP_ITERS=2 -DOUTPUT_GROUPING=2 -DSUBMIT_INPUTS=2", oclVerArg, infoShortVecSize, wgSize);
		std::stringstream tmpStream;
		tmpStream << sourceStream.str();
		tmpStream << "\n#define KERNFN(f) gf16_kernel_##f\n";
		tmpStream << "#define numInputs " << inputBatchSize << "\n";
		if(kernelFuncs) tmpStream << kernelFuncs;
		tmpStream << "void kernel __attribute__((reqd_work_group_size(COL_GROUP_SIZE, 1, 1))) "
		"gf16_ocl_kernel(__global val_t* restrict dst, __global const val_t* restrict src, __global const ushort* restrict coeff  EX_TABLE_KARGS_DECL)\n";
		tmpStream << kernelCode << "\n";
		
		std::string tmpSource = tmpStream.str();
		cl::Program program(context, cl::Program::Sources(1, std::make_pair(tmpSource.data(), tmpSource.length())));
		try {
			program.build(std::vector<cl::Device>(1, device), paramsDump);
		} catch(cl::Error const& err) {
			if(err.err() == CL_BUILD_PROGRAM_FAILURE || err.err() == CL_COMPILE_PROGRAM_FAILURE || err.err() == CL_LINK_PROGRAM_FAILURE) {
				std::cerr << "OpenCL Build Failure: " << err.what() << "(" << err.err() << "); build log:" <<std::endl << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device) << std::endl;
			} else
				std::cerr << "OpenCL Build Error: " << err.what() << "(" << err.err() << ")" << std::endl;
			return false;
		}
		
		
		std::vector<char*> binaries = program.getInfo<CL_PROGRAM_BINARIES>();
		for(unsigned i=0; i<binaries.size(); i++) {
			std::ofstream dumpfile;
			std::stringstream filename;
			filename << "ocl_dump/program_" << i << ".bin";
			dumpfile.open(filename.str(), std::ios::out);
			dumpfile << binaries[i];
			dumpfile.close();
		}
	}
#endif
	
	// generate four kernels
	sourceStream << "#define KERNFN(f) gf16_kernel_last_##f\n";
	if(kernelFuncs) sourceStream << kernelFuncs;
	sourceStream << "void kernel __attribute__((reqd_work_group_size(COL_GROUP_SIZE, 1, 1))) "
	"gf16_ocl_kernel_last(__global val_t* restrict dst, __global const val_t* restrict src, __global const ushort* restrict coeff, const ushort numInputs  EX_TABLE_KARGS_DECL)\n";
	sourceStream << kernelCode << "\n";
	
	sourceStream << "#undef KERNFN\n#define KERNFN(f) gf16_kernel_##f\n";
	sourceStream << "#define numInputs " << inputBatchSize << "\n";
	if(kernelFuncs) sourceStream << kernelFuncs;
	sourceStream << "void kernel __attribute__((reqd_work_group_size(COL_GROUP_SIZE, 1, 1))) "
	"gf16_ocl_kernel(__global val_t* restrict dst, __global const val_t* restrict src, __global const ushort* restrict coeff  EX_TABLE_KARGS_DECL)\n";
	sourceStream << kernelCode << "\n";
	
	sourceStream << "#undef KERNFN\n#define KERNFN(f) gf16_kernel_first_##f\n";
	sourceStream << "#define MUL_ONLY\n";
	if(kernelFuncs) sourceStream << kernelFuncs;
	sourceStream << "void kernel __attribute__((reqd_work_group_size(COL_GROUP_SIZE, 1, 1))) "
	"gf16_ocl_kernel_first(__global val_t* restrict dst, __global const val_t* restrict src, __global const ushort* restrict coeff  EX_TABLE_KARGS_DECL)\n";
	sourceStream << kernelCode;
	
	sourceStream << "#undef numInputs\n";
	
	sourceStream << "#undef KERNFN\n#define KERNFN(f) gf16_kernel_only_##f\n";
	if(kernelFuncs) sourceStream << kernelFuncs;
	sourceStream << "void kernel __attribute__((reqd_work_group_size(COL_GROUP_SIZE, 1, 1))) "
	"gf16_ocl_kernel_only(__global val_t* restrict dst, __global const val_t* restrict src, __global const ushort* restrict coeff, const ushort numInputs  EX_TABLE_KARGS_DECL)\n";
	sourceStream << kernelCode;
	
	
	std::string tmpSource = sourceStream.str();
	cl::Program::Sources sources(cl::Program::Sources(1, std::make_pair(tmpSource.data(), tmpSource.length())));
	
	// compile OpenCL kernel
	cl::Program program(context, sources);
	try {
		program.build(std::vector<cl::Device>(1, device), params);
	} catch(cl::Error const& err) {
		if(err.err() == CL_BUILD_PROGRAM_FAILURE || err.err() == CL_COMPILE_PROGRAM_FAILURE || err.err() == CL_LINK_PROGRAM_FAILURE) {
			std::cerr << "OpenCL Build Failure: " << err.what() << "(" << err.err() << "); build log:" <<std::endl << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device) << std::endl;
		} else
			std::cerr << "OpenCL Build Error: " << err.what() << "(" << err.err() << ")" << std::endl;
		return false;
	}
	
	
	// variant kernels for first (mul only) & last (misaligned input count) iterations
	kernelMul = cl::Kernel(program, "gf16_ocl_kernel_first");
	kernelMulLast = cl::Kernel(program, "gf16_ocl_kernel_only");
	kernelMulAddLast = cl::Kernel(program, "gf16_ocl_kernel_last");
	kernelMulAdd = cl::Kernel(program, "gf16_ocl_kernel");
	
	
	// TODO: check if supports CPU shared memory and use host mem instead?
	// should probably check for dedicated memory to determine if transferring is needed
	for(int i=0; i<OCL_BUFFER_COUNT; i++) {
		buffer_input[i] = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, inputBatchSize*sliceSizeAligned);
		if(coeffAsLog) {
			buffer_coeffs[i] = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, inputBatchSize*sizeof(uint16_t));
			tmp_coeffs[i].resize(inputBatchSize);
		} else {
			buffer_coeffs[i] = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, inputBatchSize*numOutputs*sizeof(uint16_t));
			tmp_coeffs[i].resize(inputBatchSize*numOutputs);
		}
	}
	// TODO: need to consider CL_DEVICE_MAX_MEM_ALLOC_SIZE and perhaps break this into multiple allocations
	buffer_output = cl::Buffer(context, CL_MEM_READ_WRITE, numOutputs*sliceSizeAligned);
	
	workGroupRange = cl::NDRange(wgSize, 1);
	
	
	// attach arguments to kernels
	kernelMul.setArg(0, buffer_output);
	kernelMulLast.setArg(0, buffer_output);
	kernelMulAddLast.setArg(0, buffer_output);
	kernelMulAdd.setArg(0, buffer_output);
	
	extra_buffers.clear();
	
	// if we couldn't embed log tables directly into the source, transfer them now
	if(tblLog) {
		extra_buffers.push_back(cl::Buffer(context, CL_MEM_READ_ONLY, 65536*2));
		const cl::Buffer& buf = extra_buffers.back();
		queue.enqueueWriteBuffer(buf, CL_TRUE, 0, 65536*2, tblLog);
		
		kernelMul.setArg(3, buf);
		kernelMulAdd.setArg(3, buf);
		kernelMulLast.setArg(4, buf);
		kernelMulAddLast.setArg(4, buf);
		
		delete[] tblLog;
	}
	if(tblAntiLog) {
		extra_buffers.push_back(cl::Buffer(context, CL_MEM_READ_ONLY, tblAntiLogSize*2));
		const cl::Buffer& buf = extra_buffers.back();
		queue.enqueueWriteBuffer(buf, CL_TRUE, 0, tblAntiLogSize*2, tblAntiLog);
		
		kernelMul.setArg(4, buf);
		kernelMulAdd.setArg(4, buf);
		kernelMulLast.setArg(5, buf);
		kernelMulAddLast.setArg(5, buf);
		
		delete[] tblAntiLog;
	}
	
	return true;
}

