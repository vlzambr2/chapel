/*
 * Copyright 2020-2023 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Supports utility functions for operating with GPUs.

  .. warning::

    This module is unstable and its interface is subject to change in the
    future.

    GPU support is a relatively new feature to Chapel and is under active
    development.

    For the most up-to-date information about GPU support see the
    :ref:`technical note <readme-gpu>` about it.
*/
@unstable("The GPU module is unstable and its interface is subject to change in the future.")
module GPU
{
  use CTypes;
  use ChplConfig;

  pragma "codegen for CPU and GPU"
  extern proc chpl_gpu_write(const str : c_ptrConst(c_char)) : void;

  pragma "codegen for CPU and GPU"
  extern proc chpl_gpu_clock() : uint;

  pragma "codegen for CPU and GPU"
  extern proc chpl_gpu_printTimeDelta(
    msg : c_ptrConst(c_char), start : uint, stop : uint) : void;

  pragma "codegen for CPU and GPU"
  extern proc chpl_gpu_device_clock_rate(devNum : int(32)) : uint;

  /*
     This function is intended to be called from within a GPU kernel and is
     useful for debugging purposes.

     Currently using :proc:`~ChapelIO.write` to send output to ``stdout`` will
     make a loop ineligible for GPU execution; use :proc:`gpuWrite` instead.

     Currently this function will only work if values of type
     ``c_ptrConst(c_char)`` are passed.

     On NVIDIA GPUs the written values will be flushed to the terminal after
     the kernel has finished executing.  Note that there is a 1MB limit on the
     size of this buffer.
   */
  proc gpuWrite(const args ...?k) {
    // Right now this function will only work if passed arguments are of type
    // c_ptrConst(c_char).
    // I would prefer to do some string processing within the
    // function so I could pass in arguments other than C types.
    //
    // One thing I tried was changing the call to chpl_gpu_write
    // to look like this:
    //
    //    chpl_gpu_write((args[i] : string).c_str());
    //
    // Unfortunately that made things un-gpuizable as I believe
    // it ends up calling the constructor for string which
    // somewhere uses the "outside variable" "nil", which
    // fails our gpuization check.
    //
    // I also explored making `printf` an extern proc
    // and calling it directly but that resulted in this error:
    //   ptxas fatal   : Unresolved extern function 'printf
    for param i in 0..<k {
      chpl_gpu_write(args[i]);
    }
  }

  /*
     Pass arguments to :proc:`gpuWrite` and follow with a newline.
  */
  proc gpuWriteln(const args ...?k) {
    gpuWrite((...args), "\n":c_ptrConst(c_char));
  }

  /*
    Will halt execution at runtime if called from outside a GPU.  If used on
    first line in ``foreach`` or ``forall`` loop will also do a compile time
    check that the loop is eligible for execution on a GPU.
  */
  pragma "insert line file info"
  pragma "always propagate line file info"
  @deprecated(notes="the functional form of assertOnGpu() is deprecated. Please use the @assertOnGpu loop attribute instead.")
  inline proc assertOnGpu() {
    __primitive("chpl_assert_on_gpu", false);
  }

  /*
    Returns value of a per-multiprocessor counter that increments every clock cycle.
    This function is meant to be called to time sections of code within a GPU
    enabled loop.
  */
  proc gpuClock() : uint {
    return chpl_gpu_clock();
  }

  /*
    Prints 'msg' followed by the difference between 'stop' and 'start'. Meant to
    print the time elapsed between subsequent calls to 'gpuClock()'.
    To convert to seconds divide by 'gpuClocksPerSec()'
  */
  @chpldoc.nodoc
  proc gpuPrintTimeDelta(msg : c_ptrConst(c_char), start : uint, stop : uint) : void {
    chpl_gpu_printTimeDelta(msg, start, stop);
  }

  /*
    Returns the number of clock cycles per second of a GPU multiprocessor.
    Note: currently we don't support calling this function from within a kernel.
   */
  proc gpuClocksPerSec(devNum : int) {
    return chpl_gpu_device_clock_rate(devNum : int(32));
  }

  @chpldoc.nodoc
  type GpuAsyncCommHandle = c_ptr(void);

  /*
    Copy srcArr to dstArr, at least one array must be on a GPU; this function
    can be used for either communication to or from the GPU

    Returns a handle that can be passed to `waitGpuComm` to pause execution
    until completion of this asynchronous transfer
  */
  @chpldoc.nodoc
  proc asyncGpuComm(ref dstArr : ?t1, srcArr : ?t2) : GpuAsyncCommHandle
    where isArrayType(t1) && isArrayType(t2)
  {
    extern proc chpl_gpu_comm_async(dstArr : c_ptr(void), srcArr : c_ptr(void),
       n : c_size_t) : c_ptr(void);

    if(dstArr.size != srcArr.size) {
      halt("Arrays passed to asyncGpuComm must have the same number of elements. ",
        "Sizes passed: ", dstArr.size, " and ", srcArr.size);
    }
    return chpl_gpu_comm_async(c_ptrTo(dstArr), c_ptrToConst(srcArr),
      dstArr.size * numBytes(dstArr.eltType));
  }

  /*
     Wait for communication to complete, the handle passed in should be from the return
     value of a previous call to `asyncGpuComm`.
  */
  @chpldoc.nodoc
  proc gpuCommWait(gpuHandle : GpuAsyncCommHandle) {
    extern proc chpl_gpu_comm_wait(stream : c_ptr(void));

    chpl_gpu_comm_wait(gpuHandle);
  }

  /*
     Synchronize threads within a GPU block.
   */
  inline proc syncThreads() {
    __primitive("gpu syncThreads");
  }

  /*
    Allocate block shared memory, enough to store ``size`` elements of
    ``eltType``. Returns a :type:`CTypes.c_ptr` to the allocated array. Note that
    although every thread in a block calls this procedure, the same shared array
    is returned to all of them.

    :arg eltType: the type of elements to allocate the array for.

    :arg size: the number of elements in each GPU thread block's copy of the array.
   */
  inline proc createSharedArray(type eltType, param size): c_ptr(eltType) {
    if !__primitive("call and fn resolves", "numBits", eltType) {
      compilerError("attempting to allocate a shared array of '",
                    eltType : string,
                    "', which does not have a known size. Is 'numBits(",
                    eltType : string,
                    ")' supported?");
    }
    else if CHPL_GPU != "cpu" {
      const voidPtr = __primitive("gpu allocShared", numBytes(eltType)*size);
      return voidPtr : c_ptr(eltType);
    }
    else {
      // this works because the function is inlined.
      var alloc = new c_array(eltType, size);
      return c_ptrTo(alloc[0]);
    }
  }

  /*
    Set the block size for kernels launched on the GPU.
   */
  inline proc setBlockSize(blockSize: integral) {
    __primitive("gpu set blockSize", blockSize);
  }

  @chpldoc.nodoc
  proc canAccessPeer(loc1 : locale, loc2 : locale) : bool {
    extern proc chpl_gpu_can_access_peer(i : c_int, j : c_int) : bool;

    if(!loc1.isGpu() || !loc2.isGpu()) then
      halt("Non GPU locale passed to 'canAccessPeer'");
    const loc1Sid = chpl_sublocFromLocaleID(loc1.chpl_localeid());
    const loc2Sid = chpl_sublocFromLocaleID(loc2.chpl_localeid());

    return chpl_gpu_can_access_peer(loc1Sid, loc2Sid);
  }

  @chpldoc.nodoc
  proc setPeerAccess(loc1 : locale, loc2 : locale, shouldEnable : bool) {
    extern proc chpl_gpu_set_peer_access(
      i : c_int, j : c_int, shouldEnable : bool) : void;

    if(!loc1.isGpu() || !loc2.isGpu()) then
      halt("Non GPU locale passed to 'canAccessPeer'");
    const loc1Sid = chpl_sublocFromLocaleID(loc1.chpl_localeid());
    const loc2Sid = chpl_sublocFromLocaleID(loc2.chpl_localeid());

    chpl_gpu_set_peer_access(loc1Sid, loc2Sid, shouldEnable);
  }

  // ============================
  // Atomics
  // ============================

  // In the runtime library we have various type specific wrappers to call out
  // to the CUDA/ROCM atomic operation functions.  Note that the various
  // CUDA/ROCM atomic functions are defined in terms of the various "minimum
  // width" C types (like int, long, etc.) rather than fixed width types (like
  // int32_t, int64_t, etc.) thus we need to figure out which of these C types
  // makes the "best fit" for a corresponding Chapel type.
  private proc atomicExternTString(type T) param {
    param nb = if isNumeric(T) then numBits(T) else -1;
    param nbInt = numBits(c_int);
    param nbShort = numBits(c_short);
    param nbFloat = numBits(c_float);

    if nb == -1 then return "unknown";
    if isUint(T) && nb <= nbShort then return "short";
    if isInt(T)  && nb <= nbInt   then return "int";
    if isInt(T)                   then return "longlong";
    if isUint(T) && nb <= nbInt   then return "uint";
    if isUint(T)                  then return "ulonglong";
    if isReal(T) && nb <= nbFloat then return "float";
    if isReal(T)                  then return "double";
    return "unknown";
  }

  private proc externFunc(param opName : string, type T) param {
    return "chpl_gpu_atomic_" + opName + "_" + atomicExternTString(T);
  }

  // used to indicate that although a given atomic operation
  // is supported by other SDKs these particular ones are not
  // supported by ROCm.
  private proc invalidGpuAtomicOpForRocm(param s : string) param {
    select s { when
      "chpl_gpu_atomic_min_longlong",
      "chpl_gpu_atomic_max_longlong"
      do return true; }
    return false;
  }

  private proc validGpuAtomicOp(param s : string) param {
    select s { when
      "chpl_gpu_atomic_add_int",       "chpl_gpu_atomic_add_uint",
      "chpl_gpu_atomic_add_ulonglong", "chpl_gpu_atomic_add_float",
      "chpl_gpu_atomic_add_double",

      "chpl_gpu_atomic_sub_int", "chpl_gpu_atomic_sub_uint",

      "chpl_gpu_atomic_exch_int",       "chpl_gpu_atomic_exch_uint",
      "chpl_gpu_atomic_exch_ulonglong", "chpl_gpu_atomic_exch_float",

      "chpl_gpu_atomic_min_int",       "chpl_gpu_atomic_min_uint",
      "chpl_gpu_atomic_min_ulonglong", "chpl_gpu_atomic_min_longlong",

      "chpl_gpu_atomic_max_int",       "chpl_gpu_atomic_max_uint",
      "chpl_gpu_atomic_max_ulonglong", "chpl_gpu_atomic_max_longlong",

      "chpl_gpu_atomic_inc_uint",

      "chpl_gpu_atomic_dec_uint",

      "chpl_gpu_atomic_and_int",       "chpl_gpu_atomic_and_uint",
      "chpl_gpu_atomic_and_ulonglong",

      "chpl_gpu_atomic_or_int",       "chpl_gpu_atomic_or_uint",
      "chpl_gpu_atomic_or_ulonglong",

      "chpl_gpu_atomic_xor_int",       "chpl_gpu_atomic_xor_uint",
      "chpl_gpu_atomic_xor_ulonglong",

      "chpl_gpu_atomic_CAS_int",       "chpl_gpu_atomic_CAS_uint",
      "chpl_gpu_atomic_CAS_ulonglong"

      // Before adding support for this I would want better capabilities
      // to process CHPL_GPU (this is only supported when compiling for
      // CUDA with CC >= 7.0
      //"chpl_gpu_atomic_CAS_ushort"

      do return true; }
    return false;
  }

  private proc checkValidGpuAtomicOp(param opName, param rtFuncName, type T) param {
    if CHPL_GPU == "amd" && invalidGpuAtomicOpForRocm(rtFuncName) then
      compilerError("Chapel does not support atomic ", opName, " operation on type ", T : string,
        " when using 'CHPL_GPU=amd'.");

    if(!validGpuAtomicOp(rtFuncName)) then
      compilerError("Chapel does not support atomic ", opName, " operation on type ", T : string, ".");
  }

  private inline proc gpuAtomicBinOp(param opName : string, ref x : ?T, val : T) {
    param rtName = externFunc(opName, T);
    checkValidGpuAtomicOp(opName, rtName, T);

    pragma "codegen for GPU"
    extern rtName proc chpl_atomicBinOp(x, val) : T;

    __primitive("chpl_assert_on_gpu", false);
    return chpl_atomicBinOp(c_ptrTo(x), val);
  }

  private inline proc gpuAtomicTernOp(param opName : string, ref x : ?T, cmp : T, val : T) {
    param rtName = externFunc(opName, T);
    checkValidGpuAtomicOp(opName, rtName, T);

    pragma "codegen for GPU"
    extern rtName proc chpl_atomicTernOp(x, cmp, val) : T;

    __primitive("chpl_assert_on_gpu", false);
    return chpl_atomicTernOp(c_ptrTo(x), cmp, val);
  }

  /* When run on a GPU, atomically add 'val' to 'x' and store the result in 'x'.
     The operation returns the old value of x. */
  inline proc gpuAtomicAdd(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("add", x, val); }
  /* When run on a GPU, atomically subtract 'val' from 'x' and store the result in 'x'.
     The operation returns the old value of x. */
  inline proc gpuAtomicSub(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("sub", x, val); }
  /* When run on a GPU, atomically exchange the value stored in 'x' with 'val'.
     The operation returns the old value of x. */
  inline proc gpuAtomicExch( ref x : ?T, val : T) : T { return gpuAtomicBinOp("exch", x, val); }
  /* When run on a GPU, atomically compare 'x' and 'val' and store the minimum in 'x'.
     The operation returns the old value of x. */
  inline proc gpuAtomicMin(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("min", x, val); }
  /* When run on a GPU, atomically compare 'x' and 'val' and store the maximum in 'x'.
     The operation returns the old value of x. */
  inline proc gpuAtomicMax(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("max", x, val); }
  /* When run on a GPU, atomically increments x if the original value of x is
     greater-than or equal to val, if so the result is stored in 'x'. Otherwise x is set to 0.
     The operation returns the old value of x. */
  inline proc gpuAtomicInc(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("inc", x, val); }
  /* When run on a GPU, atomically determine if 'x' equals 0 or is greater than 'val'.
     If so store 'val' in 'x' otherwise decrement 'x' by 1. Otherwise x is set to val.
     The operation returns the old value of x. */
  inline proc gpuAtomicDec(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("dec", x, val); }
  /* When run on a GPU, atomically perform a bitwise 'and' operation on 'x' and 'val' and store
     the result in 'x'. The operation returns the old value of x. */
  inline proc gpuAtomicAnd(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("and", x, val); }
  /* When run on a GPU, atomically perform a bitwise 'or' operation on 'x' and 'val' and store
     the result in 'x'. The operation returns the old value of x. */
  inline proc gpuAtomicOr(   ref x : ?T, val : T) : T { return gpuAtomicBinOp("or", x, val); }
  /* When run on a GPU, atomically perform a bitwise 'xor' operation on 'x' and 'val' and store
     the result in 'x'. The operation returns the old value of x. */
  inline proc gpuAtomicXor(  ref x : ?T, val : T) : T { return gpuAtomicBinOp("xor", x, val); }

  /* When run on a GPU, atomically compare the value in 'x' and 'cmp', if they
     are equal store 'val' in 'x'. The operation returns the old value of x. */
  inline proc gpuAtomicCAS(  ref x : ?T, cmp : T, val : T) : T { return gpuAtomicTernOp("CAS", x, cmp, val); }

  // ============================
  // Reductions
  // ============================

  @chpldoc.nodoc
  config param gpuDebugReduce = false;

  private inline proc doGpuReduce(param op: string, const ref A: [] ?t) {
    if op != "sum" && op != "min" && op != "max" &&
       op != "minloc" && op != "maxloc" {

      compilerError("Unexpected reduction kind in doGpuReduce: ", op);
    }

    param cTypeName = if      t==int(8)   then "int8_t"
                      else if t==int(16)  then "int16_t"
                      else if t==int(32)  then "int32_t"
                      else if t==int(64)  then "int64_t"
                      else if t==uint(8)  then "uint8_t"
                      else if t==uint(16) then "uint16_t"
                      else if t==uint(32) then "uint32_t"
                      else if t==uint(64) then "uint64_t"
                      else if t==real(32) then "float"
                      else if t==real(64) then "double"
                      else                     "unknown";

    if cTypeName == "unknown" {
      compilerError("Arrays with ", t:string,
                    " elements cannot be reduced with gpu*Reduce functions");
    }

    proc retType(param op: string, const ref A: [] ?t) type {
      if isValReduce(op) then return A.eltType;
      if isValIdxReduce(op) then return (A.eltType, int);
      compilerError("Unknown reduction operation: ", op);
    }


    proc doCpuReduceHelp(param op: string, const ref A: [] ?t) {
      select op {
        when "sum" do return + reduce A;
        when "min" do return min reduce A;
        when "max" do return max reduce A;
        when "minloc" do return minloc reduce zip (A, A.domain);
        when "maxloc" do return maxloc reduce zip (A, A.domain);
        otherwise do compilerError("Unknown reduction operation: ", op);
      }
    }

    proc doCpuReduce(param op: string, const ref A: [] ?t) {
      if CHPL_GPU=="cpu" {
        return doCpuReduceHelp(op, A);
      }
      else {
        var res: retType(op, A);
        on here.parent {
          var HostArr = A;
          res = doCpuReduceHelp(op, HostArr);
        }
        return res;
      }
    }

    proc getExternFuncName(param op: string, type t) param: string {
      return "chpl_gpu_"+op+"_reduce_"+cTypeName;
    }

    proc isValReduce(param op) param {
      return op=="sum" || op=="min" || op=="max";
    }

    proc isValIdxReduce(param op) param {
      return op=="minloc" || op=="maxloc";
    }

    inline proc subReduceValIdx(param op, const baseOffset, ref accum, val) {
      // do some type checking to be safe
      compilerAssert(isTupleValue(val));
      if isTupleValue(accum) {
        compilerAssert(isValIdxReduce(op));
        compilerAssert(val[0].type == accum[0].type);

      }
      else {
        compilerAssert(isValReduce(op));
        compilerAssert(val[0].type == accum.type);
      }

      select op {
        when "sum" do accum += val[0];
        when "min" do accum = min(accum, val[0]);
        when "max" do accum = max(accum, val[0]);
        when "minloc" do
          if accum[0] > val[0] then accum = (val[0], val[1]+baseOffset);
        when "maxloc" do
          if accum[0] < val[0] then accum = (val[0], val[1]+baseOffset);
        otherwise do compilerError("Unknown reduction operation: ", op);
      }
    }

    iter offsetsThatCanFitIn32Bits(size: int) {
      // Engin: I've tried to get max(int(32)) to work as this bug is about CUB
      // using `int` as the size in the interface. However, getting close to
      // max(int(32)) also triggers the bug. So, I am choosing this as a
      // round/safe value for the time being.
      param chunkSize = 2_000_000_000;

      use Math only divCeil;
      const numChunks = divCeil(size, chunkSize);
      const standardChunkSize = divCeil(size, numChunks);

      if gpuDebugReduce then
        writeln("Will use ", numChunks, " chunks of size ", standardChunkSize);

      foreach chunk in 0..<numChunks {
        const start = chunk*standardChunkSize;
        const curChunkSize = if start+standardChunkSize <= size
                               then standardChunkSize
                               else size-start;
        if gpuDebugReduce then
          writef("Chunk %i: (start=%i, curChunkSize=%i) ", chunk, start,
                 curChunkSize);

        yield (start, curChunkSize);
      }
    }

    use CTypes;
    extern proc chpl_gpu_can_reduce(): bool;
    if !chpl_gpu_can_reduce() {
      return doCpuReduce(op, A);
    }

    // find the extern function we'll use
    param externFunc = getExternFuncName(op, t);
    extern externFunc proc reduce_fn(data, size, ref val, ref idx);

    // initialize the return value
    var ret;
    if isValReduce(op) {
      var retTmp: t;
      if op == "min" then retTmp = max(t);
      else if op == "max" then retTmp = min(t);
      ret = retTmp;
    }
    else if isValIdxReduce(op) {
      var retTmp: (t, int);
      if op == "minloc" then retTmp[0] = max(t);
      else if op == "maxloc" then retTmp[0] = min(t);
      ret = retTmp;
    }
    else {
      compilerError("Unknown reduction operation: ", op);
      ret = 0;
    }

    // perform the reduction
    const basePtr = c_ptrToConst(A);
    for (offset,size) in offsetsThatCanFitIn32Bits(A.size) {
      var curIdx: int(32) = -1; // should remain -1 for sum, min, max
      var curVal: t;
      reduce_fn(basePtr+offset, size, curVal, curIdx);
      subReduceValIdx(op, offset, ret, (curVal, curIdx));
      if gpuDebugReduce then
        writef(" (curVal=%i curIdx=%i ret=%?)\n", curVal, curIdx, ret);
    }

    if isValIdxReduce(op) then
      ret[1] += A.domain.first;

    return ret;
  }

  /*
    Add all elements of an array together on the GPU (that is, perform a
    sum-reduction). The array must be in GPU-accessible memory and the function
    must be called from outside a GPU-eligible loop. Only arrays with int, uint,
    and real types are supported. A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4]; // will be GPU-accessible
         writeln(gpuSumReduce(Arr)); // 15
       }
  */
  inline proc gpuSumReduce(const ref A: [] ?t) do return doGpuReduce("sum", A);

  /*
    Return the minimum element of an array on the GPU (that is, perform a
    min-reduction). The array must be in GPU-accessible memory and the function
    must be called from outside a GPU-eligible loop. Only arrays with int, uint,
    and real types are supported. A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4]; // will be GPU-accessible
         writeln(gpuMinReduce(Arr)); // 1
       }
  */
  inline proc gpuMinReduce(const ref A: [] ?t) do return doGpuReduce("min", A);

  /*
    Return the maximum element of an array on the GPU (that is, perform a
    max-reduction). The array must be in GPU-accessible memory and the function
    must be called from outside a GPU-eligible loop. Only arrays with int, uint,
    and real types are supported. A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4]; // will be GPU-accessible
         writeln(gpuMaxReduce(Arr)); // 5
       }
  */
  inline proc gpuMaxReduce(const ref A: [] ?t) do return doGpuReduce("max", A);

  /*
    For an array on the GPU, return a tuple with the value and the index of the
    minimum element (that is, perform a minloc-reduction). If there are multiple
    elements with the same minimum value, the index of the first one is
    returned. The array must be in GPU-accessible memory and the function must
    be called from outside a GPU-eligible loop.  Only arrays with int, uint, and
    real types are supported. A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4]; // will be GPU-accessible
         writeln(gpuMinLocReduce(Arr)); // (1, 2). Note that Arr[2]==1.
       }
  */
  inline proc gpuMinLocReduce(const ref A: [] ?t) do return doGpuReduce("minloc", A);

  /*
    For an array on the GPU, return a tuple with the value and the index of the
    maximum element (that is, perform a maxloc-reduction). If there are multiple
    elements with the same maximum value, the index of the first one is
    returned. The array must be in GPU-accessible memory and the function must
    be called from outside a GPU-eligible loop.  Only arrays with int, uint, and
    real types are supported. A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4]; // will be GPU-accessible
         writeln(gpuMaxLocReduce(Arr)); // (5, 3). Note that Arr[3]==5.
       }
  */
  inline proc gpuMaxLocReduce(const ref A: [] ?t) do return doGpuReduce("maxloc", A);



  // ============================
  // GPU Scans
  // ============================

  // The following functions are used to implement GPU scans. They are
  // intended to be called from a GPU locale.
  private import Math;
  private import BitOps;

  private param DefaultGpuBlockSize = 512;

  /*
    Calculates an exclusive prefix sum (scan) of an array on the GPU.
    The array must be in GPU-accessible memory and the function
    must be called from outside a GPU-eligible loop.
    Arrays of numeric types are supported.
    A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4]; // will be GPU-accessible
         gpuScan(Arr);
         writeln(Arr); // [0, 3, 5, 6, 11]
       }
  */
  proc gpuScan(ref gpuArr : [] ?t) where isNumericType(t) && !isComplexType(t) {
    if !here.isGpu()  then halt("gpuScan must be run on a gpu locale");
    if gpuArr.size==0 then return;
    if gpuArr.rank > 1 then compilerError("gpuScan only supports 1D arrays");

    // Use a simple algorithm for small arrays
    // TODO check the actual thread block size rather than 2*default
    if gpuArr.size <= DefaultGpuBlockSize*2 {
      // The algorithms only works for arrays that are size of a power of two.
      // In case it's not a power of two we pad it out with 0s
      const size = roundToPowerof2(gpuArr.size);
      if size == gpuArr.size {
        // It's already a power of 2 so we don't do copies back and forth
        singleBlockScan(gpuArr);
        return;
      }
      var arr : [0..<size] t;
      arr[0..<gpuArr.size] = gpuArr;

      singleBlockScan(arr);

      // Copy back
      gpuArr = arr[0..<gpuArr.size];
    } else {
      // We use a parallel scan algorithm for large arrays
      parallelArrScan(gpuArr);
    }
  }

  private proc singleBlockScan(ref gpuArr : [] ?t) {
    // Hillis Steele Scan is better if we can scan in
    // a single thread block
    // TODO check the actual thread block size rather than the default
    if gpuArr.size <= DefaultGpuBlockSize then
      hillisSteeleScan(gpuArr);
    else
      blellochScan(gpuArr);
  }

  private proc parallelArrScan(ref gpuArr : [] ?t) where isNumericType(t) && !isComplexType(t) {
    // Divide up the array into chunks of a reasonable size
    // For our default, we choose our default block size which is 512
    const scanChunkSize = DefaultGpuBlockSize;
    const numScanChunks = Math.divCeil(gpuArr.size, scanChunkSize);

    if numScanChunks == 1 {
      hillisSteeleScan(gpuArr);
      return;
    }

    // Allocate an accumulator array
    var gpuScanArr : [0..<numScanChunks] t;
    const low = gpuArr.domain.low; // https://github.com/chapel-lang/chapel/issues/22433
    const ceil = gpuArr.domain.high;

    // In parallel: For each chunk we do an in lane serial scan
    @assertOnGpu
    foreach chunk in 0..<numScanChunks {
      const start = low+chunk*scanChunkSize;
      const end = min(ceil, start+scanChunkSize-1);
      gpuScanArr[chunk] = gpuArr[end];  // Save the last element before the scan overwrites it
      serialScan(gpuArr, start, end);   // Exclusive scan in serial
      gpuScanArr[chunk] += gpuArr[end]; // Save inclusive scan in the scan Arr

    }

      // Scan the scanArr and we do it recursively
      gpuScan(gpuScanArr);

      @assertOnGpu
      foreach i in gpuArr.domain {
        // In propagate the right values from scanArr
        // to complete the global scan
        const offset : int = (i-low) / scanChunkSize;
        gpuArr[i] += gpuScanArr[offset];
      }
  }

  private proc roundToPowerof2(const x : uint) {
    // Powers of two only have the highest bit set.
    // Power of two minus one will have all bits set except the highest.
    // & those two together should give us 0;
    // Ex 1000 & 0111 = 0000
    if (x & (x - 1)) == 0 then
      return x; // x is already a power of 2
    // Not a power of two, so we pad it out
    // To the next nearest power of two
    const log_2_x = numBytes(uint)*8 - BitOps.clz(x); // get quick log for uint
    // Next highest nerest power of two is
    return 1 << log_2_x;
  }

  // This function requires that startIdx and endIdx are within the bounds of the array
  // it checks that only if boundsChecking is true (i.e. NOT with --fast or --no-checks)
  private proc serialScan(ref arr : [] ?t, startIdx = arr.domain.low, endIdx = arr.domain.high) {
    // Convert this count array into a prefix sum
    // This is the same as the count array, but each element is the sum of all previous elements
    // This is an exclusive scan
    // Serial implementation
    if boundsChecking then assert(startIdx >= arr.domain.low && endIdx <= arr.domain.high);
    // Calculate the prefix sum
    var sum : t = 0;
    for i in startIdx..endIdx {
      var temp : t = arr[i];
      arr[i] = sum;
      sum += temp;
    }
  }

  private proc hillisSteeleScan(ref arr : [] ?t) where isNumericType(t) && !isComplexType(t) {
    // Hillis Steele Scan
    // This is the same as the count array, but each element is the sum of all previous elements
    // Uses a naive algorithm that does O(nlogn) work
    // Hillis and Steele (1986)
    const x = arr.size;
    if x == 0 then return;
    if (x & (x - 1)) != 0 then {
      halt("Hillis Steele Scan only works for arrays of size a power of two.");
    }

    var offset = 1;
    while offset < arr.size {
        var arrBuffer = arr;
        const low = arr.domain.low; // https://github.com/chapel-lang/chapel/issues/22433
        @assertOnGpu
        foreach idx in offset..<arr.size {
          const i = idx + low;
          arr[i] = arrBuffer[i] + arrBuffer[i-offset];
        }
        offset = offset << 1;
    }

    // Change inclusive scan to exclusive
    var arrBuffer = arr;
    foreach i in arr.domain.low+1..arr.domain.high {
      arr[i] = arrBuffer[i-1];
    }
    arr[arr.domain.low] = 0;
  }

  private proc blellochScan(ref arr : [] ?t) where isNumericType(t) && !isComplexType(t) {
    // Blelloch Scan
    // This is the same as the count array, but each element is the sum of all previous elements
    // Uses a more efficient algorithm that does O(n) work
    // Blelloch (1990)

    const x = arr.size;
    if x == 0 then return;
    if (x & (x - 1)) != 0 then {
      halt("Blelloch Scan only works for arrays of size a power of two.");
    }


    const low = arr.domain.low; // https://github.com/chapel-lang/chapel/issues/22433

    // Up-sweep
    var offset = 1;
    while offset < arr.size {
      var arrBuffer = arr;
      const doubleOff = offset << 1;
      @assertOnGpu
      foreach idx in 0..<arr.size/(2*offset) {
        const i = idx*doubleOff + low;
        arr[i+doubleOff-1] = arrBuffer[i+offset-1] + arrBuffer[i+doubleOff-1];
      }
      offset = offset << 1;
    }

    // Down-sweep
    arr[arr.domain.high] = 0;
    offset = arr.size >> 1;
    while offset > 0 {
      var arrBuffer = arr;
      @assertOnGpu
      foreach idx in 0..<arr.size/(2*offset) {
        const i = idx*2*offset+low;
        const t = arrBuffer[i+offset-1];
        arr[i+offset-1] = arrBuffer[i+2*offset-1];
        arr[i+2*offset-1] = arr[i+2*offset-1] + t;
      }
      offset = offset >> 1;
    }
  }





  // ============================
  // GPU Sort
  // ============================

  private import Time;

  /*
    Sort an array on the GPU.
    The array must be in GPU-accessible memory and the function must
    be called from outside a GPU-eligible loop.
    Only arrays with uint eltType are supported.
    A simple example is the following:

     .. code-block:: chapel

       on here.gpus[0] {
         var Arr = [3, 2, 1, 5, 4] : uint; // will be GPU-accessible
         gpuSort(Arr);
         writeln(Arr); // [1, 2, 3, 4, 5]
       }
  */
  proc gpuSort(ref gpuInputArr : [] uint) {
    if !here.isGpu() then halt("gpuSort must be run on a gpu locale");

    if gpuInputArr.size == 0 then return;
    // Based on the inputArr size, get a chunkSize such that numChunks is on the order of thousands
    // TODO better heuristic here?
    var chunkSize = Math.divCeil(gpuInputArr.size, 2000);
    parallelRadixSort(gpuInputArr, bitsAtATime=8, chunkSize, noisy=false, distributed=false);
  }

  // We no doc it so we can test this independently to simulate all cases that can happen with sort
  @chpldoc.nodoc
  proc parallelRadixSort(ref gpuInputArr : [] uint, const bitsAtATime : int = 8, const chunkSize : int = 512, const noisy : bool = false,
                         const distributed : bool = false) { // The last argument is for multi GPU sort that is pending a patch before it can work

    // How many bits we sort at once based on the bitsAtATime
    const buckets = 1 << bitsAtATime; // 2^bitsAtATime, ex: 2^4 = 16 = 0b10000
    const bitMask = buckets - 1; // 2^bitsAtATime - 1, ex: 2^4 - 1 = 15 = 0b1111

    // The size of the chunks the input array is divided into for parallelism
    // Each chunk has it's own count table (hence it's own prefix sum)
    // Which we then intelligently combine together to produce a sorted output
    const numChunks = Math.divCeil(gpuInputArr.size,chunkSize);
    // TODO: make up some heuristic to chose a good chunkSize and numChunks?


    if noisy {
        writeln("Bits at a time: ", bitsAtATime);
        writeln("Buckets: ", buckets);
        writeln("Bit mask: ", bitMask);
        writeln("Chunk Size: ", chunkSize);
        writeln("Num Chunks: ", numChunks);
    }

    // Based on the number of chunks we're gonna divide this input into,
    // we create the prefixSum arrays, one for each chunk
    // And we only create it once
    // This was we can reuse it for each iteration of radix Sort
    var prefixSums : [0..<numChunks*buckets] uint;

    var timer : Time.stopwatch;
    // Ceiling on number of iterations based on max element in array
    timer.start();
    var maxVal = max(uint);
    // Reduce can be used to do this faster
    // We do this because the preferred ways below either don't work or take too long
    // But I'm leaving this as is due to https://github.com/chapel-lang/chapel/issues/22736
    // var maxVal = max reduce gpuInputArr;
    // var maxVal = gpuInputArr[0]; // Takes toooo long for GPU Arrays
    // for i in 1..<gpuInputArr.size {
    //   if gpuInputArr[i] > maxVal {
    //     maxVal = gpuInputArr[i];
    //   }
    // }
    timer.stop();
    if noisy then writeln("maxVal: ", maxVal);
    if noisy then writeln("Finding Max time: ", timer.elapsed());

    // Number of iterations is the number of bits in the max element divided by number of bits we sort at a time
    var exp = 0;
    while maxVal> 0 {
      if noisy then writeln("       Exp: ", exp);

      // Each chunk maintains it's own count array (here it's called prefixSum)
      // First we do the counts in parallel
      // After this we need to use the prefix sum to get a global offset
      // And finally we will use that offset to do the reorder/scatter step
      // in parallel

      // Count
      timer.clear();
      timer.start();
      if !distributed then
        parallelCount(prefixSums, gpuInputArr, exp, bitMask, chunkSize, numChunks);
      else {
        const numChunksPerGpu = Math.divCeil(numChunks, 2);
        distributedCount(prefixSums, gpuInputArr, exp, bitMask, numChunks, here.gpus.size);
      }
      timer.stop();

      if noisy then writeln("Parallel Count Time: ", timer.elapsed());
      if noisy then writeln("Prefix Sums after count: ", prefixSums);

      // Calculate global offsets for each chunk
      // I can calculate the global offset by simply doing a prefix sum over the array
      // See https://gpuopen.com/download/publications/Introduction_to_GPU_Radix_Sort.pdf
      // Figure 1 and equation 1 for explanation
      timer.clear();
      timer.start();
      gpuScan(prefixSums);
      timer.stop();
      if noisy then writeln("Parallel Scan Time: ", timer.elapsed());
      if noisy then writeln("Prefix Sums after scan: ", prefixSums);

      // Scatter
      // Now we have the offsets for each individual chunk
      // We can use those to scatter back into the output Array
      timer.clear();
      timer.start();
      parallelScatter(prefixSums, gpuInputArr, exp, bitMask, chunkSize, numChunks);
      timer.stop();
      if noisy then writeln("Parallel Scatter Time: ", timer.elapsed());
      if noisy then writeln("Array after scatter: ", gpuInputArr);

      // Increment the exp, decrement maxVal by shifting, and copy arrays
      exp += bitsAtATime;
      maxVal = maxVal >> bitsAtATime;
    }
  }

  private proc parallelCount(ref gpuCounts : [], ref gpuInputArr : [] uint, const exp : int,
                    const bitMask : int, const chunkSize : int,
                    const numChunks : int,  const numChunksThisGpu : int = numChunks,
                    const startChunk : int = 0,
                    const gpuId : int = 0, const resetCountsArray = true) {

    // Instead of using a nested array of arrays, use a simple 1D array of
    // size numChunks*buckets which is a column major representation
    // of the 2D array where each chunk has it's own array of buckets.
    // This way calculating the offset is just a prefix sum
    // And we can use strided iteration to work on them as if they were
    // 2D arrays.
    // writeln("Counting on gpu: ", gpuId);
    if resetCountsArray then
      gpuCounts = 0; // All 0s
    // writeln("Bounds for chunk: ", startChunk, " to ", startChunk+numChunksThisGpu-1);
    const arrSize = gpuInputArr.size;
    const low = gpuInputArr.domain.low; // https://github.com/chapel-lang/chapel/issues/22433

    @assertOnGpu
    foreach chunk in startChunk..#numChunksThisGpu {
      // Count for each chunk in parallel.
      const startIdx : int = (chunk:int)*chunkSize;
      const endIdx : int = min(arrSize,startIdx+chunkSize);
      for i in startIdx..<endIdx {
        const arrIdx = i+low;
        const tmp = ((gpuInputArr[arrIdx]>>exp) & bitMask):int;
        const tmp2 = chunk+(numChunks*tmp);
        gpuCounts[tmp2] += 1;
      }
    }
  }

  // Multi GPU Experimental function
  // This won't work for now since it wasn't updated with the modularization of radix sort
  private proc distributedCount(ref counts : [], ref gpuInputArr : [] uint, const exp : int, const bitMask : int,
                        const numChunks : int, const numGpus : int ) {
    // Counts should be all 0s
    // counts = 0;

    const numChunksPerGpu = Math.divCeil(numChunks, numGpus);
    const numChunksPerGpuLast = numChunks - (numChunksPerGpu * (numGpus-1));
    var firstRunReset = true;

    // Distribute the counts across the GPUs
    for gpu in 0..<numGpus {
      const numChunksThisGpu = if gpu == numGpus-1 then numChunksPerGpuLast else numChunksPerGpu;
      const startChunk = gpu*numChunksPerGpu;

      // Distributing the counts across the GPUs
      // Each GPU works on a subset of chunks from the array
      // Since each chunk already gets a slot in the counts array
      // We will pass the entire counts array to each GPU
      // But each GPU will only work on a subset of the counts array and not care about the rest
      parallelCount(counts, gpuInputArr, exp, bitMask, numChunks, numChunksThisGpu, startChunk, gpu, firstRunReset);
      // writeln(" Count: ", thisCounts);
      // This works because counts array is all 0s and
      // we're only adding to it. The parallelCount function leaves the
      // rest of the array untouched (i.e. 0s) so for 2 GPUs the add might be:
      // counts1 : 1 0 | 3 0 | 2 0 | 32 0
      // counts2 : 0 3 | 0 2 | 0 12 | 0 2
      // counts      : 1 3 | 3 2 | 2 12 | 32 2
      // I think this is not a race condition for the same reason
      firstRunReset = false; // This is a hacky solution


      // In an ideal world I would like to use a + reduce intent here such that each iteration gets it's own
      // copy of the counts array and it is accumulated into the main counts array
      // And the loop should be a coforall instead of a foreach
      // But I could not get that to work/ don't know how that works
    }
  }

  private proc parallelScatter(ref gpuOffsets : [], ref gpuInputArr : [] uint,
                       const exp : int, const bitMask : int,
                       const chunkSize : int, const numChunks : int) {

    var gpuOutputArr : gpuInputArr.type;
    const arrSize = gpuInputArr.size;
    const low = gpuInputArr.domain.low; // https://github.com/chapel-lang/chapel/issues/22433

    @assertOnGpu
    foreach chunk in 0..<numChunks {
      // Count for each chunk in parallel.
      const startIdx : int = (chunk:int)*chunkSize;
      const endIdx : int = min(arrSize,startIdx+chunkSize);
      for i in startIdx..<endIdx {
        const arrIdx = i+low;
        const tmp = ((gpuInputArr[arrIdx]>>exp) & bitMask):int; // Where in the counts array to look
        const tmp2 = chunk+(numChunks*tmp); // Index into the offsets array
        const tmp3 = gpuOffsets[tmp2]:int; // Index into the output array
        // This may happen when gpuInputArr.size%chunkSize!=0
        // if tmp3>=gpuInputArr.size then continue;
        gpuOutputArr[low+tmp3] = gpuInputArr[arrIdx];
        gpuOffsets[tmp2] += 1;
      }
    }
    gpuInputArr = gpuOutputArr;
  }

}
