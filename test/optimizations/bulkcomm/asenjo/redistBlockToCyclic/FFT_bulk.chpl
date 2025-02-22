use CTypes, Math;

proc copyBtoC(A:[], B:[])
{
  A._value.copyBtoC(B);
}

proc copyCtoB(A:[], B:[])
{
  A._value.copyCtoB(B);
}

proc BlockArr.copyBtoC(B)
{
  coforall loc in Locales do on loc
  {
    param stridelevels=1;
    var dststrides:[1..#stridelevels] c_size_t;
    var srcstrides: [1..#stridelevels] c_size_t;
    var count: [1..#(stridelevels+1)] c_size_t;
    var lid=loc.id;

    var numLocales: int(32)=dom.dist.targetLocDom.dim(0).size:int(32);
    var n:int(32)=dom.dist.boundingBox.dim(0).size:int(32);
    var src = locArr[lid].myElems._value.theData;

    dststrides[1]=1;
    srcstrides[1]=numLocales.safeCast(c_size_t);

    var dststr=dststrides._value.theData;
    var srcstr=srcstrides._value.theData;
    var cnt=count._value.theData;

    //Domain size (n) and first index (arrayini)

    var arrayini:int(32)=dom.dsiLow:int(32);

    //writeln("Domain: ",dom.whole.dims());

    //a,b: first and last global indices in each locale
    var a: int(32)=dom.locDoms[lid].myBlock.lowBound:int(32);
    var b: int(32)=dom.locDoms[lid].myBlock.highBound:int(32);
    var blksize=b-a+1;
    //writeln("Locale", here.id," : blksize ",blksize," subblock first index  a ",a,

    var t1,t2: real;
    var schunkini: int;
    var chunksize: int(32);
    //Loop of strided chunk sends (chpl_comm_put_strd)
    for k in 0..#numLocales do {

      var dst=(lid+k)%numLocales;

      //Compute the chunk first index in src locale to be send to locale dst.
      if (dst-a%numLocales+arrayini)<0 then schunkini=a+numLocales +(dst-a%numLocales+arrayini);
      else schunkini=a+(dst-a%numLocales+arrayini)%numLocales;

      //Compute chunk size
      if (schunkini+(blksize/numLocales)*numLocales>b) then chunksize=blksize/numLocales;
      else chunksize=blksize/numLocales+1;

      //var destr = privB.locArr[dst].myElems._value.theData;
      var destr = B._value.locArr[dst].myElems._value.theData;
      count[1]=1;
      count[2]=chunksize.safeCast(c_size_t);

      __primitive("chpl_comm_put_strd",
		  __primitive("array_get",destr,
			      B._value.locArr[dst].myElems._value.getDataIndex(schunkini)),
		  __primitive("array_get",dststr,dststrides._value.getDataIndex(1)),
		  dst, c_sublocid_any,
		  __primitive("array_get",src,
			      locArr[lid].myElems._value.getDataIndex(schunkini)),
		  __primitive("array_get",srcstr,srcstrides._value.getDataIndex(1)),
		  __primitive("array_get",cnt, count._value.getDataIndex(1)),
		  stridelevels);

    } // end for dst
  } // end coforall loc
} // end proc

proc  BlockArr.copyCtoB(B)
{

  coforall loc in Locales do on loc
  {
    param stridelevels=1;
    var dststrides:[1..#stridelevels] c_size_t;
    var srcstrides: [1..#stridelevels] c_size_t;
    var count: [1..#(stridelevels+1)] c_size_t;
    var lid=loc.id;
    var numLocales: int=dom.dist.targetLocDom.dim(0).size;
    var n:int(32)=dom.dist.boundingBox.dim(0).size:int(32);

    var dststr=dststrides._value.theData;
    var srcstr=srcstrides._value.theData;
    var cnt=count._value.theData;

    //On each locale (src) we compute the chunk that goes to each dst
    var num: int;
    var schunkini: int;
    var chunksize: int;
    var a: int(32)=dom.locDoms[lid].myBlock.lowBound:int(32);
    var b: int(32)=dom.locDoms[lid].myBlock.highBound:int(32);
    num=b-a+1;

    var src = locArr[lid].myElems._value.theData;
    var arrayini:int(32)=dom.dsiLow:int(32);

    var t,t1,t2: real;
    t=0.0;
    //Compute the starting index of the src chunk with destination dst

    for dst in 0..#numLocales do {
      if (dst-a%numLocales+arrayini)<0 then schunkini=a+numLocales+(dst-a%numLocales+arrayini);
      else schunkini=a+(dst-a%numLocales+arrayini)%numLocales;

      //Compute the size of the chunk. The number of elements of src subarray is num
      if (schunkini+(num/numLocales)*numLocales>b) then chunksize=num/numLocales;
      else chunksize=num/numLocales+1;

      var destr = B._value.locArr[dst].myElems._value.theData;
      dststrides[1]=numLocales:c_size_t;
      srcstrides[1]=1;
      count[1]=1;
      count[2]=chunksize:c_size_t;

      __primitive("chpl_comm_get_strd",
		  __primitive("array_get",src,
                              locArr[lid].myElems._value.getDataIndex(schunkini)),
		  __primitive("array_get",dststr,dststrides._value.getDataIndex(1)),
		  dst, c_sublocid_any,
		  __primitive("array_get",destr,
			      B._value.locArr[dst].myElems._value.getDataIndex(schunkini)),
		  __primitive("array_get",srcstr,srcstrides._value.getDataIndex(1)),
		  __primitive("array_get",cnt,count._value.getDataIndex(1)),
		  stridelevels);
    } //end for dst
  }//end for loc
} //end proc copyCtoB


/* This implementation of the FFT benchmark uses radix-4 butterflies
   and is divided into two main phases: one which uses a Block
   distribution and the second which uses a Cyclic distribution.  When
   run on 4**k locales, this guarantees that each butterfly will only
   access local data.  In an optimized implementation, this should
   cause most of the communication to occur when copying the vector
   between Block and Cyclic storage formats.
*/

//
// Use standard modules for Bit operations, Random numbers, Timing, and
// Block and Cyclic distributions
//
use BitOps, Random, Time, BlockDist, CyclicDist;

//
// Use shared user module for computing HPCC problem sizes
//
use HPCCProblemSize;

const radix = 4;               // the radix of this FFT implementation

const numVectors = 2;          // the number of vectors to be stored
type elemType = complex(128);  // the element type of the vectors
type idxType = int(64);        // the index type of the vectors

//
// A configuration constant defining log2(problem size) -- n -- and a
// constant defining the problem size itself -- m
//
config const n = 8;//computeProblemSize(numVectors, elemType, returnLog2 = true);
const m = 2**n;

//
// Configuration constants defining the epsilon and threshold values
// used to verify the result
//
config const epsilon = 2.0 ** -51.0,
  threshold = 16.0;

//
// Configuration constants to indicate whether or not to use a
// pseudo-random seed (based on the clock) or a fixed seed; and to
// specify the fixed seed explicitly
//
config const useRandomSeed = true;

//
// Configuration constants to control what's printed -- benchmark
// parameters, input and output arrays, and/or statistics
//
config const printParams = false,
  printArrays = false,
  printStats = false;

//
// The program entry point
//
proc main() {
  printConfiguration();          // print the problem size

  //
  // This implementation assumes 4**k locales due to its assertion that
  // all butterflies are local to a given locale
  //
  assert(4**log4(numLocales) == numLocales,
         "numLocales must be a power of 4 for this fft implementation");

  //
  // TwiddleDom describes the index set used to define the vector of
  // twiddle values and is a 1D domain indexed by 64-bit ints from 0
  // to m/4-1 stored using a block distribution.
  // Twiddles is the vector of twiddle values.
  //
  const TwiddleDom:
    domain(1, idxType) dmapped blockDist(boundingBox={0..m/4-1}) = {0..m/4-1};
  var Twiddles: [TwiddleDom] elemType;

  //
  // ProblemSpace describes the abstract problem space used for the
  // FFT benchmark: the indices 0..m-1
  //
  const ProblemSpace = {0..m-1};

  //
  // BlkDom defines a Block-distributed problem space and is used to
  // define the vectors z (used to store the input vector) and ZBlk
  // (used for the first half of the FFT phases).
  //
  const BlkDom:
    domain(1, idxType) dmapped blockDist(boundingBox=ProblemSpace) = ProblemSpace;
  var Zblk, z: [BlkDom] elemType;

  //
  // CycDom defines the Cyclic-distributed problem space and is used
  // to define the Zcyc vector, used for the second half of the FFT
  // phases.
  //
  const CycDom:
    domain(1, idxType) dmapped cyclicDist(startIdx=0:idxType) = ProblemSpace;

  var Zcyc: [CycDom] elemType;

  initVectors(Twiddles, z);            // initialize twiddles and input vector z
  var t1,t2,T1,T2,T3,T4: real;
  const startTime = timeSinceEpoch().totalSeconds();  // capture the start time
  [(a,b) in zip(Zblk, z)] a = conj(b);      // store the conjugate of z in Zblk

  //Comm y tieme bitReverse
  t1=timeSinceEpoch().totalSeconds();
  bitReverseShuffle(Zblk);                // permute Zblk
  t2=timeSinceEpoch().totalSeconds();
  T1=t2-t1;

  //Comm and Time dfft
  t1=timeSinceEpoch().totalSeconds();
  dfft(Zblk, Twiddles, cyclicPhase=false); // compute the DFFT, block phases
  t2=timeSinceEpoch().totalSeconds();
  T2=t2-t1;

  //Comm and Time first forall
  t1=timeSinceEpoch().totalSeconds();
  copyBtoC(Zblk,Zcyc);
  t2=timeSinceEpoch().totalSeconds();
  T3=t2-t1;

  /*
    writeln ("Validation copy_blk_cyc");
    var e:elemType;
    for i in [1..n] do e+=Zblk[i]-Zcyc[i];
    writeln("ERROR = ",e);
    if (e==0.0) then writeln("Correct"); else writeln("Wrong!");
  */
  t1=timeSinceEpoch().totalSeconds();
  dfft(Zcyc, Twiddles, cyclicPhase=true); // compute the DFFT, cyclic phases
  t2=timeSinceEpoch().totalSeconds();
  T2=T2+t2-t1;

  t1=timeSinceEpoch().totalSeconds();
  //    forall (b, c) in zip(Zblk, Zcyc) do        // copy vector back to Block storage
  //   b = c;
  copyCtoB(Zblk,Zcyc);
  t2=timeSinceEpoch().totalSeconds();
  T4=t2-t1;

  const execTime = timeSinceEpoch().totalSeconds() - startTime;     // store the elapsed time
  //  writeln("bitReverse Time = ",T1);
  //  writeln("dffts Time = ",T2," copyBtoC time= ",T3, " copyCtoB time= ",T4);

  const validAnswer = verifyResults(z, Zblk, Zcyc, Twiddles); // validate answer
  printResults(validAnswer, execTime);               // print the results
}

//
// compute the discrete fast Fourier transform of a vector A declared
// over domain ADom using twiddle vector W
//
proc dfft(ref A: [?ADom], W, cyclicPhase) {
  const numElements = A.size;
  //
  // loop over the phases of the DFT sequentially using custom
  // iterator genDFTStrideSpan that yields the stride and span for
  // each bank of butterfly calculations
  //
  for (str, span) in genDFTStrideSpan(numElements, cyclicPhase) {
    //
    // loop in parallel over each of the banks of butterflies with
    // shared twiddle factors, zippering with the unbounded range
    // 0.. to get the base twiddle indices
    //
    forall (bankStart, twidIndex) in zip(ADom by 2*span, 0..) {
      //
      // compute the first set of multipliers for the low bank
      //
      var wk2 = W(twidIndex),
	wk1 = W(2*twidIndex),
	wk3 = (wk1.re - 2 * wk2.im * wk1.im,
	       2 * wk2.im * wk1.re - wk1.im):elemType;
      //
      // loop in parallel over the low bank, computing butterflies
      // Note: lo..#num         == lo, lo+1, lo+2, ..., lo+num-1
      //       lo.. by str #num == lo, lo+str, lo+2*str, ... lo+(num-1)*str
      //
      forall lo in bankStart..#str do
        on ADom.distribution.idxToLocale(lo) do
          local do butterfly(wk1, wk2, wk3, A.localSlice(lo..by str #radix));

      //
      // update the multipliers for the high bank
      //
      wk1 = W(2*twidIndex+1);
      wk3 = (wk1.re - 2 * wk2.re * wk1.im,
             2 * wk2.re * wk1.re - wk1.im):elemType;
      wk2 *= 1.0i;

      //
      // loop in parallel over the high bank, computing butterflies
      //
      forall lo in bankStart+span..#str do
        on ADom.distribution.idxToLocale(lo) do
          local do butterfly(wk1, wk2, wk3, A.localSlice(lo.. by str #radix));
    }
  }

  if cyclicPhase {
    //
    // Do the last set of butterflies...
    //
    const str = radix**log4(numElements-1);
    //
    // ...using the radix-4 butterflies with 1.0 multipliers if the
    // problem size is a power of 4
    //
    if (str*radix == numElements) {
      forall lo in 0..#str do
        on ADom.distribution.idxToLocale(lo) do
          local do butterfly(1.0, 1.0, 1.0, A.localSlice(lo.. by str # radix));
    }
    //
    // ...otherwise using a simple radix-2 butterfly scheme
    //
    else
      forall lo in 0..#str with (ref A) do
        on ADom.distribution.idxToLocale(lo) do
          local {
            const a = A(lo),
	      b = A(lo+str);
            A(lo)     = a + b;
            A(lo+str) = a - b;
          }
  }
}

//
// this is the radix-4 butterfly routine that takes multipliers wk1,
// wk2, and wk3 and a 4-element array (slice) A.
//
proc butterfly(wk1, wk2, wk3, ref X:[?D]) {
  const i0 = D.lowBound,
        i1 = i0 + D.stride,
        i2 = i1 + D.stride,
        i3 = i2 + D.stride;
  var x0 = X(i0) + X(i1),
      x1 = X(i0) - X(i1),
      x2 = X(i2) + X(i3),
      x3rot = (X(i2) - X(i3))*1.0i;

  X(i0) = x0 + x2;                   // compute the butterfly in-place on X
  x0 -= x2;
  X(i2) = wk2 * x0;
  x0 = x1 + x3rot;
  X(i1) = wk1 * x0;
  x0 = x1 - x3rot;
  X(i3) = wk3 * x0;
}

//
// this iterator generates the stride and span values for the phases
// of the DFFT simply by yielding tuples: (radix**i, radix**(i+1))
//
iter genDFTStrideSpan(numElements, cyclicPhase) {
  const (start, end) = if !cyclicPhase then (1, numLocales:idxType)
    else (numLocales, numElements-1);
  var stride = start;
  for i in log4(start)+1..log4(end):int {
    const span = stride * radix;
    yield (stride, span);
    stride = span;
  }
}

//
// Print the problem size
//
proc printConfiguration() {
  if (printParams) {
    if (printStats) then printLocalesTasks();
    printProblemSize(elemType, numVectors, m);
  }
}


//
// Initialize the twiddle vector and random input vector and
// optionally print them to the console
//
proc initVectors(ref Twiddles, ref z) {
  computeTwiddles(Twiddles);
  bitReverseShuffle(Twiddles);

  if useRandomSeed
    then fillRandom(z);
    else fillRandom(z, 314159265);

  if (printArrays) {
    writeln("After initialization, Twiddles is: ", Twiddles, "\n");
    writeln("z is: ", z, "\n");
  }
}

//
// Compute the twiddle vector values
//
proc computeTwiddles(ref Twiddles) {
  const numTwdls = Twiddles.size,
    delta = 2.0 * atan(1.0) / numTwdls;

  Twiddles(0) = 1.0;
  Twiddles(numTwdls/2) = let x = cos(delta * numTwdls/2)
    in (x, x): elemType;
  forall i in 1..numTwdls/2-1 with (ref Twiddles) {
    const x = cos(delta*i),
      y = sin(delta*i);
    Twiddles(i)            = (x, y): elemType;
    Twiddles(numTwdls - i) = (y, x): elemType;
  }
}

//
// Perform a permutation of the argument vector by reversing the bits
// of the indices
//
proc bitReverseShuffle(ref Vect: [?Dom]) {
  const numBits = log2(Vect.size),
    Perm: [Dom] Vect.eltType = [i in Dom] Vect(bitReverse(i, revBits=numBits));
  Vect = Perm;
}

//
// Reverse the low revBits bits of val
//
proc bitReverse(val: ?valType, revBits = 64) {
  param mask = 0x0102040810204080;
  const valReverse64 = bitMatMultOr(mask, bitMatMultOr(val:uint(64), mask)),
    valReverse = rotl(valReverse64, revBits);
  return valReverse: valType;
}

//
// Compute the log base 4 of x
//
proc log4(x) do return logBasePow2(x, 2);

	     //
	     // verify that the results are correct by reapplying the dfft and then
	     // calculating the maximum error, comparing against epsilon
	     //
	     proc verifyResults(ref z, ref Zblk, ref Zcyc, Twiddles) {
	       if (printArrays) then writeln("After FFT, Z is: ", Zblk, "\n");

	       [z in Zblk] z = conj(z) / m;
	       bitReverseShuffle(Zblk);
	       dfft(Zblk, Twiddles, cyclicPhase=false);
	       forall (b, c) in zip(Zblk, Zcyc) do
		 c = b;
	       dfft(Zcyc, Twiddles, true);
	       forall (b, c) in zip(Zblk, Zcyc) do
		 b = c;

	       if (printArrays) then writeln("After inverse FFT, Z is: ", Zblk, "\n");

	       var maxerr = max reduce sqrt((z.re - Zblk.re)**2 + (z.im - Zblk.im)**2);
	       maxerr /= (epsilon * n);
	       if (printStats) then writeln("error = ", maxerr);

	       return (maxerr < threshold);
	     }

//
// print out sucess/failure, the timing, and the Gflop/s value
//
proc printResults(successful, execTime) {
  writeln("Validation: ", if successful then "SUCCESS" else "FAILURE");
  if (printStats) {
    writeln("Execution time = ", execTime);
    writeln("Performance (Gflop/s) = ", 5 * (m * n / execTime) * 1e-9);
  }
}
