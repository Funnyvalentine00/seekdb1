/* Minimal CUDA->HIP shim for compiling upstream raft headers under HIP.
 * Only the runtime symbols used by raft/core/{memory_type,device_mdspan}.hpp
 * and raft/util/cuda_rt_essentials.hpp are mapped; everything else resolves
 * through hip/hip_runtime.h macros. */
#pragma once
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>

typedef hipError_t cudaError_t;
typedef hipStream_t cudaStream_t;
typedef hipEvent_t cudaEvent_t;
typedef hipDeviceProp_t cudaDeviceProp;
typedef hipMemcpyKind cudaMemcpyKind;
typedef hipPointerAttribute_t cudaPointerAttributes;
typedef hipFuncAttributes cudaFuncAttributes;

#define cudaPointerGetAttributes hipPointerGetAttributes
#define cudaMemoryTypeHost hipMemoryTypeHost
#define cudaMemoryTypeDevice hipMemoryTypeDevice
#define cudaMemoryTypeManaged hipMemoryTypeManaged
#define cudaMemoryTypeUnregistered hipMemoryTypeUnregistered
#define cudaFuncGetAttributes hipFuncGetAttributes

#define cudaSuccess hipSuccess
#define cudaErrorNotReady hipErrorNotReady
#define cudaGetLastError hipGetLastError
#define cudaPeekAtLastError hipPeekAtLastError
#define cudaGetErrorName hipGetErrorName
#define cudaGetErrorString hipGetErrorString
#define cudaStreamSynchronize hipStreamSynchronize
#define cudaEventSynchronize hipEventSynchronize
#define cudaGetDeviceCount hipGetDeviceCount
#define cudaSetDevice hipSetDevice
#define cudaGetDevice hipGetDevice
#define cudaGetDeviceProperties hipGetDeviceProperties
#define cudaDeviceGetAttribute hipDeviceGetAttribute
#define cudaDeviceSynchronize hipDeviceSynchronize
#define cudaMalloc hipMalloc
#define cudaFree hipFree
#define cudaMemset hipMemset
#define cudaMemsetAsync hipMemsetAsync
#define cudaMemcpy hipMemcpy
#define cudaMemcpyAsync hipMemcpyAsync
#define cudaMemcpy2DAsync hipMemcpy2DAsync
#define cudaHostMalloc hipHostMalloc
#define cudaHostAlloc hipHostAlloc
#define cudaHostFree hipHostFree
#define cudaHostRegister hipHostRegister
#define cudaHostUnregister hipHostUnregister
#define cudaStreamCreate hipStreamCreate
#define cudaStreamCreateWithFlags hipStreamCreateWithFlags
#define cudaStreamDestroy hipStreamDestroy
#define cudaEventCreate hipEventCreate
#define cudaEventCreateWithFlags hipEventCreateWithFlags
#define cudaEventDestroy hipEventDestroy
#define cudaEventRecord hipEventRecord
#define cudaEventElapsedTime hipEventElapsedTime

#define cudaMemcpyHostToHost hipMemcpyHostToHost
#define cudaMemcpyHostToDevice hipMemcpyHostToDevice
#define cudaMemcpyDeviceToHost hipMemcpyDeviceToHost
#define cudaMemcpyDeviceToDevice hipMemcpyDeviceToDevice
#define cudaMemcpyDefault hipMemcpyDefault

#define cudaHostRegisterDefault hipHostRegisterDefault
#define cudaHostRegisterMapped hipHostRegisterMapped
#define cudaHostAllocDefault hipHostAllocDefault
#define cudaHostAllocMapped hipHostAllocMapped
#define cudaHostAllocWriteCombined hipHostAllocWriteCombined
#define cudaStreamNonBlocking hipStreamNonBlocking
#define cudaDeviceWarpSize hipDeviceWarpSize
