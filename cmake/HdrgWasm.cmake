# SPDX-License-Identifier: Apache-2.0 OR MIT
# Emscripten builds: {scalar, SIMD128} x {single thread, pthreads}, and SIMD128
# with WebGPU (HDRG_WASM_WEBGPU: the emdawnwebgpu port of Emscripten, JSPI, a single thread: the
# GPU's waits suspend the calling export and return to the event loop; node runs it with
# --experimental-wasm-jspi).
# HDRG_WITH_SIMD selects SIMD128: -msimd128 for every target, the dependencies included (never
# relaxed SIMD: its results depend on the hardware). HDRG_WASM_THREADS selects pthreads: the
# pool's workers are created up front (PTHREAD_POOL_SIZE), since the pool waits for them to
# start and a worker created later would need the event loop of the waiting thread.
include_guard(GLOBAL)

# programs run by node (tests, tools): the host file system, a 1 MB stack, the exit code
function(hdrg_wasm_node_program target)
    if(NOT EMSCRIPTEN)
        return()
    endif()
    target_link_options(${target} PRIVATE
        -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB)
endfunction()

if(NOT EMSCRIPTEN)
    return()
endif()

option(HDRG_WASM_THREADS "Emscripten: the pthreads build (browsers need crossOriginIsolated)" OFF)
option(HDRG_WASM_WEBGPU "Emscripten: WebGPU in the browser (emdawnwebgpu, JSPI; single thread)"
       OFF)

if(HDRG_WASM_WEBGPU)
    if(HDRG_WASM_THREADS)
        message(FATAL_ERROR "HDRG_WASM_WEBGPU is single-threaded (the GPU waits through JSPI)")
    endif()
    # setjmp / longjmp (libpng, jpegli) by Wasm exceptions: JSPI cannot suspend through the JS
    # frames of the emulated invoke_* calls
    add_compile_options(--use-port=emdawnwebgpu -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
    add_link_options(--use-port=emdawnwebgpu -sJSPI -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
    list(APPEND CMAKE_CROSSCOMPILING_EMULATOR --experimental-wasm-jspi)
endif()

if(HDRG_WITH_SIMD)
    add_compile_options(-msimd128)
endif()

if(HDRG_WASM_THREADS)
    add_compile_options(-pthread)
    # memory growth with pthreads only slows JS reading the heap (the module copies the output once)
    add_link_options(-pthread -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency
        -sDEFAULT_PTHREAD_STACK_SIZE=1MB -Wno-pthreads-mem-growth)
endif()
